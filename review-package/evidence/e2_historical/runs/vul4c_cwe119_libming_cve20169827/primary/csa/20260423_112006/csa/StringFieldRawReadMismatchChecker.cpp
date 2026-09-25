#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/Version.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/CheckerManager.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/MemRegion.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramStateTrait.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/SVals.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"
#include <memory>

using namespace clang;
using namespace ento;

namespace {

class StringFieldRawReadMismatchChecker;

} // namespace

REGISTER_SET_WITH_PROGRAMSTATE(TaintedStringFieldRegions, const MemRegion *)

namespace {

static bool isZeroIntegerLiteral(const Expr *E) {
  E = E ? E->IgnoreParenCasts() : nullptr;
  if (const auto *IL = dyn_cast_or_null<IntegerLiteral>(E))
    return IL->getValue().isZero();
  return false;
}

static StringRef getMemberName(const Expr *E) {
  E = E ? E->IgnoreParenCasts() : nullptr;
  if (const auto *ME = dyn_cast_or_null<MemberExpr>(E)) {
    if (const ValueDecl *VD = ME->getMemberDecl())
      return VD->getName();
  }
  return StringRef();
}

static bool hasStringSemanticName(StringRef Name) {
  if (Name.empty())
    return false;
  return Name.contains_insensitive("string") ||
         Name.contains_insensitive("name") ||
         Name.contains_insensitive("text") ||
         Name.contains_insensitive("label") ||
         Name.contains_insensitive("title") ||
         Name.contains_insensitive("comment") ||
         Name.contains_insensitive("password") ||
         Name.contains_insensitive("url") ||
         Name.contains_insensitive("path") ||
         Name.contains_insensitive("message") ||
         Name.contains_insensitive("desc");
}

static bool isStringLikeType(QualType QT) {
  if (QT.isNull())
    return false;
  QT = QT.getCanonicalType();
  if (QT->isPointerType()) {
    QualType Pointee = QT->getPointeeType();
    return !Pointee.isNull() && Pointee->isAnyCharacterType();
  }
  if (const auto *CAT = dyn_cast<ConstantArrayType>(QT.getTypePtr()))
    return CAT->getElementType()->isAnyCharacterType();
  return false;
}

static bool exprHasStringSemantic(const Expr *E) {
  E = E ? E->IgnoreParenCasts() : nullptr;
  if (!E)
    return false;

  if (isStringLikeType(E->getType()))
    return true;

  StringRef MemberName = getMemberName(E);
  if (hasStringSemanticName(MemberName))
    return true;

  if (const auto *DRE = dyn_cast<DeclRefExpr>(E)) {
    if (const ValueDecl *VD = DRE->getDecl()) {
      if (hasStringSemanticName(VD->getName()))
        return true;
      if (isStringLikeType(VD->getType()))
        return true;
    }
  }

  if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(E))
    return exprHasStringSemantic(ASE->getBase());

  if (const auto *UO = dyn_cast<UnaryOperator>(E))
    return exprHasStringSemantic(UO->getSubExpr());

  return false;
}

static const MemRegion *getBaseRegion(const MemRegion *MR) {
  if (!MR)
    return nullptr;
  const MemRegion *Base = MR->getBaseRegion();
  return Base ? Base : MR;
}

static const MemRegion *getExprRegion(CheckerContext &C, const Expr *E) {
  if (!E)
    return nullptr;
  SVal V = C.getSVal(E);
  const MemRegion *MR = V.getAsRegion();
  return getBaseRegion(MR);
}

static bool isRawReadProducer(StringRef Name) {
  return Name == "readBytes" ||
         Name == "readbytes" ||
         Name == "fread" ||
         Name == "read" ||
         Name == "recv";
}

static bool isStringReadProducer(StringRef Name) {
  return Name == "readString" ||
         Name == "readstring" ||
         Name == "strdup" ||
         Name == "getline" ||
         Name == "fgets";
}

