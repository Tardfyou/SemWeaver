#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Checkers/Taint.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/Environment.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramState.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramStateTrait.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/SymExpr.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/StmtVisitor.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/StaticAnalyzer/Checkers/utility.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Type.h"
#include "llvm/ADT/SmallPtrSet.h"

using namespace clang;
using namespace ento;
using namespace taint;

// Program state: Set of metadata_dst objects (their canonical regions) that are
// known to be refcounted via dst_hold/skb_dst_set on this path.
REGISTER_SET_WITH_PROGRAMSTATE(RefcountedMdSet, const MemRegion *)

// Program state: alias map to propagate identity between pointers to metadata_dst
REGISTER_MAP_WITH_PROGRAMSTATE(PtrAliasMap, const MemRegion*, const MemRegion*)

namespace {

class SAGenTestChecker : public Checker<
    check::PreCall,
    check::Bind
  > {
   mutable std::unique_ptr<BugType> BT;

   // TU-level ownership fact: metadata_dst storage fields whose embedded
   // dst had a reference handed out (dst_hold / skb_dst_set[_noref])
   // somewhere in this translation unit. Skbs in flight can still hold such
   // a dst reference, so the backing metadata_dst must be released via
   // dst_release(&...->dst), never freed directly.
   mutable llvm::SmallPtrSet<const FieldDecl *, 8> TUSharedMdDstFields;

   public:
      SAGenTestChecker() : BT(new BugType(this, "Incorrect free of metadata_dst", "Use-after-free risk")) {}

      void checkPreCall(const CallEvent &Call, CheckerContext &C) const;
      void checkBind(SVal Loc, SVal Val, const Stmt *S, CheckerContext &C) const;

   private:
      // Helpers
      static bool isNamedFunc(const CallEvent &Call, StringRef Name, CheckerContext &C);
      static bool isFreeLike(const CallEvent &Call, CheckerContext &C);
      static bool isDstHoldLike(const CallEvent &Call, CheckerContext &C);
      static bool isSkbDstInstallLike(const CallEvent &Call, CheckerContext &C);
      static bool isDstReleaseLike(const CallEvent &Call, CheckerContext &C);

      static const MemRegion *followAliases(ProgramStateRef State, const MemRegion *R);
      static bool baseIsMetadataDst(const Expr *Base);
      static const MemRegion *getMdRegionFromDstAddressArg(const Expr *Arg, CheckerContext &C);
      static const MemRegion *getMdRegionFromExprArg(const Expr *Arg, CheckerContext &C);
      static const FieldDecl *getMdFieldFromDstAddressArg(const Expr *Arg);
      static const FieldDecl *mdStorageFieldFromExpr(const Expr *E);
      static bool isMetadataDstPtrType(QualType T);
      static bool memberIsDstEntryLike(const ValueDecl *VD);
      static bool regionIsMdPtr(const MemRegion *R);

      void reportAtCall(const CallEvent &Call, CheckerContext &C) const;
};

bool SAGenTestChecker::isNamedFunc(const CallEvent &Call, StringRef Name, CheckerContext &C) {
  const Expr *Orig = Call.getOriginExpr();
  if (!Orig)
    return false;
  return ExprHasName(Orig, Name, C);
}

bool SAGenTestChecker::isFreeLike(const CallEvent &Call, CheckerContext &C) {
  return isNamedFunc(Call, "metadata_dst_free", C) ||
         isNamedFunc(Call, "kfree", C) ||
         isNamedFunc(Call, "kfree_sensitive", C) ||
         isNamedFunc(Call, "kvfree", C);
}

bool SAGenTestChecker::isDstHoldLike(const CallEvent &Call, CheckerContext &C) {
  // Ownership-transfer ops that hand out a reference on the embedded dst.
  return isNamedFunc(Call, "dst_hold", C) ||
         isNamedFunc(Call, "dst_hold_safe", C);
}

bool SAGenTestChecker::isSkbDstInstallLike(const CallEvent &Call, CheckerContext &C) {
  // Installing the dst on an skb hands a dst reference to that skb.
  return isNamedFunc(Call, "skb_dst_set", C) ||
         isNamedFunc(Call, "skb_dst_set_noref", C);
}

bool SAGenTestChecker::isDstReleaseLike(const CallEvent &Call, CheckerContext &C) {
  return isNamedFunc(Call, "dst_release", C);
}

static const RecordDecl *getAsRecordDecl(QualType QT) {
  if (QT.isNull())
    return nullptr;
  if (const auto *RT = QT->getAs<RecordType>())
    return RT->getDecl();
  if (const auto *ST = QT->getAsStructureType())
    return ST->getDecl();
  return nullptr;
}

bool SAGenTestChecker::baseIsMetadataDst(const Expr *Base) {
  if (!Base)
    return false;
  QualType T = Base->getType();
  if (T.isNull())
    return false;
  if (const auto *PT = T->getAs<PointerType>())
    T = PT->getPointeeType();
  const RecordDecl *RD = getAsRecordDecl(T);
  if (!RD)
    return false;
  IdentifierInfo *II = RD->getIdentifier();
  if (!II)
    return false;
  return II->getName() == "metadata_dst";
}

const MemRegion *SAGenTestChecker::getMdRegionFromDstAddressArg(const Expr *Arg, CheckerContext &C) {
  if (!Arg)
    return nullptr;

  const Expr *E = Arg->IgnoreParenImpCasts();
  const UnaryOperator *UO = dyn_cast<UnaryOperator>(E);
  if (!UO || UO->getOpcode() != UO_AddrOf)
    return nullptr;

  const Expr *Sub = UO->getSubExpr();
  // The subexpr might have extra parens/casts around a MemberExpr
  const MemberExpr *ME = dyn_cast<MemberExpr>(Sub->IgnoreParenImpCasts());
  if (!ME)
    return nullptr;

  const ValueDecl *VD = ME->getMemberDecl();
  if (!VD || VD->getName() != "dst")
    return nullptr;

  const Expr *Base = ME->getBase();
  if (!baseIsMetadataDst(Base))
    return nullptr;

  // Important: don't strip implicit nodes when retrieving MemRegion, per guidance.
  const MemRegion *MR = getMemRegionFromExpr(Base, C);
  if (!MR)
    return nullptr;
  MR = MR->getBaseRegion();
  return MR;
}

bool SAGenTestChecker::isMetadataDstPtrType(QualType T) {
  if (T.isNull())
    return false;
  const PointerType *PT = T->getAs<PointerType>();
  if (!PT)
    return false;
  const RecordDecl *RD = getAsRecordDecl(PT->getPointeeType());
  if (!RD)
    return false;
  const IdentifierInfo *II = RD->getIdentifier();
  return II && II->getName() == "metadata_dst";
}

bool SAGenTestChecker::memberIsDstEntryLike(const ValueDecl *VD) {
  if (!VD)
    return false;
  // The refcounted owner slot of a metadata_dst is its embedded dst_entry
  // member; identify it structurally, not only by the canonical name.
  if (VD->getName() == "dst")
    return true;
  const RecordDecl *RD = getAsRecordDecl(VD->getType());
  return RD && RD->getIdentifier() && RD->getIdentifier()->getName() == "dst_entry";
}

const FieldDecl *SAGenTestChecker::mdStorageFieldFromExpr(const Expr *E) {
  if (!E)
    return nullptr;
  E = E->IgnoreParenImpCasts();
  const MemberExpr *ME = dyn_cast<MemberExpr>(E);
  if (!ME)
    return nullptr;
  const FieldDecl *FD = dyn_cast<FieldDecl>(ME->getMemberDecl());
  if (!FD)
    return nullptr;
  if (!isMetadataDstPtrType(FD->getType()))
    return nullptr;
  return FD;
}

const FieldDecl *SAGenTestChecker::getMdFieldFromDstAddressArg(const Expr *Arg) {
  if (!Arg)
    return nullptr;
  const Expr *E = Arg->IgnoreParenImpCasts();
  const UnaryOperator *UO = dyn_cast<UnaryOperator>(E);
  if (!UO || UO->getOpcode() != UO_AddrOf)
    return nullptr;
  const MemberExpr *ME =
      dyn_cast<MemberExpr>(UO->getSubExpr()->IgnoreParenImpCasts());
  if (!ME || !memberIsDstEntryLike(ME->getMemberDecl()))
    return nullptr;
  if (!baseIsMetadataDst(ME->getBase()))
    return nullptr;
  // The shared metadata_dst value flows out of a pointer storage field
  // (e.g. tx_sc->md_dst); that field is the ownership provenance key.
  return mdStorageFieldFromExpr(ME->getBase());
}

const MemRegion *SAGenTestChecker::getMdRegionFromExprArg(const Expr *Arg, CheckerContext &C) {
  if (!Arg)
    return nullptr;

  // Ensure the argument is of type 'struct metadata_dst *'
  QualType T = Arg->getType();
  const PointerType *PT = T->getAs<PointerType>();
  if (!PT)
    return nullptr;
  const RecordDecl *RD = getAsRecordDecl(PT->getPointeeType());
  if (!RD)
    return nullptr;
  const IdentifierInfo *II = RD->getIdentifier();
  if (!II || II->getName() != "metadata_dst")
    return nullptr;

  const MemRegion *MR = getMemRegionFromExpr(Arg, C);
  if (!MR)
    return nullptr;
  MR = MR->getBaseRegion();
  return MR;
}

bool SAGenTestChecker::regionIsMdPtr(const MemRegion *R) {
  if (!R)
    return false;
  const auto *TVR = dyn_cast<TypedValueRegion>(R);
  if (!TVR)
    return false;
  QualType T = TVR->getValueType();
  const PointerType *PT = T->getAs<PointerType>();
  if (!PT)
    return false;
  const RecordDecl *RD = getAsRecordDecl(PT->getPointeeType());
  if (!RD)
    return false;
  const IdentifierInfo *II = RD->getIdentifier();
  return II && II->getName() == "metadata_dst";
}

const MemRegion *SAGenTestChecker::followAliases(ProgramStateRef State, const MemRegion *R) {
  if (!R)
    return nullptr;
  R = R->getBaseRegion();
  llvm::SmallPtrSet<const MemRegion *, 8> Visited;
  const MemRegion *Cur = R;
  while (Cur && !Visited.count(Cur)) {
    Visited.insert(Cur);
    if (const MemRegion *const *NextP = State->get<PtrAliasMap>(Cur))
      Cur = *NextP;
    else
      break;
  }
  return Cur ? Cur->getBaseRegion() : R;
}

void SAGenTestChecker::reportAtCall(const CallEvent &Call, CheckerContext &C) const {
  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto R = std::make_unique<PathSensitiveBugReport>(
      *BT,
      "Freeing metadata_dst directly while refs may exist; use dst_release(&...->dst)",
      N);
  R->addRange(Call.getSourceRange());
  C.emitReport(std::move(R));
}

void SAGenTestChecker::checkPreCall(const CallEvent &Call, CheckerContext &C) const {
  ProgramStateRef State = C.getState();

  // Track installs/ref-holds
  if (isDstHoldLike(Call, C)) {
    if (Call.getNumArgs() >= 1) {
      const Expr *ArgE = Call.getArgExpr(0);
      const MemRegion *MR = getMdRegionFromDstAddressArg(ArgE, C);
      const FieldDecl *FD = getMdFieldFromDstAddressArg(ArgE);
      if (FD)
        TUSharedMdDstFields.insert(FD);
      if (MR) {
        const MemRegion *Root = followAliases(State, MR);
        State = State->add<RefcountedMdSet>(Root);
        C.addTransition(State);
      }
    }
    return;
  }

  if (isSkbDstInstallLike(Call, C)) {
    if (Call.getNumArgs() >= 2) {
      const Expr *ArgE = Call.getArgExpr(1);
      const MemRegion *MR = getMdRegionFromDstAddressArg(ArgE, C);
      const FieldDecl *FD = getMdFieldFromDstAddressArg(ArgE);
      if (FD)
        TUSharedMdDstFields.insert(FD);
      if (MR) {
        const MemRegion *Root = followAliases(State, MR);
        State = State->add<RefcountedMdSet>(Root);
        C.addTransition(State);
      }
    }
    return;
  }

  // Safe release: drop from set when we see dst_release(&md->dst)
  if (isDstReleaseLike(Call, C)) {
    if (Call.getNumArgs() >= 1) {
      const Expr *ArgE = Call.getArgExpr(0);
      const MemRegion *MR = getMdRegionFromDstAddressArg(ArgE, C);
      if (MR) {
        const MemRegion *Root = followAliases(State, MR);
        if (State->contains<RefcountedMdSet>(Root)) {
          State = State->remove<RefcountedMdSet>(Root);
          C.addTransition(State);
        }
      }
    }
    return;
  }

  // Diagnose free-like calls on metadata_dst pointer
  if (isFreeLike(Call, C)) {
    if (Call.getNumArgs() >= 1) {
      const Expr *ArgE = Call.getArgExpr(0);

      // Strong signal: path-proven that this md_dst had its dst ref-held/installed.
      const MemRegion *MR = getMdRegionFromExprArg(ArgE, C);
      if (MR) {
        const MemRegion *Root = followAliases(State, MR);
        if (State->contains<RefcountedMdSet>(Root)) {
          reportAtCall(Call, C);
          return;
        }
      }

      // Ownership provenance: the freed pointer is loaded from a storage
      // field whose embedded dst had a reference handed to skbs (dst_hold /
      // skb_dst_set[_noref]) elsewhere in this TU; a direct free drops the
      // object while those outstanding dst references may still exist.
      if (const FieldDecl *FD = mdStorageFieldFromExpr(ArgE)) {
        if (TUSharedMdDstFields.count(FD)) {
          reportAtCall(Call, C);
          return;
        }
      }
    }
    return;
  }
}

void SAGenTestChecker::checkBind(SVal Loc, SVal Val, const Stmt *S, CheckerContext &C) const {
  ProgramStateRef State = C.getState();

  const MemRegion *LHSReg = Loc.getAsRegion();
  if (!LHSReg)
    return;
  LHSReg = LHSReg->getBaseRegion();

  // Track pointer aliases only for metadata_dst*
  if (!regionIsMdPtr(LHSReg))
    return;

  const MemRegion *RHSReg = Val.getAsRegion();
  if (!RHSReg)
    return;
  RHSReg = RHSReg->getBaseRegion();

  // Propagate alias mapping both ways to ease followAliases
  const MemRegion *Root = followAliases(State, RHSReg);
  State = State->set<PtrAliasMap>(LHSReg, Root);
  State = State->set<PtrAliasMap>(Root, LHSReg);
  C.addTransition(State);
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects freeing metadata_dst directly despite refcounts; use dst_release(&...->dst)",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
