#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Type.h"
#include "clang/AST/Decl.h"

using namespace clang;
using namespace ento;

namespace {

class SAGenTestChecker : public Checker<check::PreCall> {
   mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker()
      : BT(new BugType(this, "Unbounded string copy into fixed-size buffer",
                       "Security")) {}

  void checkPreCall(const CallEvent &Call, CheckerContext &C) const;

private:
  // Return true if the call is to strcpy.
  static bool isStrcpy(const CallEvent &Call, CheckerContext &C);

  // Try to obtain a constant array size from an expression that refers to an
  // array variable or a struct/union field of array type.
  static bool getConstArraySize(llvm::APInt &Size, const Expr *E, CheckerContext &C);

  // Emit a diagnostic with a short message.
  void report(const CallEvent &Call, CheckerContext &C, StringRef Msg) const;
};

bool SAGenTestChecker::isStrcpy(const CallEvent &Call, CheckerContext &C) {
  // Sink contract: strcpy(dest, src) copies bytes until the source's NUL
  // terminator with no bound on the destination capacity.
  const auto *FD = dyn_cast_or_null<FunctionDecl>(Call.getDecl());
  if (!FD)
    return false;
  const IdentifierInfo *II = FD->getIdentifier();
  return II && II->isStr("strcpy");
}

bool SAGenTestChecker::getConstArraySize(llvm::APInt &Size, const Expr *E, CheckerContext &C) {
  if (!E)
    return false;

  const Expr *EI = E->IgnoreParenImpCasts();

  // Array-typed local or parameter variable (the call argument decays to a
  // pointer to its first element).
  if (const auto *DR = dyn_cast<DeclRefExpr>(EI)) {
    if (const auto *VD = dyn_cast<VarDecl>(DR->getDecl())) {
      QualType QT = VD->getType();
      if (const auto *CAT = dyn_cast<ConstantArrayType>(QT.getTypePtr())) {
        Size = CAT->getSize();
        return true;
      }
    }
    return false;
  }

  // Try struct/union member that is an array field.
  if (const auto *ME = dyn_cast<MemberExpr>(EI)) {
    const ValueDecl *VD = ME->getMemberDecl();
    const auto *FD = dyn_cast_or_null<FieldDecl>(VD);
    if (!FD)
      return false;

    QualType QT = FD->getType();
    if (const auto *CAT = dyn_cast<ConstantArrayType>(QT.getTypePtr())) {
      Size = CAT->getSize();
      return true;
    }
  }

  return false;
}

void SAGenTestChecker::report(const CallEvent &Call, CheckerContext &C, StringRef Msg) const {
  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto R = std::make_unique<PathSensitiveBugReport>(*BT, Msg, N);
  if (const Stmt *S = Call.getOriginExpr())
    R->addRange(S->getSourceRange());
  C.emitReport(std::move(R));
}

void SAGenTestChecker::checkPreCall(const CallEvent &Call, CheckerContext &C) const {
  if (!isStrcpy(Call, C))
    return;

  // Extract destination and source expressions.
  if (Call.getNumArgs() < 2)
    return;

  const Expr *DstArg = Call.getArgExpr(0);
  const Expr *SrcArg = Call.getArgExpr(1);
  if (!DstArg || !SrcArg)
    return;

  // Determine destination fixed-size.
  llvm::APInt DstSize(64, 0);
  if (!getConstArraySize(DstSize, DstArg, C)) {
    // If destination size is unknown, skip to reduce false positives.
    return;
  }

  // Try to evaluate source length if it's a string literal.
  llvm::APInt SrcLen(64, 0);
  if (const auto *Lit = dyn_cast<StringLiteral>(SrcArg->IgnoreParenImpCasts())) {
    SrcLen = llvm::APInt(64, Lit->getLength());
    // getLength() returns number of chars, not including the null terminator.
    // strcpy needs at least SrcLen + 1 bytes in destination.
    if (SrcLen.getZExtValue() >= DstSize.getZExtValue()) {
      // Definite overflow.
      SmallString<128> Msg;
      {
        llvm::raw_svector_ostream OS(Msg);
        OS << "strcpy overflows fixed-size buffer (dest size "
           << DstSize.getZExtValue() << ", source length "
           << SrcLen.getZExtValue() << ")";
      }
      report(Call, C, Msg);
    }
    // Else, provably safe for this literal.
    return;
  }

  // Source is not a literal. Try to detect a bounded source array.
  llvm::APInt SrcBound(64, 0);
  if (getConstArraySize(SrcBound, SrcArg, C)) {
    // The source is itself a fixed-size array: its contents are not guaranteed
    // to be NUL-terminated within the array, so when the source capacity is at
    // least the destination capacity the unbounded strcpy can overflow the
    // destination; an explicit bound such as strscpy(dst, src, sizeof(dst)) is
    // required to establish the capacity relation.
    if (SrcBound.getZExtValue() >= DstSize.getZExtValue()) {
      SmallString<128> Msg;
      {
        llvm::raw_svector_ostream OS(Msg);
        OS << "Possible overflow: strcpy into fixed-size buffer (dest size "
           << DstSize.getZExtValue() << ", source bound "
           << SrcBound.getZExtValue()
           << "); use strscpy(dest, src, sizeof(dest))";
      }
      report(Call, C, Msg);
      return;
    } else {
      // Still potentially unsafe as strcpy is unbounded.
      report(Call, C, "Possible overflow: strcpy into fixed-size buffer; use strscpy(dest, src, sizeof(dest))");
      return;
    }
  }

  // Unknown source size: warn about possible overflow due to unbounded copy.
  report(Call, C, "Possible overflow: strcpy into fixed-size buffer; use strscpy(dest, src, sizeof(dest))");
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects strcpy into fixed-size buffers which may overflow; suggests strscpy with sizeof(dest)",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
