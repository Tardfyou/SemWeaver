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
#include "clang/AST/Type.h"

using namespace clang;
using namespace ento;
using namespace taint;

// No custom program state needed.

namespace {

class SAGenTestChecker : public Checker<check::PreCall> {
   mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker()
      : BT(new BugType(this, "Overflow check defeated by narrowing cast",
                       "Integer")) {}

  void checkPreCall(const CallEvent &Call, CheckerContext &C) const;

private:
  // Returns the ExplicitCastExpr that is suspicious, or nullptr if none.
  const ExplicitCastExpr *isSuspiciousCastArg(const Expr *ArgE,
                                              QualType ResPointeeCanonical,
                                              uint64_t ResWidthBits,
                                              ASTContext &ACtx) const;
};

const ExplicitCastExpr *SAGenTestChecker::isSuspiciousCastArg(
    const Expr *ArgE, QualType ResPointeeCanonical, uint64_t ResWidthBits,
    ASTContext &ACtx) const {
  if (!ArgE)
    return nullptr;

  // Do not ignore explicit casts, but do ignore parens.
  const Expr *E = ArgE->IgnoreParens();
  const auto *CE = dyn_cast<ExplicitCastExpr>(E);
  if (!CE)
    return nullptr;

  QualType CastDestTy = CE->getType().getCanonicalType();
  if (!CastDestTy->isIntegerType() || !CastDestTy->isSignedIntegerType())
    return nullptr;

  if (CastDestTy != ResPointeeCanonical)
    return nullptr;

  const Expr *Sub = CE->getSubExpr()->IgnoreImpCasts();
  if (!Sub)
    return nullptr;

  QualType SubTy = Sub->getType().getCanonicalType();
  if (!SubTy->isIntegerType() || !SubTy->isUnsignedIntegerType())
    return nullptr;

  uint64_t SubWidthBits = ACtx.getTypeSize(SubTy);
  if (SubWidthBits <= ResWidthBits)
    return nullptr;

  return CE;
}

void SAGenTestChecker::checkPreCall(const CallEvent &Call,
                                    CheckerContext &C) const {
  const Expr *Origin = Call.getOriginExpr();
  if (!Origin)
    return;

  // check_add_overflow() is a macro expanding to
  // __must_check_overflow(__builtin_add_overflow(...)); after preprocessing
  // the analyzer only sees the builtin call, so accept both spellings of the
  // overflow-check sink. The warning predicate stays the numeric width
  // relation checked below, not this name filter.
  const auto *CE = dyn_cast<CallExpr>(Origin);
  if (!CE)
    return;
  const FunctionDecl *CalleeFD = CE->getDirectCallee();
  if (!CalleeFD)
    return;
  const IdentifierInfo *CalleeII = CalleeFD->getIdentifier();
  if (!CalleeII)
    return;
  StringRef CalleeName = CalleeII->getName();
  if (CalleeName != "__builtin_add_overflow" &&
      CalleeName != "check_add_overflow")
    return;

  if (Call.getNumArgs() != 3)
    return;

  // Get result argument type: it must be pointer to signed integer type.
  const Expr *ResArgE = Call.getArgExpr(2);
  if (!ResArgE)
    return;

  QualType ResArgTy = ResArgE->getType();
  const Type *ResArgTyPtr = ResArgTy.getTypePtrOrNull();
  if (!ResArgTyPtr || !ResArgTyPtr->isPointerType())
    return;

  QualType ResPointeeTy = ResArgTy->getPointeeType();
  if (ResPointeeTy.isNull())
    return;

  if (!ResPointeeTy->isIntegerType() || !ResPointeeTy->isSignedIntegerType())
    return;

  ASTContext &ACtx = C.getASTContext();
  QualType ResPointeeCanonical = ResPointeeTy.getCanonicalType();
  uint64_t ResWidthBits = ACtx.getTypeSize(ResPointeeCanonical);

  // Check first two operands for suspicious explicit narrowing cast.
  const ExplicitCastExpr *BadCast = nullptr;
  for (unsigned i = 0; i < 2; ++i) {
    const Expr *ArgE = Call.getArgExpr(i);
    const ExplicitCastExpr *Candidate =
        isSuspiciousCastArg(ArgE, ResPointeeCanonical, ResWidthBits, ACtx);
    if (Candidate) {
      BadCast = Candidate;
      break; // Report once per callsite.
    }
  }

  if (!BadCast)
    return;

  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto R = std::make_unique<PathSensitiveBugReport>(
      *BT, "Narrowing cast to signed type before check_add_overflow may hide "
           "overflow; avoid casting size-related values to int.",
      N);
  R->addRange(BadCast->getSourceRange());
  C.emitReport(std::move(R));
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects narrowing signed casts to match result type before "
      "check_add_overflow that may hide overflow",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