static bool isStringConsumer(StringRef Name) {
  return Name == "strlen" ||
         Name == "strcmp" ||
         Name == "strncmp" ||
         Name == "strcpy" ||
         Name == "strncpy" ||
         Name == "strcat" ||
         Name == "strncat" ||
         Name == "puts" ||
         Name == "printf" ||
         Name == "fprintf" ||
         Name == "snprintf" ||
         Name == "vprintf" ||
         Name == "vfprintf" ||
         Name == "atoi" ||
         Name == "atol";
}

class StringFieldRawReadMismatchChecker
    : public Checker<check::Bind, check::PreCall> {
  mutable std::unique_ptr<BugType> BT;

public:
  StringFieldRawReadMismatchChecker()
      : BT(std::make_unique<BugType>(
            this,
            "String field populated by raw byte reader",
            "Custom")) {}

  void checkBind(SVal Loc, SVal Val, const Stmt *S, CheckerContext &C) const {
    const auto *AssignedE = dyn_cast_or_null<Expr>(S);
    if (!AssignedE)
      return;

    const MemRegion *TargetMR = Loc.getAsRegion();
    TargetMR = getBaseRegion(TargetMR);
    if (!TargetMR)
      return;

    ProgramStateRef State = C.getState();

    const auto *BO = dyn_cast<BinaryOperator>(AssignedE->IgnoreParenCasts());
    if (!BO || !BO->isAssignmentOp())
      return;

    const Expr *LHS = BO->getLHS();
    const Expr *RHS = BO->getRHS();
    if (!LHS || !RHS)
      return;

    if (!exprHasStringSemantic(LHS))
      return;

    const auto *CallRHS = dyn_cast<CallExpr>(RHS->IgnoreParenCasts());
    if (!CallRHS)
      return;

    const FunctionDecl *FD = CallRHS->getDirectCallee();
    if (!FD)
      return;

    StringRef Callee = FD->getName();

    if (isStringReadProducer(Callee)) {
      State = State->remove<TaintedStringFieldRegions>(TargetMR);
      C.addTransition(State);
      return;
    }

    if (!isRawReadProducer(Callee))
      return;

    if (Callee == "readBytes" && CallRHS->getNumArgs() >= 2) {
      if (isZeroIntegerLiteral(CallRHS->getArg(1)))
        return;
    }

    State = State->add<TaintedStringFieldRegions>(TargetMR);
    C.addTransition(State);
  }

  void checkPreCall(const CallEvent &Call, CheckerContext &C) const {
    const IdentifierInfo *II = Call.getCalleeIdentifier();
    if (!II)
      return;

    StringRef Callee = II->getName();
    if (!isStringConsumer(Callee))
      return;

    ProgramStateRef State = C.getState();

    for (unsigned I = 0; I < Call.getNumArgs(); ++I) {
      const Expr *ArgE = Call.getArgExpr(I);
      if (!ArgE)
        continue;

      if (!exprHasStringSemantic(ArgE) && !ArgE->getType()->isPointerType())
        continue;

      const MemRegion *ArgMR = getExprRegion(C, ArgE);
      if (!ArgMR)
        continue;

      if (!State->contains<TaintedStringFieldRegions>(ArgMR))
        continue;

      ExplodedNode *N = C.generateNonFatalErrorNode();
      if (!N)
        return;

      auto Report = std::make_unique<PathSensitiveBugReport>(
          *BT,
          "A field or variable with string semantics is populated by a raw byte "
          "reader and later used as a C string without a string-aware read "
          "barrier, which may leave it unterminated or semantically invalid.",
          N);
      Report->addRange(ArgE->getSourceRange());
      C.emitReport(std::move(Report));
      return;
    }
  }
};

} // namespace

extern "C" void clang_registerCheckers(CheckerRegistry &Registry) {
  Registry.addChecker<StringFieldRawReadMismatchChecker>(
      "custom.StringFieldRawReadMismatchChecker",
      "Detects string-semantic fields populated by raw byte readers and later consumed as C strings.",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] = CLANG_VERSION_STRING;
