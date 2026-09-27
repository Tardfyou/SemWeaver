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
#include "clang/AST/Type.h"
#include "clang/Lex/Lexer.h"

using namespace clang;
using namespace ento;
using namespace taint;

namespace {

class SAGenTestChecker : public Checker<check::PostCall, check::EndFunction> {
  mutable std::unique_ptr<BugType> BT;

  // Retains the existing translation-unit gate for timed-wait patterns.
  mutable bool SawTimedWaitInTU = false;

public:
  SAGenTestChecker()
      : BT(new BugType(this, "Workqueue timed-wait UAF risk", "Concurrency")) {}

  void checkPostCall(const CallEvent &Call, CheckerContext &C) const;
  void checkEndFunction(const ReturnStmt *RS, CheckerContext &C) const;

private:
  bool callIs(const CallEvent &Call, StringRef Name) const;
  bool isWorkerFunction(const FunctionDecl *FD) const;
  bool workerHasRelevantContextUse(const FunctionDecl *FD) const;
  bool workerHasMatchingCompletionDone(const FunctionDecl *FD) const;
  void reportMissingGuard(const Stmt *S, CheckerContext &C) const;
};

// Return the first variable referenced by an expression. This handles common
// forms such as ctx, &ctx->compl, and casts around either expression.
class ReferencedVarVisitor : public RecursiveASTVisitor<ReferencedVarVisitor> {
public:
  const VarDecl *ReferencedVar = nullptr;

  bool VisitDeclRefExpr(const DeclRefExpr *DRE) {
    if (!ReferencedVar)
      ReferencedVar = dyn_cast<VarDecl>(DRE->getDecl());
    return ReferencedVar == nullptr;
  }
};

static const VarDecl *getReferencedVar(const Expr *E) {
  if (!E)
    return nullptr;

  ReferencedVarVisitor Visitor;
  Visitor.TraverseStmt(const_cast<Expr *>(E->IgnoreParenImpCasts()));
  return Visitor.ReferencedVar
             ? Visitor.ReferencedVar->getCanonicalDecl()
             : nullptr;
}

static StringRef getDirectCalleeName(const CallExpr *CE) {
  if (!CE)
    return {};

  const FunctionDecl *FD = CE->getDirectCallee();
  if (!FD || !FD->getIdentifier())
    return {};

  return FD->getName();
}

bool SAGenTestChecker::callIs(const CallEvent &Call, StringRef Name) const {
  const IdentifierInfo *ID = Call.getCalleeIdentifier();
  return ID && ID->getName() == Name;
}

bool SAGenTestChecker::isWorkerFunction(const FunctionDecl *FD) const {
  if (!FD)
    return false;

  for (const ParmVarDecl *P : FD->parameters()) {
    QualType QT = P->getType();
    if (!QT->isPointerType())
      continue;

    QualType Pointee = QT->getPointeeType();
    if (const RecordType *RT = Pointee->getAs<RecordType>()) {
      const RecordDecl *RD = RT->getDecl();
      const IdentifierInfo *II = RD->getIdentifier();
      if (II && II->getName() == "work_struct")
        return true;
    }
  }

  return false;
}

class ContextUseVisitor
    : public RecursiveASTVisitor<ContextUseVisitor> {
public:
  llvm::SmallPtrSet<const VarDecl *, 8> Contexts;

  bool VisitCallExpr(const CallExpr *CE) {
    StringRef Name = getDirectCalleeName(CE);
    if (Name != "kfree" && Name != "complete")
      return true;

    if (CE->getNumArgs() == 0)
      return true;

    if (const VarDecl *VD = getReferencedVar(CE->getArg(0)))
      Contexts.insert(VD);

    return true;
  }
};

class CompletionDoneVisitor
    : public RecursiveASTVisitor<CompletionDoneVisitor> {
public:
  llvm::SmallPtrSet<const VarDecl *, 8> Contexts;

  bool VisitCallExpr(const CallExpr *CE) {
    if (getDirectCalleeName(CE) != "completion_done" ||
        CE->getNumArgs() == 0)
      return true;

    if (const VarDecl *VD = getReferencedVar(CE->getArg(0)))
      Contexts.insert(VD);

    return true;
  }
};

bool SAGenTestChecker::workerHasRelevantContextUse(
    const FunctionDecl *FD) const {
  if (!FD || !FD->getBody())
    return false;

  ContextUseVisitor Visitor;
  Visitor.TraverseStmt(const_cast<Stmt *>(FD->getBody()));
  return !Visitor.Contexts.empty();
}

bool SAGenTestChecker::workerHasMatchingCompletionDone(
    const FunctionDecl *FD) const {
  if (!FD || !FD->getBody())
    return false;

  ContextUseVisitor Uses;
  Uses.TraverseStmt(const_cast<Stmt *>(FD->getBody()));
  if (Uses.Contexts.empty())
    return false;

  CompletionDoneVisitor DoneCalls;
  DoneCalls.TraverseStmt(const_cast<Stmt *>(FD->getBody()));

  for (const VarDecl *Context : Uses.Contexts) {
    if (DoneCalls.Contexts.contains(Context))
      return true;
  }

  return false;
}

void SAGenTestChecker::checkPostCall(const CallEvent &Call,
                                     CheckerContext &C) const {
  if (callIs(Call, "wait_for_completion_timeout"))
    SawTimedWaitInTU = true;
}

void SAGenTestChecker::reportMissingGuard(const Stmt *S,
                                          CheckerContext &C) const {
  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto R = std::make_unique<PathSensitiveBugReport>(
      *BT,
      "Missing completion_done() guard in worker; submitter with timed wait "
      "may free the context",
      N);

  if (S)
    R->addRange(S->getSourceRange());

  C.emitReport(std::move(R));
}

void SAGenTestChecker::checkEndFunction(const ReturnStmt *RS,
                                        CheckerContext &C) const {
  const LocationContext *LCtx = C.getLocationContext();
  const FunctionDecl *FD =
      dyn_cast_or_null<FunctionDecl>(LCtx ? LCtx->getDecl() : nullptr);

  if (!isWorkerFunction(FD) || !SawTimedWaitInTU)
    return;

  if (!workerHasRelevantContextUse(FD))
    return;

  // A completion_done() on the same context is evidence that the worker
  // coordinates its ownership with the timed-wait submitter. Do not let an
  // unrelated completion_done() call suppress this warning.
  if (workerHasMatchingCompletionDone(FD))
    return;

  reportMissingGuard(RS ? static_cast<const Stmt *>(RS) : FD->getBody(), C);
}

} // namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects missing completion_done() guard in workqueue worker that may "
      "race with submitter timeout and free",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
