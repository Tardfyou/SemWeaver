// /external/upstream-study/SAGEN/result-0224-bugfail-multi-o3mini/test-Misuse-61c43780e9444123410cd48c2483e01d2b8f75e8/checkers/checker0.cpp
#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Checkers/Taint.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/StmtVisitor.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/StaticAnalyzer/Checkers/utility.h"
#include "clang/Lex/Lexer.h"  // For Lexer::getSourceText

using namespace clang;
using namespace ento;
using namespace taint;

namespace {

class SAGenTestChecker : public Checker<check::PreCall> {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker() : BT(new BugType(this, "Incorrect command constant")) {}

  void checkPreCall(const CallEvent &Call, CheckerContext &C) const;

private:
  // (Optional) You can add helper functions here if needed.
};

void SAGenTestChecker::checkPreCall(const CallEvent &Call, CheckerContext &C) const {
  const FunctionDecl *Callee = dyn_cast_or_null<FunctionDecl>(Call.getDecl());
  if (!Callee || Callee->getName() != "devlink_nl_port_fill" ||
      Callee->getNumParams() < 3)
    return;

  const EnumType *CommandType =
      Callee->getParamDecl(2)->getType()->getAs<EnumType>();
  if (!CommandType || CommandType->getDecl()->getName() != "devlink_command")
    return;

  const Expr *OriginExpr = Call.getOriginExpr();
  const CallExpr *CE = dyn_cast_or_null<CallExpr>(OriginExpr);
  if (!CE || CE->getNumArgs() < 3)
    return;

  const Expr *CmdArg = CE->getArg(2)->IgnoreParenImpCasts();
  const auto *Command = dyn_cast<DeclRefExpr>(CmdArg);
  const auto *CommandDecl =
      Command ? dyn_cast<EnumConstantDecl>(Command->getDecl()) : nullptr;
  if (!CommandDecl || CommandDecl->getName() != "DEVLINK_CMD_NEW")
    return;

  ExplodedNode *ErrNode = C.generateNonFatalErrorNode();
  if (!ErrNode)
    return;

  auto Report = std::make_unique<PathSensitiveBugReport>(
      *BT, "Incorrect command constant: DEVLINK_CMD_NEW used instead of DEVLINK_CMD_PORT_NEW",
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
