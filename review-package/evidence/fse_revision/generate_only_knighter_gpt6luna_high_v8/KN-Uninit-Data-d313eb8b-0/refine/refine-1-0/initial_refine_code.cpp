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
#include "llvm/ADT/SmallVector.h"

using namespace clang;
using namespace ento;
using namespace taint;

REGISTER_SET_WITH_PROGRAMSTATE(ZeroedStructSet, const MemRegion *)

namespace {

class SAGenTestChecker : public Checker<check::PostCall, check::PreCall> {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker()
      : BT(new BugType(this, "Kernel information leak", "Security")) {}

  void checkPostCall(const CallEvent &Call, CheckerContext &C) const;
  void checkPreCall(const CallEvent &Call, CheckerContext &C) const;

private:
  bool isFuncNamed(const CallEvent &Call, StringRef Name) const;
  bool extractAddrOfLocalRecordVar(const Expr *E, const VarDecl *&VD) const;
  bool sizeMatchesWholeObject(const Expr *SizeE, const VarDecl *VD,
                              CheckerContext &C) const;
  bool isZeroValueExpr(const Expr *E, CheckerContext &C) const;
  void recordWholeObjectZeroing(const CallEvent &Call, CheckerContext &C,
                                unsigned DstIdx, unsigned SizeIdx) const;
  bool matchSink(const CallEvent &Call, unsigned &LenIdx,
                 unsigned &DataIdx) const;
  void reportLeak(const CallEvent &Call, CheckerContext &C) const;
};

// Prefer the callee declaration over source-text matching. Source text may
// include wrappers, macros, or other spelling that does not identify the
// function actually called.
bool SAGenTestChecker::isFuncNamed(const CallEvent &Call, StringRef Name) const {
  if (const IdentifierInfo *ID = Call.getCalleeIdentifier())
    return ID->getName() == Name;

  if (const auto *FD = dyn_cast_or_null<FunctionDecl>(Call.getDecl()))
    return FD->getName() == Name;

  return false;
}

bool SAGenTestChecker::extractAddrOfLocalRecordVar(const Expr *E,
                                                   const VarDecl *&VD) const {
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

  return VD->hasLocalStorage() && !VD->hasGlobalStorage();
}

bool SAGenTestChecker::isZeroValueExpr(const Expr *E,
                                       CheckerContext &C) const {
  if (!E)
    return false;

  llvm::APSInt Val;
  return EvaluateExprToInt(Val, E, C) && Val.isZero();
}

bool SAGenTestChecker::sizeMatchesWholeObject(const Expr *SizeE,
                                              const VarDecl *VD,
                                              CheckerContext &C) const {
  if (!SizeE || !VD)
    return false;

  if (const auto *UETT =
          findSpecificTypeInChildren<UnaryExprOrTypeTraitExpr>(SizeE)) {
    if (UETT->getKind() == UETT_SizeOf) {
      if (UETT->isArgumentType()) {
        QualType ArgTy = UETT->getArgumentType();
        if (!ArgTy.isNull() &&
            C.getASTContext().hasSameType(
                ArgTy.getUnqualifiedType(),
                VD->getType().getUnqualifiedType()))
          return true;
      } else if (const Expr *Arg = UETT->getArgumentExpr()) {
        Arg = Arg->IgnoreParenCasts();
        if (const auto *DRE = dyn_cast<DeclRefExpr>(Arg))
          if (DRE->getDecl() == VD)
            return true;
      }
    }
  }

  llvm::APSInt Val;
  if (!EvaluateExprToInt(Val, SizeE, C) || !Val.isNonNegative())
    return false;

  uint64_t ObjSizeBytes =
      C.getASTContext().getTypeSizeInChars(VD->getType()).getQuantity();
  return Val.getZExtValue() == ObjSizeBytes;
}

void SAGenTestChecker::recordWholeObjectZeroing(const CallEvent &Call,
                                                CheckerContext &C,
                                                unsigned DstIdx,
                                                unsigned SizeIdx) const {
  if (Call.getNumArgs() <= std::max(DstIdx, SizeIdx))
    return;

  const Expr *DstExpr = Call.getArgExpr(DstIdx);
  const Expr *SizeExpr = Call.getArgExpr(SizeIdx);
  if (!DstExpr || !SizeExpr)
    return;

  const VarDecl *VD = nullptr;
  if (!extractAddrOfLocalRecordVar(DstExpr, VD) ||
      !sizeMatchesWholeObject(SizeExpr, VD, C))
    return;

  const MemRegion *MR = getMemRegionFromExpr(DstExpr, C);
  if (!MR)
    return;

  MR = MR->getBaseRegion();
  if (!MR)
    return;

  ProgramStateRef State = C.getState();
  C.addTransition(State->add<ZeroedStructSet>(MR));
}

bool SAGenTestChecker::matchSink(const CallEvent &Call, unsigned &LenIdx,
                                 unsigned &DataIdx) const {
  struct SinkInfo {
    StringRef Name;
    unsigned LenIndex;
    unsigned DataIndex;
  };

  static const SinkInfo Sinks[] = {
      {"nla_put", 2u, 3u},
      {"nla_put_64bit", 2u, 3u},
  };

  for (const SinkInfo &Sink : Sinks) {
    if (isFuncNamed(Call, Sink.Name)) {
      LenIdx = Sink.LenIndex;
      DataIdx = Sink.DataIndex;
      return true;
    }
  }

  return false;
}

void SAGenTestChecker::reportLeak(const CallEvent &Call,
                                  CheckerContext &C) const {
  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto R = std::make_unique<PathSensitiveBugReport>(
      *BT, "stack struct not fully zeroed before user copy (padding leak)", N);
  R->addRange(Call.getSourceRange());
  C.emitReport(std::move(R));
}

void SAGenTestChecker::checkPostCall(const CallEvent &Call,
                                     CheckerContext &C) const {
  if (isFuncNamed(Call, "memset") ||
      isFuncNamed(Call, "__builtin_memset")) {
    if (Call.getNumArgs() >= 3 &&
        isZeroValueExpr(Call.getArgExpr(1), C))
      recordWholeObjectZeroing(Call, C, /*DstIdx=*/0, /*SizeIdx=*/2);
    return;
  }

  if (isFuncNamed(Call, "memzero_explicit") ||
      isFuncNamed(Call, "bpf_memzero")) {
    if (Call.getNumArgs() >= 2)
      recordWholeObjectZeroing(Call, C, /*DstIdx=*/0, /*SizeIdx=*/1);
  }
}

void SAGenTestChecker::checkPreCall(const CallEvent &Call,
                                    CheckerContext &C) const {
  unsigned LenIdx = 0;
  unsigned DataIdx = 0;
  if (!matchSink(Call, LenIdx, DataIdx))
    return;

  if (Call.getNumArgs() <= std::max(LenIdx, DataIdx))
    return;

  const Expr *DataExpr = Call.getArgExpr(DataIdx);
  const Expr *LenExpr = Call.getArgExpr(LenIdx);
  if (!DataExpr || !LenExpr)
    return;

  const VarDecl *VD = nullptr;
  if (!extractAddrOfLocalRecordVar(DataExpr, VD) ||
      !sizeMatchesWholeObject(LenExpr, VD, C))
    return;

  const MemRegion *MR = getMemRegionFromExpr(DataExpr, C);
  if (!MR)
    return;

  MR = MR->getBaseRegion();
  if (!MR)
    return;

  ProgramStateRef State = C.getState();
  if (!State->contains<ZeroedStructSet>(MR))
    reportLeak(Call, C);
}

} // namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects copying partially initialized stack structs with sizeof(struct) to user (padding leak)",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
