#include "clang/AST/Expr.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/Version.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/CheckerManager.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/MemRegion.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/SVals.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"
#include "llvm/ADT/StringRef.h"
#include <memory>

using namespace clang;
using namespace ento;

namespace {

class UnboundedFormattedWriteChecker : public Checker<check::PreCall> {
  mutable std::unique_ptr<BugType> BT;

  static const Expr *stripExpr(const Expr *E) {
    if (!E)
      return nullptr;
    return E->IgnoreParenImpCasts();
  }

  static bool isTargetFunction(StringRef Name) {
    return Name == "g_sprintf" || Name == "sprintf" || Name == "vsprintf";
  }

  static bool isCharLikeType(QualType QT) {
    if (QT.isNull())
      return false;

    if (const auto *AT = QT->getAsArrayTypeUnsafe()) {
      QualType ElemTy = AT->getElementType();
      if (ElemTy.isNull())
        return false;
      return ElemTy->isAnyCharacterType();
    }

    return false;
  }

  static bool isLocalArrayDestinationExpr(const Expr *E) {
    E = stripExpr(E);
    if (!E)
      return false;

    if (const auto *DRE = dyn_cast<DeclRefExpr>(E)) {
      const ValueDecl *VD = DRE->getDecl();
      if (!VD)
        return false;
      const auto *Var = dyn_cast<VarDecl>(VD);
      if (!Var)
        return false;
      if (!Var->hasLocalStorage())
        return false;
      return isCharLikeType(Var->getType());
    }

    if (const auto *ME = dyn_cast<MemberExpr>(E)) {
      const ValueDecl *VD = ME->getMemberDecl();
      if (!VD)
        return false;
      const auto *FD = dyn_cast<FieldDecl>(VD);
      if (!FD)
        return false;
      return isCharLikeType(FD->getType());
    }

    return false;
  }

  static bool firstArgLooksLikeDestinationBuffer(const CallEvent &Call) {
    if (Call.getNumArgs() < 1)
      return false;
    const Expr *Arg0 = Call.getArgExpr(0);
    if (!Arg0)
      return false;

    if (isLocalArrayDestinationExpr(Arg0))
      return true;

    SVal V = Call.getArgSVal(0);
    std::optional<loc::MemRegionVal> MRV = V.getAs<loc::MemRegionVal>();
    if (!MRV)
      return false;
    const MemRegion *MR = MRV->getRegion();
    if (!MR)
      return false;

    const auto *VR = dyn_cast<VarRegion>(MR->StripCasts());
    if (!VR)
      return false;
    const VarDecl *Var = VR->getDecl();
    if (!Var)
      return false;
    if (!Var->hasLocalStorage())
      return false;
    return isCharLikeType(Var->getType());
  }

public:
  UnboundedFormattedWriteChecker()
      : BT(std::make_unique<BugType>(this,
                                     "Unbounded formatted write",
                                     "Memory safety")) {}

  void checkPreCall(const CallEvent &Call, CheckerContext &C) const {
    const IdentifierInfo *II = Call.getCalleeIdentifier();
    if (!II)
      return;

    StringRef FuncName = II->getName();
    if (!isTargetFunction(FuncName))
      return;

    if (!firstArgLooksLikeDestinationBuffer(Call))
      return;

    ExplodedNode *N = C.generateNonFatalErrorNode();
    if (!N)
      return;

    auto R = std::make_unique<PathSensitiveBugReport>(
        *BT,
        "Unbounded formatted write into a local array via sprintf-like API; use a size-bounded formatting function.",
        N);
    C.emitReport(std::move(R));
  }
};

} // namespace

extern "C" void clang_registerCheckers(CheckerRegistry &Registry) {
  Registry.addChecker<UnboundedFormattedWriteChecker>(
      "custom.UnboundedFormattedWriteChecker",
      "Detect unbounded sprintf-like writes into local arrays.",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] = CLANG_VERSION_STRING;
