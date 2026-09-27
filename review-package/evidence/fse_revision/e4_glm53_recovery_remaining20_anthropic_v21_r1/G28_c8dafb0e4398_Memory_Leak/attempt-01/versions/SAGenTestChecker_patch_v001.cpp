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
#include "clang/AST/Stmt.h"

using namespace clang;
using namespace ento;
using namespace taint;

// Program states: HWRM arena request objects that hwrm_req_replace() has
// taken into use and that must be released by hwrm_req_drop() before the
// owning function returns. Tracked by both the request pointer symbol and
// its region so the inline arena wrappers and the underlying __hwrm_*
// implementations refer to the same object.
REGISTER_SET_WITH_PROGRAMSTATE(ReqOwnedSyms, SymbolRef)
REGISTER_SET_WITH_PROGRAMSTATE(ReqOwnedRegions, const MemRegion*)

namespace {

class SAGenTestChecker
  : public Checker<
        check::BeginFunction,
        check::PostCall,
        check::PreCall,
        check::PreStmt<ReturnStmt>,
        check::EndFunction> {

  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker()
      : BT(std::make_unique<BugType>(this,
                                     "Missing hwrm_req_drop on error path",
                                     "Resource Management")) {}

  void checkBeginFunction(CheckerContext &C) const;
  void checkPostCall(const CallEvent &Call, CheckerContext &C) const;
  void checkPreCall(const CallEvent &Call, CheckerContext &C) const;
  void checkPreStmt(const ReturnStmt *RS, CheckerContext &C) const;
  void checkEndFunction(const ReturnStmt *RS, CheckerContext &C) const;

private:
  // Helper: callee name with leading underscores stripped, so both the arena
  // wrapper (hwrm_req_replace) and its implementation (__hwrm_req_replace)
  // are recognized when the analyzer inlines or macro-expands the wrapper.
  static StringRef normalizedCalleeName(const CallEvent &Call);

  // Helper: true when this call is the named HWRM arena request API. The
  // arena API itself defines the ownership contract: replace takes the
  // request into use, drop releases it.
  static bool isHwrmReqApi(const CallEvent &Call, StringRef Base);

  // Helpers: identify the request object passed right after the bnxt handle
  // (argument index 1), by pointer symbol and by region.
  static SymbolRef reqArgSymbol(const CallEvent &Call);
  static const MemRegion *reqArgRegion(const CallEvent &Call);

  // Report a missing-drop issue at the given statement; returns the error
  // node so the caller can continue the path with ownership cleared.
  ExplodedNode *reportMissingDrop(const Stmt *S, CheckerContext &C) const;
};

// Helper implementations

StringRef SAGenTestChecker::normalizedCalleeName(const CallEvent &Call) {
  StringRef Name;
  if (const IdentifierInfo *ID = Call.getCalleeIdentifier())
    Name = ID->getName();
  else if (const auto *FD = dyn_cast_or_null<FunctionDecl>(Call.getDecl())) {
    if (const IdentifierInfo *II = FD->getIdentifier())
      Name = II->getName();
  }
  while (Name.starts_with("_"))
    Name = Name.drop_front();
  return Name;
}

bool SAGenTestChecker::isHwrmReqApi(const CallEvent &Call, StringRef Base) {
  return normalizedCalleeName(Call) == Base;
}

SymbolRef SAGenTestChecker::reqArgSymbol(const CallEvent &Call) {
  if (Call.getNumArgs() < 2)
    return nullptr;
  return Call.getArgSVal(1).getAsSymbol();
}

const MemRegion *SAGenTestChecker::reqArgRegion(const CallEvent &Call) {
  if (Call.getNumArgs() < 2)
    return nullptr;
  if (auto L = Call.getArgSVal(1).getAs<Loc>())
    return L->getAsRegion();
  return nullptr;
}

ExplodedNode *SAGenTestChecker::reportMissingDrop(const Stmt *S, CheckerContext &C) const {
  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return nullptr;
  auto R = std::make_unique<PathSensitiveBugReport>(
      *BT, "Request not released: missing hwrm_req_drop() before return", N);
  if (S)
    R->addRange(S->getSourceRange());
  C.emitReport(std::move(R));
  return N;
}

// Callbacks

void SAGenTestChecker::checkBeginFunction(CheckerContext &C) const {
  // Owned-request state starts empty for each top-level analysis and must
  // not be reset here: this callback also runs when the analyzer enters an
  // inlined arena helper, which would erase the caller's live ownership.
}

void SAGenTestChecker::checkPostCall(const CallEvent &Call, CheckerContext &C) const {
  // hwrm_req_drop() releases the arena request: clear its ownership so the
  // following returns of the owning function are silent.
  if (isHwrmReqApi(Call, "hwrm_req_drop")) {
    ProgramStateRef State = C.getState();
    bool Changed = false;
    if (SymbolRef S = reqArgSymbol(Call)) {
      State = State->remove<ReqOwnedSyms>(S);
      Changed = true;
    }
    if (const MemRegion *R = reqArgRegion(Call)) {
      State = State->remove<ReqOwnedRegions>(R);
      Changed = true;
    }
    if (Changed)
      C.addTransition(State);
    return;
  }
}

void SAGenTestChecker::checkPreCall(const CallEvent &Call, CheckerContext &C) const {
  // hwrm_req_replace() takes the arena request into use on both of its
  // outcomes: if it fails, the caller still owns the request and must drop
  // it before returning. Marking happens before the branch on its return
  // value, so both successors carry the ownership.
  if (!isHwrmReqApi(Call, "hwrm_req_replace"))
    return;

  ProgramStateRef State = C.getState();
  bool Changed = false;
  if (SymbolRef S = reqArgSymbol(Call)) {
    if (!State->contains<ReqOwnedSyms>(S)) {
      State = State->add<ReqOwnedSyms>(S);
      Changed = true;
    }
  }
  if (const MemRegion *R = reqArgRegion(Call)) {
    if (!State->contains<ReqOwnedRegions>(R)) {
      State = State->add<ReqOwnedRegions>(R);
      Changed = true;
    }
  }
  if (Changed)
    C.addTransition(State);
}

void SAGenTestChecker::checkPreStmt(const ReturnStmt *RS, CheckerContext &C) const {
  ProgramStateRef State = C.getState();
  // If any req is marked acquired (replace seen, no drop yet), warn.
  auto M = State->get<ReqAcquiredMap>();
  if (!M.isEmpty()) {
    reportMissingDrop(RS, C);
  }
}

void SAGenTestChecker::checkEndFunction(const ReturnStmt *RS, CheckerContext &C) const {
  ProgramStateRef State = C.getState();
  auto M = State->get<ReqAcquiredMap>();
  if (!M.isEmpty()) {
    // Function end without proper drop.
    reportMissingDrop(RS, C);
  }
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detect missing hwrm_req_drop() on error paths after hwrm_req_replace()",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
