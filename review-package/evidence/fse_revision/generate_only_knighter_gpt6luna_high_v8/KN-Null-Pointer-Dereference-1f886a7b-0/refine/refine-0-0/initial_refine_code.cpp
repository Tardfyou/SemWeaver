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

using namespace clang;
using namespace ento;
using namespace taint;

// Track symbols returned by devm_* allocators. The symbol, unlike the region
// containing a pointer variable, remains the same when the pointer is aliased.
REGISTER_MAP_WITH_PROGRAMSTATE(DevmPtrState, SymbolRef, unsigned)

namespace {

class SAGenTestChecker
    : public Checker<check::PostCall, check::PreCall, check::Location> {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker()
      : BT(new BugType(this, "Unchecked devm allocation dereference",
                       "Null pointer dereference")) {}

  void checkPostCall(const CallEvent &Call, CheckerContext &C) const;
  void checkPreCall(const CallEvent &Call, CheckerContext &C) const;
  void checkLocation(SVal Loc, bool IsLoad, const Stmt *S,
                     CheckerContext &C) const;

private:
  static bool isKnownDevmAllocatorName(StringRef Name);
  static SymbolRef getPtrSymbolFromExpr(const Expr *E, CheckerContext &C);
  static bool canBeNull(SVal V, ProgramStateRef State);

  void reportDerefUnchecked(const Stmt *S, const Expr *BaseE,
                            CheckerContext &C) const;
  void reportPassToDerefUnchecked(const CallEvent &Call, unsigned ArgIdx,
                                  CheckerContext &C) const;

  template <typename T>
  const T *findInParents(const Stmt *S, CheckerContext &C) const {
    return findSpecificTypeInParents<T>(S, C);
  }
};

bool SAGenTestChecker::isKnownDevmAllocatorName(StringRef Name) {
  return Name.equals("devm_kzalloc") || Name.equals("devm_kmalloc") ||
         Name.equals("devm_kcalloc") || Name.equals("devm_kmalloc_array") ||
         Name.equals("devm_kstrdup");
}

SymbolRef SAGenTestChecker::getPtrSymbolFromExpr(const Expr *E,
                                                 CheckerContext &C) {
  if (!E)
    return nullptr;

  SVal V = C.getSVal(E->IgnoreParenCasts());
  return V.getAsSymbol();
}

// Return true only when the current path still has a feasible null value.
// This lets the analyzer's constraints from a successful null check suppress
// reports on the non-null branch.
bool SAGenTestChecker::canBeNull(SVal V, ProgramStateRef State) {
  if (V.isUnknownOrUndef())
    return false;

  DefinedOrUnknownSVal DV = V.castAs<DefinedOrUnknownSVal>();
  return static_cast<bool>(State->assume(DV, false));
}

void SAGenTestChecker::reportDerefUnchecked(const Stmt *S, const Expr *BaseE,
                                           CheckerContext &C) const {
  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto R = std::make_unique<PathSensitiveBugReport>(
      *BT, "Unchecked devm allocation may be NULL and is dereferenced", N);
  if (S)
    R->addRange(S->getSourceRange());
  if (BaseE)
    R->addRange(BaseE->getSourceRange());
  C.emitReport(std::move(R));
}

void SAGenTestChecker::reportPassToDerefUnchecked(const CallEvent &Call,
                                                 unsigned ArgIdx,
                                                 CheckerContext &C) const {
  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto R = std::make_unique<PathSensitiveBugReport>(
      *BT,
      "Unchecked devm allocation may be NULL and is passed to a function "
      "that dereferences it",
      N);
  if (const Expr *OE = Call.getOriginExpr())
    R->addRange(OE->getSourceRange());
  if (ArgIdx < Call.getNumArgs()) {
    if (const Expr *AE = Call.getArgExpr(ArgIdx))
      R->addRange(AE->getSourceRange());
  }
  C.emitReport(std::move(R));
}

// Track the symbolic value returned by a recognized devm_* allocator.
void SAGenTestChecker::checkPostCall(const CallEvent &Call,
                                     CheckerContext &C) const {
  const IdentifierInfo *ID = Call.getCalleeIdentifier();
  if (!ID || !isKnownDevmAllocatorName(ID->getName()))
    return;

  SymbolRef RetSym = Call.getReturnValue().getAsSymbol();
  if (!RetSym)
    return;

  ProgramStateRef State = C.getState()->set<DevmPtrState>(RetSym, 0);
  C.addTransition(State);
}

// Report passing a tracked allocation to a function known to dereference
// the corresponding argument, provided that null remains feasible.
void SAGenTestChecker::checkPreCall(const CallEvent &Call,
                                    CheckerContext &C) const {
  llvm::SmallVector<unsigned, 4> DerefParams;
  if (!functionKnownToDeref(Call, DerefParams))
    return;

  ProgramStateRef State = C.getState();

  for (unsigned Idx : DerefParams) {
    if (Idx >= Call.getNumArgs())
      continue;

    const Expr *ArgE = Call.getArgExpr(Idx);
    SymbolRef Sym = getPtrSymbolFromExpr(ArgE, C);
    if (!Sym || !State->get<DevmPtrState>(Sym))
      continue;

    SVal ArgV = C.getSVal(ArgE);
    if (canBeNull(ArgV, State))
      reportPassToDerefUnchecked(Call, Idx, C);
  }
}

// Detect pointer dereferences such as p->field, *p, and p[index].
// A load of an array slot used by a null check is not itself a dereference of
// the allocation stored in that slot: its base expression has no tracked
// allocation symbol. Once the loaded pointer is aliased, its symbol is
// preserved and is detected at the actual dereference.
void SAGenTestChecker::checkLocation(SVal Loc, bool IsLoad, const Stmt *S,
                                    CheckerContext &C) const {
  (void)Loc;
  (void)IsLoad;

  if (!S)
    return;

  const Expr *BaseE = nullptr;

  if (const auto *ME = findInParents<MemberExpr>(S, C)) {
    if (ME->isArrow())
      BaseE = ME->getBase()->IgnoreParenCasts();
  }

  if (!BaseE) {
    if (const auto *UO = findInParents<UnaryOperator>(S, C)) {
      if (UO->getOpcode() == UO_Deref)
        BaseE = UO->getSubExpr()->IgnoreParenCasts();
    }
  }

  if (!BaseE) {
    if (const auto *ASE = findInParents<ArraySubscriptExpr>(S, C))
      BaseE = ASE->getBase()->IgnoreParenCasts();
  }

  if (!BaseE)
    return;

  SymbolRef Sym = getPtrSymbolFromExpr(BaseE, C);
  if (!Sym || !C.getState()->get<DevmPtrState>(Sym))
    return;

  SVal BaseV = C.getSVal(BaseE);
  if (canBeNull(BaseV, C.getState()))
    reportDerefUnchecked(S, BaseE, C);
}

} // namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects dereference of devm_* allocation without NULL check", "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
