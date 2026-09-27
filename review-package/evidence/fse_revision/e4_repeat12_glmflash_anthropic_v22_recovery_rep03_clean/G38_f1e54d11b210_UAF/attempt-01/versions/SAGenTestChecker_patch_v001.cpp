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

   public:
      SAGenTestChecker() : BT(new BugType(this, "Incorrect free of metadata_dst", "Use-after-free risk")) {}

      void checkPreCall(const CallEvent &Call, CheckerContext &C) const;
      void checkBind(SVal Loc, SVal Val, const Stmt *S, CheckerContext &C) const;

   private:
      // Helpers
      static bool isNamedFunc(const CallEvent &Call, StringRef Name, CheckerContext &C);
      static bool isFreeLike(const CallEvent &Call, CheckerContext &C);
      static bool isDstHoldLike(const CallEvent &Call, CheckerContext &C);
      static bool isSkbDstSetLike(const CallEvent &Call, CheckerContext &C);
      static bool isDstReleaseLike(const CallEvent &Call, CheckerContext &C);

      static const MemRegion *followAliases(ProgramStateRef State, const MemRegion *R);
      static bool baseIsMetadataDst(const Expr *Base);
      static const MemRegion *getMdRegionFromDstAddressArg(const Expr *Arg, CheckerContext &C);
      static const MemRegion *getMdRegionFromExprArg(const Expr *Arg, CheckerContext &C);
      static bool regionIsMdPtr(const MemRegion *R);
      static bool isMetadataDstPtrExpr(const Expr *E);
      static bool isSharedStoredMdReference(const Expr *ArgE);

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
  return isNamedFunc(Call, "dst_hold", C);
}

bool SAGenTestChecker::isSkbDstSetLike(const CallEvent &Call, CheckerContext &C) {
  return isNamedFunc(Call, "skb_dst_set", C);
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

// True for 'struct metadata_dst *'. A metadata_dst embeds a refcounted
// dst_entry, so its lifetime is governed by that refcount.
static bool isMetadataDstPtrType(QualType T) {
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

bool SAGenTestChecker::isMetadataDstPtrExpr(const Expr *E) {
  if (!E)
    return false;
  return isMetadataDstPtrType(E->getType());
}

// Ownership relation: the freed metadata_dst pointer is loaded from persistent
// object storage (a field of a heap-resident object such as device private
// data) instead of being a plain function-local pointer. Such stored
// references are reachable by other subsystems, and the embedded dst refcount
// of the metadata_dst may still be held (e.g. while it is installed into an
// skb). Freeing the object directly can then release memory that is still
// referenced; it must be retired through its refcount via
// dst_release(&...->dst) instead.
bool SAGenTestChecker::isSharedStoredMdReference(const Expr *ArgE) {
  if (!ArgE)
    return false;

  const Expr *E = ArgE->IgnoreParenImpCasts();
  const MemberExpr *ME = dyn_cast<MemberExpr>(E);
  if (!ME)
    return false; // a local pointer value carries no stored-reference relation

  const ValueDecl *FD = ME->getMemberDecl();
  if (!FD || !isMetadataDstPtrType(FD->getType()))
    return false;

  // The containing object must be reached through pointer indirection into
  // dynamically obtained storage; a field of a local aggregate does not
  // establish the shared-storage relation.
  bool HasIndirection = false;
  const Expr *Cur = E;
  while (const auto *CurME = dyn_cast<MemberExpr>(Cur)) {
    if (CurME->isArrow())
      HasIndirection = true;
    Cur = CurME->getBase()->IgnoreParenImpCasts();
  }
  return HasIndirection;
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
      if (MR) {
        const MemRegion *Root = followAliases(State, MR);
        State = State->add<RefcountedMdSet>(Root);
        C.addTransition(State);
      }
    }
    return;
  }

  if (isSkbDstSetLike(Call, C)) {
    if (Call.getNumArgs() >= 2) {
      const Expr *ArgE = Call.getArgExpr(1);
      const MemRegion *MR = getMdRegionFromDstAddressArg(ArgE, C);
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

  // Diagnose free-like calls on metadata_dst pointers.
  if (isFreeLike(Call, C)) {
    if (Call.getNumArgs() >= 1) {
      const Expr *ArgE = Call.getArgExpr(0);
      if (!isMetadataDstPtrExpr(ArgE))
        return;

      // Strong signal: path-proven that this md_dst had its dst ref-held/installed.
      const MemRegion *MR = getMdRegionFromExprArg(ArgE, C);
      if (MR) {
        const MemRegion *Root = followAliases(State, MR);
        if (State->contains<RefcountedMdSet>(Root)) {
          reportAtCall(Call, C);
          return;
        }
      }

      // Ownership relation: the freed metadata_dst is a pointer stored in
      // persistent heap-resident object storage (e.g. device private data),
      // so this function is not its unique owner. Its embedded dst refcount
      // may still be held, and the object must be retired via
      // dst_release(&...->dst) instead of being freed directly.
      if (isSharedStoredMdReference(ArgE)) {
        reportAtCall(Call, C);
        return;
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
