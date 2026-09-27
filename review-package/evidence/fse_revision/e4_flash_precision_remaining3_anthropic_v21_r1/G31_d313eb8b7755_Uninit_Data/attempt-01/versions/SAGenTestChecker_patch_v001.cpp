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
#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Stmt.h"
#include "clang/Lex/Lexer.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/ImmutableSet.h"
#include "llvm/ADT/SmallVector.h"

using namespace clang;
using namespace ento;
using namespace taint;

REGISTER_SET_WITH_PROGRAMSTATE(ZeroedStructSet, const MemRegion *)

// Per-path set of record fields that were explicitly written, keyed by the
// base region of the local record variable.
REGISTER_MAP_WITH_PROGRAMSTATE(CoveredFieldMap, const MemRegion *,
                               llvm::ImmutableSet<const FieldDecl *>)

namespace {

class SAGenTestChecker : public Checker<check::PostCall, check::PreCall, check::Bind> {
   mutable std::unique_ptr<BugType> BT;
   mutable llvm::ImmutableSet<const FieldDecl *>::Factory FieldSetFactory;

   public:
      SAGenTestChecker() : BT(new BugType(this, "Kernel information leak", "Security")) {}

      void checkPostCall(const CallEvent &Call, CheckerContext &C) const;
      void checkPreCall(const CallEvent &Call, CheckerContext &C) const;
      void checkBind(SVal Loc, SVal Val, const Stmt *StoreE, CheckerContext &C) const;

   private:

