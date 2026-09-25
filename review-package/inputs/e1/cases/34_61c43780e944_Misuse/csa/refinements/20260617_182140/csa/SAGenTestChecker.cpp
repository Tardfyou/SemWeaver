// /external/upstream-study/SAGEN/result-0224-bugfail-multi-o3mini/test-Misuse-61c43780e9444123410cd48c2483e01d2b8f75e8/checkers/checker0.cpp
#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Checkers/Taint.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/SymExpr.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"
#include "clang/StaticAnalyzer/Checkers/utility.h"

using namespace clang;
using namespace ento;
using namespace taint;

namespace {

static bool isDirectCallTo(const CallExpr *CE, StringRef CalleeName) {
  if (!CE)
    return false;

  const FunctionDecl *FD = CE->getDirectCallee();
  return FD && FD->getName() == CalleeName;
}

static bool isEnumConstantNamed(const Expr *E, StringRef ConstantName) {
  if (!E)
    return false;

  E = E->IgnoreParenImpCasts();
  if (const auto *DRE = dyn_cast<DeclRefExpr>(E)) {
    const auto *ECD = dyn_cast<EnumConstantDecl>(DRE->getDecl());
    return ECD && ECD->getName() == ConstantName;
  }

  return false;
}

static bool isPointerToRecordNamed(QualType QT, StringRef RecordName) {
  const auto *PT = QT->getAs<PointerType>();
  if (!PT)
    return false;

  const auto *RT = PT->getPointeeType()->getAs<RecordType>();
  if (!RT)
    return false;

  const RecordDecl *RD = RT->getDecl();
  return RD && RD->getName() == RecordName;
}

static bool isInNetlinkDumpCallback(const FunctionDecl *FD) {
  if (!FD)
    return false;

  for (const ParmVarDecl *Param : FD->parameters()) {
    if (isPointerToRecordNamed(Param->getType(), "netlink_callback"))
      return true;
  }

  return false;
}

class SAGenTestChecker : public Checker<check::PreCall> {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker() : BT(new BugType(this, "Incorrect netlink dump response command")) {}

  void checkPreCall(const CallEvent &Call, CheckerContext &C) const;
};

void SAGenTestChecker::checkPreCall(const CallEvent &Call, CheckerContext &C) const {
  const auto *CE = dyn_cast_or_null<CallExpr>(Call.getOriginExpr());
  if (!CE || !isDirectCallTo(CE, "devlink_nl_port_fill") || CE->getNumArgs() < 3)
    return;

  const auto *FD = dyn_cast_or_null<FunctionDecl>(C.getLocationContext()->getDecl());
  if (!isInNetlinkDumpCallback(FD))
    return;

  const Expr *CmdArg = CE->getArg(2);
  if (!isEnumConstantNamed(CmdArg, "DEVLINK_CMD_NEW"))
    return;

  ExplodedNode *ErrNode = C.generateNonFatalErrorNode();
  if (!ErrNode)
    return;

  auto Report = std::make_unique<PathSensitiveBugReport>(
      *BT, "netlink dump response uses generic DEVLINK_CMD_NEW instead of its family-specific response command", ErrNode);
  Report->addRange(CmdArg->getSourceRange());
  C.emitReport(std::move(Report));
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker", 
      "Detects the use of an incorrect command constant (DEVLINK_CMD_NEW) in devlink_nl_port_fill calls", 
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] = CLANG_ANALYZER_API_VERSION_STRING;
