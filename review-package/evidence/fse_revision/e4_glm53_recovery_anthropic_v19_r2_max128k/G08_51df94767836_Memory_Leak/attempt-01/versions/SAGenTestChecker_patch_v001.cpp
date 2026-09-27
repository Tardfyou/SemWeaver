#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Checkers/Taint.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/Environment.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramState.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramStateTrait.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ConstraintManager.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/MemRegion.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/SymExpr.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/StmtVisitor.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/StaticAnalyzer/Checkers/utility.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/Expr.h"
#include "llvm/ADT/StringExtras.h"

using namespace clang;
using namespace ento;
using namespace taint;

// Ownership record for a live net_device obtained from the alloc_netdev family.
// The acquiring frame owns the object until it is released with free_netdev or
// transferred to the network stack with register_netdev.
struct NetdevAllocInfo {
  const LocationContext *Frame; // stack frame that acquired the object
  const CallExpr *AllocCall;    // acquisition call, kept for diagnostics
};

// Region of the allocated net_device -> ownership record.
REGISTER_MAP_WITH_PROGRAMSTATE(NetdevAllocMap, const MemRegion*, NetdevAllocInfo)

namespace {

class SAGenTestChecker : public Checker<check::PostCall, check::EndFunction> {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker()
      : BT(new BugType(this, "Leaked net_device (missing free_netdev)",
                       "Memory Management")) {}

  void checkPostCall(const CallEvent &Call, CheckerContext &C) const;
  void checkEndFunction(const ReturnStmt *RS, CheckerContext &C) const;

private:
  // Acquisition contract: the alloc_netdev family returns a caller-owned
  // net_device.
  static bool isAllocName(StringRef Name);
  // Release/transfer contract: free_netdev releases the object,
  // register_netdev hands it to the network stack.
  static bool isReleaseOrTransferName(StringRef Name);

  // A net_device pointer constrained to NULL on this path means the
  // acquisition itself failed: there is no object to leak.
  static bool isConstrainedNull(ProgramStateRef State, const MemRegion *R) {
    if (const auto *SymR = dyn_cast<SymbolicRegion>(R)) {
      ConditionTruthVal TV =
          State->getConstraintManager().isNull(State, SymR->getSymbol());
      return TV.isConstrainedTrue();
    }
    return false;
  }
};

// Ownership is tracked from the acquisition call itself; no variable,
// label, or loop-scope bookkeeping is needed.

void SAGenTestChecker::checkPostCall(const CallEvent &Call, CheckerContext &C) const {
  const FunctionDecl *Callee = dyn_cast_or_null<FunctionDecl>(Call.getDecl());
  if (!Callee)
    return;
  StringRef Name = Callee->getName();

  ProgramStateRef State = C.getState();

  // Acquisition: a fresh net_device object is owned by the current frame.
  if (isAllocName(Name)) {
    const MemRegion *R = Call.getReturnValue().getAsRegion();
    if (!R)
      return;
    R = R->getBaseRegion();
    const auto *OriginCE = dyn_cast_or_null<CallExpr>(Call.getOriginExpr());
    State = State->set<NetdevAllocMap>(
        R, NetdevAllocInfo{C.getLocationContext(), OriginCE});
    C.addTransition(State);
    return;
  }

  // Release (free_netdev) or ownership transfer (register_netdev): the
  // object is settled, stop tracking it.
  if (isReleaseOrTransferName(Name) && Call.getNumArgs() >= 1) {
    const MemRegion *R = Call.getArgSVal(0).getAsRegion();
    if (!R)
      return;
    R = R->getBaseRegion();
    if (State->get<NetdevAllocMap>(R)) {
      State = State->remove<NetdevAllocMap>(R);
      C.addTransition(State);
    }
  }
}

void SAGenTestChecker::checkEndFunction(const ReturnStmt *RS, CheckerContext &C) const {
  ProgramStateRef State = C.getState();
  auto Map = State->get<NetdevAllocMap>();
  if (Map.isEmpty())
    return;

  // Ownership only has to be settled inside the frame that acquired the
  // object; entries owned by other (e.g. inlined callee) frames are not
  // judged at this exit.
  const LocationContext *CurFrame = C.getLocationContext();

  // If the function hands the object back to its caller, ownership follows
  // the returned value and is not decided here.
  const MemRegion *ReturnedR = nullptr;
  if (RS) {
    if (const Expr *RetE = RS->getRetValue())
      if (const MemRegion *RetReg = C.getSVal(RetE).getAsRegion())
        ReturnedR = RetReg->getBaseRegion();
  }

  SmallVector<const CallExpr *, 4> Leaks;
  bool Changed = false;

  for (auto I = Map.begin(), E = Map.end(); I != E; ++I) {
    if (I->second.Frame != CurFrame)
      continue;

    const MemRegion *R = I->first;
    Changed = true;
    State = State->remove<NetdevAllocMap>(R);

    if (R == ReturnedR)
      continue; // ownership flows back to the caller

    if (isConstrainedNull(State, R))
      continue; // the acquisition failed; no object exists

    Leaks.push_back(I->second.AllocCall);
  }

  if (!Leaks.empty()) {
    // Leaked: the acquiring frame returns without ever releasing the object
    // with free_netdev or transferring it with register_netdev.
    ExplodedNode *N = C.generateNonFatalErrorNode(State);
    if (!N)
      return;
    for (const CallExpr *AllocCE : Leaks) {
      auto Rpt = std::make_unique<PathSensitiveBugReport>(
          *BT,
          "net_device allocated here leaks on this path: it is neither "
          "released with free_netdev nor transferred with register_netdev "
          "before the allocating function returns",
          N);
      if (AllocCE)
        Rpt->addRange(AllocCE->getSourceRange());
      C.emitReport(std::move(Rpt));
    }
    return;
  }

  if (Changed)
    C.addTransition(State);
}

// Helper implementations

// The alloc_netdev family is the acquisition API for a net_device: it returns
// an object owned by the caller.
bool SAGenTestChecker::isAllocName(StringRef Name) {
  return Name == "alloc_netdev" || Name == "alloc_netdev_mqs" ||
         Name == "alloc_etherdev" || Name == "alloc_etherdev_mq" ||
         Name == "alloc_etherdev_mqs";
}

// free_netdev releases the object back to the allocator; register_netdev
// transfers it to the network stack (registered devices are released through
// the driver's cleanup path, not by the immediate caller).
bool SAGenTestChecker::isReleaseOrTransferName(StringRef Name) {
  return Name == "free_netdev" || Name == "register_netdev";
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects missing free_netdev before goto exit in loops (leaks current iteration net_device)",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