      // Helpers
      bool isFuncNamed(const CallEvent &Call, CheckerContext &C, StringRef Name) const;
      bool extractAddrOfLocalRecordVar(const Expr *E, const VarDecl *&VD) const;
      bool sizeMatchesWholeObject(const Expr *SizeE, const VarDecl *VD, CheckerContext &C) const;
      bool isZeroValueExpr(const Expr *E, CheckerContext &C) const;
      void recordWholeObjectZeroing(const CallEvent &Call, CheckerContext &C,
                                    unsigned DstIdx, unsigned SizeIdx) const;
      bool matchSink(const CallEvent &Call, CheckerContext &C,
                     unsigned &LenIdx, unsigned &DataIdx) const;
      bool isFullyFieldCovered(const MemRegion *MR, const VarDecl *VD,
                               CheckerContext &C) const;
      void noteFieldWrite(const MemRegion *LocRegion, CheckerContext &C) const;
      void reportLeak(const CallEvent &Call, CheckerContext &C) const;
};

// Check function name using source text of the origin expression.
bool SAGenTestChecker::isFuncNamed(const CallEvent &Call, CheckerContext &C, StringRef Name) const {
  const Expr *OriginExpr = Call.getOriginExpr();
  if (!OriginExpr)
    return false;
  return ExprHasName(OriginExpr, Name, C);
}

bool SAGenTestChecker::extractAddrOfLocalRecordVar(const Expr *E, const VarDecl *&VD) const {
  if (!E)
    return false;
  E = E->IgnoreParenCasts();
  const auto *UO = dyn_cast<UnaryOperator>(E);
  if (!UO || UO->getOpcode() != UO_AddrOf)
    return false;

  const Expr *Sub = UO->getSubExpr();
  if (!Sub)
    return false;
  Sub = Sub->IgnoreParenCasts();

  const auto *DRE = dyn_cast<DeclRefExpr>(Sub);
  if (!DRE)
    return false;

  VD = dyn_cast<VarDecl>(DRE->getDecl());
  if (!VD)
    return false;

  QualType T = VD->getType();
  if (T.isNull() || !T->isRecordType())
    return false;

  if (!VD->hasLocalStorage() || VD->hasGlobalStorage())
    return false;

  return true;
}

bool SAGenTestChecker::isZeroValueExpr(const Expr *E, CheckerContext &C) const {
  if (!E)
    return false;
  llvm::APSInt Val;
  if (EvaluateExprToInt(Val, E, C)) {
    return Val.isZero();
  }
  return false;
}

bool SAGenTestChecker::sizeMatchesWholeObject(const Expr *SizeE, const VarDecl *VD,
                                              CheckerContext &C) const {
  if (!SizeE || !VD)
    return false;

  // Prefer sizeof-based check.
  if (const auto *UETT = findSpecificTypeInChildren<UnaryExprOrTypeTraitExpr>(SizeE)) {
    if (UETT->getKind() == UETT_SizeOf) {
      if (UETT->isArgumentType()) {
        QualType ArgTy = UETT->getArgumentType();
        if (!ArgTy.isNull()) {
          if (C.getASTContext().hasSameType(ArgTy.getUnqualifiedType(),
                                            VD->getType().getUnqualifiedType()))
            return true;
        }
      } else {
        const Expr *Arg = UETT->getArgumentExpr();
        if (Arg) {
          Arg = Arg->IgnoreParenCasts();
          if (const auto *DRE = dyn_cast<DeclRefExpr>(Arg)) {
            if (DRE->getDecl() == VD)
              return true;
          }
        }
      }
    }
  }

  // Fallback: constant value equals sizeof(VD->getType())
  llvm::APSInt Val;
  if (EvaluateExprToInt(Val, SizeE, C)) {
    uint64_t ObjSizeBytes =
        C.getASTContext().getTypeSizeInChars(VD->getType()).getQuantity();
    return Val.isNonNegative() && Val.getZExtValue() == ObjSizeBytes;
  }

  return false;
}

void SAGenTestChecker::recordWholeObjectZeroing(const CallEvent &Call, CheckerContext &C,
                                                unsigned DstIdx, unsigned SizeIdx) const {
  if (Call.getNumArgs() <= std::max(DstIdx, SizeIdx))
    return;

  const Expr *DstExpr = Call.getArgExpr(DstIdx);
  const Expr *SizeExpr = Call.getArgExpr(SizeIdx);
  if (!DstExpr || !SizeExpr)
    return;

  // Ensure the destination is &Var where Var is a local record.
  const VarDecl *VD = nullptr;
  if (!extractAddrOfLocalRecordVar(DstExpr, VD))
    return;

  // Ensure size equals the whole object size.
  if (!sizeMatchesWholeObject(SizeExpr, VD, C))
    return;

  // Get the region from the original expression (without stripping) and add base region to the set.
  const MemRegion *MR = getMemRegionFromExpr(DstExpr, C);
  if (!MR)
    return;
  MR = MR->getBaseRegion();
  if (!MR)
    return;

  ProgramStateRef State = C.getState();
  State = State->add<ZeroedStructSet>(MR);
  C.addTransition(State);
}

bool SAGenTestChecker::matchSink(const CallEvent &Call, CheckerContext &C,
                                 unsigned &LenIdx, unsigned &DataIdx) const {
  struct SinkInfo {
    StringRef Name;
    unsigned LenIndex;
    unsigned DataIndex;
  };
  static const SinkInfo Sinks[] = {
      {"nla_put", 2u, 3u},
      {"nla_put_64bit", 2u, 3u},
  };

  for (const auto &S : Sinks) {
    if (isFuncNamed(Call, C, S.Name)) {
      LenIdx = S.LenIndex;
      DataIdx = S.DataIndex;
      return true;
    }
  }
  return false;
}

void SAGenTestChecker::reportLeak(const CallEvent &Call, CheckerContext &C) const {
  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;
  auto R = std::make_unique<PathSensitiveBugReport>(
      *BT, "stack struct not fully zeroed before user copy (padding leak)", N);
  R->addRange(Call.getSourceRange());
  C.emitReport(std::move(R));
}

void SAGenTestChecker::checkPostCall(const CallEvent &Call, CheckerContext &C) const {
  // Detect whole-object zeroing calls.
  // memset(dst, 0, size)
  if (isFuncNamed(Call, C, "memset") || isFuncNamed(Call, C, "__builtin_memset")) {
    if (Call.getNumArgs() >= 3) {
      const Expr *ValExpr = Call.getArgExpr(1);
      if (isZeroValueExpr(ValExpr, C)) {
        recordWholeObjectZeroing(Call, C, /*DstIdx=*/0, /*SizeIdx=*/2);
      }
    }
    return;
  }

  // memzero_explicit(dst, size) or bpf_memzero(dst, size)
  if (isFuncNamed(Call, C, "memzero_explicit") || isFuncNamed(Call, C, "bpf_memzero")) {
    if (Call.getNumArgs() >= 2) {
      recordWholeObjectZeroing(Call, C, /*DstIdx=*/0, /*SizeIdx=*/1);
    }
    return;
  }
}

void SAGenTestChecker::checkPreCall(const CallEvent &Call, CheckerContext &C) const {
  // Whole-object zeroing must be recorded before the call is evaluated:
  // memset is typically modeled by a built-in eval::Call model, and calls
  // taken over that way do not reach checkPostCall. Recording the zeroed
  // state in the pre-call callback keeps the patch's memset(&opt, 0,
  // sizeof(opt)) guard effective for every revision.
  if (isFuncNamed(Call, C, "memset") || isFuncNamed(Call, C, "__builtin_memset")) {
    if (Call.getNumArgs() >= 3) {
      const Expr *ValExpr = Call.getArgExpr(1);
      if (isZeroValueExpr(ValExpr, C))
        recordWholeObjectZeroing(Call, C, /*DstIdx=*/0, /*SizeIdx=*/2);
    }
    return;
  }

  // memzero_explicit(dst, size) or bpf_memzero(dst, size)
  if (isFuncNamed(Call, C, "memzero_explicit") || isFuncNamed(Call, C, "bpf_memzero")) {
    if (Call.getNumArgs() >= 2)
      recordWholeObjectZeroing(Call, C, /*DstIdx=*/0, /*SizeIdx=*/1);
    return;
  }

  // Detect sinks that export raw bytes to user space, e.g., nla_put*
  unsigned LenIdx = 0, DataIdx = 0;
  if (!matchSink(Call, C, LenIdx, DataIdx))
    return;

  if (Call.getNumArgs() <= std::max(LenIdx, DataIdx))
    return;

  const Expr *DataExpr = Call.getArgExpr(DataIdx);
  const Expr *LenExpr = Call.getArgExpr(LenIdx);
  if (!DataExpr || !LenExpr)
    return;

  // Ensure data is &Var where Var is a local record.
  const VarDecl *VD = nullptr;
  if (!extractAddrOfLocalRecordVar(DataExpr, VD))
    return;

  // Ensure length equals sizeof(Var) to match the risky pattern.
  if (!sizeMatchesWholeObject(LenExpr, VD, C))
    return;

  // Get the MemRegion for the data argument and check if it was fully zeroed.
  const MemRegion *MR = getMemRegionFromExpr(DataExpr, C);
  if (!MR)
    return;
  MR = MR->getBaseRegion();
  if (!MR)
    return;

  ProgramStateRef State = C.getState();
  // Silence when the object was zeroed as a whole before the copy (the
  // patch's memset(&opt, 0, sizeof(opt)) guard establishes this state), or
  // when every field of the local record has been written on this path, so
  // no uninitialized bytes of the object can reach the user copy.
  if (State->contains<ZeroedStructSet>(MR))
    return;
  if (isFullyFieldCovered(MR, VD, C))
    return;

  reportLeak(Call, C);
}

void SAGenTestChecker::checkBind(SVal Loc, SVal Val, const Stmt *,
                                 CheckerContext &C) const {
  const MemRegion *LocRegion = Loc.getAsRegion();
  if (!LocRegion)
    return;
  noteFieldWrite(LocRegion, C);
}

void SAGenTestChecker::noteFieldWrite(const MemRegion *LocRegion,
                                      CheckerContext &C) const {
  // Track direct writes into fields of a local record variable so that a
  // struct whose every field is assigned before the copy (like tcf_t) is
  // not reported as partially initialized.
  const auto *FR = dyn_cast<FieldRegion>(LocRegion);
  if (!FR)
    return;
  const auto *VR = dyn_cast_or_null<VarRegion>(FR->getSuperRegion());
  if (!VR)
    return;

  const VarDecl *VD = VR->getDecl();
  QualType T = VD->getType();
  if (T.isNull() || !T->isRecordType())
    return;
  if (!VD->hasLocalStorage() || VD->hasGlobalStorage())
    return;

  const FieldDecl *FD = FR->getDecl();
  if (!FD)
    return;

  llvm::ImmutableSet<const FieldDecl *> Covered;
  if (const llvm::ImmutableSet<const FieldDecl *> *Prev =
          C.getState()->get<CoveredFieldMap>(VR))
    Covered = *Prev;
  else
    Covered = FieldSetFactory.getEmptySet();

  Covered = FieldSetFactory.add(FD->getCanonicalDecl(), Covered);
  C.addTransition(C.getState()->set<CoveredFieldMap>(VR, Covered));
}

bool SAGenTestChecker::isFullyFieldCovered(const MemRegion *MR, const VarDecl *VD,
                                           CheckerContext &C) const {
  const auto *RT = VD->getType()->getAs<RecordType>();
  if (!RT)
    return false;
  const RecordDecl *RD = RT->getDecl()->getDefinition();
  if (!RD)
    return false;

  const llvm::ImmutableSet<const FieldDecl *> *Covered =
      C.getState()->get<CoveredFieldMap>(MR);
  if (!Covered)
    return false;

  const bool IsUnion = RD->isUnion();
  bool FoundNamed = false;
  for (const FieldDecl *FD : RD->fields()) {
    if (FD->isUnnamedBitField() || FD->getName().empty())
      continue;
    FoundNamed = true;
    const bool Has = Covered->contains(FD->getCanonicalDecl());
    if (IsUnion) {
      if (Has)
        return true;
    } else if (!Has) {
      return false;
    }
  }
  return IsUnion ? false : FoundNamed;
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects copying partially initialized stack structs with sizeof(struct) to user (padding leak)",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
