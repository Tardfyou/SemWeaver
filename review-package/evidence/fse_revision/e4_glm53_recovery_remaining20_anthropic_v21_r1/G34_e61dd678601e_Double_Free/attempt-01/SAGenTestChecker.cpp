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
#include "clang/Lex/Lexer.h"

using namespace clang;
using namespace ento;
using namespace taint;

// Program state: storage regions of struct-member buffers that have already
// been handed to a memory-free call; a second free of the same member region
// of the same base object is a double free.
REGISTER_SET_WITH_PROGRAMSTATE(FreedMemberBuffers, const MemRegion *)

namespace {

class SAGenTestChecker : public Checker<check::PreCall> {
   mutable std::unique_ptr<BugType> BT;

   public:
      SAGenTestChecker() : BT(new BugType(this, "Double free of member", "Memory Management")) {}

      void checkPreCall(const CallEvent &Call, CheckerContext &C) const;

   private:
      // Helpers
      static bool isMemoryFreeCall(const CallEvent &Call);
      static const MemberExpr *findMemberAccess(const Expr *E);

      const MemRegion *getMemberBufferRegion(const CallEvent &Call,
                                             CheckerContext &C) const;

      void reportDoubleFree(const CallEvent &Call, CheckerContext &C) const;
};

// A call to a memory-free entry point (kfree/kvfree/vfree).
bool SAGenTestChecker::isMemoryFreeCall(const CallEvent &Call) {
  const auto *FD = dyn_cast_or_null<FunctionDecl>(Call.getDecl());
  if (!FD)
    return false;

  const IdentifierInfo *II = FD->getIdentifier();
  if (!II)
    return false;

  StringRef Name = II->getName();
  return Name == "kfree" || Name == "kvfree" || Name == "vfree";
}

// Find a struct-member access in an expression, e.g. ca->buckets_nouse.
const MemberExpr *SAGenTestChecker::findMemberAccess(const Expr *E) {
  if (!E)
    return nullptr;

  E = E->IgnoreParenImpCasts();
  if (const auto *ME = dyn_cast<MemberExpr>(E))
    return ME;

  // Macro expansions may wrap the member access in casts or other operators.
  for (const Stmt *Child : E->children())
    if (const auto *CE = dyn_cast<Expr>(Child))
      if (const MemberExpr *ME = findMemberAccess(CE))
        return ME;

  return nullptr;
}

// If the freed pointer is owned through a struct member of a base object
// (e.g. ca->buckets_nouse), return the storage region of that member inside
// its base object. The region identifies the same owned buffer even across
// inlined call frames and independently of which free API releases it.
const MemRegion *SAGenTestChecker::getMemberBufferRegion(const CallEvent &Call,
                                                         CheckerContext &C) const {
  if (Call.getNumArgs() == 0)
    return nullptr;

  const MemberExpr *ME = findMemberAccess(Call.getArgExpr(0));
  if (!ME)
    return nullptr; // free of a pointer not reached through a struct member

  const auto *Field = dyn_cast<FieldDecl>(ME->getMemberDecl());
  if (!Field)
    return nullptr;

  const Expr *BaseE = ME->getBase();
  if (!BaseE)
    return nullptr;

  SVal BaseV = C.getSVal(BaseE);
  if (!BaseV.getAsRegion())
    return nullptr;

  SVal FieldLoc = C.getState()->getLValue(Field, BaseV);
  return FieldLoc.getAsRegion();
}

// Report a double free
void SAGenTestChecker::reportDoubleFree(const CallEvent &Call, CheckerContext &C) const {
  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto R = std::make_unique<PathSensitiveBugReport>(
      *BT, "Member buffer freed more than once", N);
  R->addRange(Call.getSourceRange());
  C.emitReport(std::move(R));
}

void SAGenTestChecker::checkPreCall(const CallEvent &Call, CheckerContext &C) const {
  if (!isMemoryFreeCall(Call))
    return;

  const MemRegion *MemberR = getMemberBufferRegion(Call, C);
  if (!MemberR)
    return;

  ProgramStateRef State = C.getState();

  // A second free of the same member storage of the same base object is a
  // double free: the first free already released the buffer it owns.
  if (State->contains<FreedMemberBuffers>(MemberR)) {
    reportDoubleFree(Call, C);
    return;
  }

  State = State->add<FreedMemberBuffers>(MemberR);
  C.addTransition(State);
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects double free of ca->buckets_nouse when both kfree(member) and bch2_dev_buckets_free(ca) are called",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
