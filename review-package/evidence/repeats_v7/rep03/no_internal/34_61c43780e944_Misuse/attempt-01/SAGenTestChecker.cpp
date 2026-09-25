// /external/upstream-study/SAGEN/result-0224-bugfail-multi-o3mini/test-Misuse-61c43780e9444123410cd48c2483e01d2b8f75e8/checkers/checker0.cpp
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"

#include <memory>
#include <utility>

using namespace clang;
using namespace ento;

namespace {

class SAGenTestChecker : public Checker<check::PreCall> {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker() : BT(new BugType(this, "Incorrect port command")) {}

  void checkPreCall(const CallEvent &Call, CheckerContext &C) const;

private:
  static const EnumConstantDecl *getCommandConstant(const Expr *Arg) {
    Arg = Arg->IgnoreParenImpCasts();
    const auto *Ref = dyn_cast<DeclRefExpr>(Arg);
    if (!Ref)
      return nullptr;
    return dyn_cast<EnumConstantDecl>(Ref->getDecl());
  }
};

void SAGenTestChecker::checkPreCall(const CallEvent &Call,
                                    CheckerContext &C) const {
  const Expr *OriginExpr = Call.getOriginExpr();
  const auto *CE = dyn_cast_or_null<CallExpr>(OriginExpr);
  if (!CE || CE->getNumArgs() < 3)
    return;

  const FunctionDecl *Callee = CE->getDirectCallee();
  if (!Callee || Callee->getName() != "devlink_nl_port_fill")
    return;

  const Expr *CmdArg = CE->getArg(2);
  const EnumConstantDecl *Command = getCommandConstant(CmdArg);
  if (!Command || Command->getName() != "DEVLINK_CMD_NEW")
    return;

  ExplodedNode *ErrNode = C.generateNonFatalErrorNode();
  if (!ErrNode)
    return;

  auto Report = std::make_unique<PathSensitiveBugReport>(
      *BT,
      "Port fill uses DEVLINK_CMD_NEW instead of DEVLINK_CMD_PORT_NEW",
      ErrNode);
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
