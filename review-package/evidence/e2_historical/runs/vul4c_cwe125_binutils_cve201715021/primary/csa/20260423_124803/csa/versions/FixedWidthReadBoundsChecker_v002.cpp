#include "clang/AST/Expr.h"
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

class FixedWidthReadBoundsChecker : public Checker<check::PreCall> {
  mutable std::unique_ptr<BugType> BT;

  static const Expr *ignoreCasts(const Expr *E) {
    return E ? E->IgnoreParenCasts() : nullptr;
  }

  static const Expr *stripAdditiveOffsetExpr(const Expr *E) {
    E = ignoreCasts(E);
    const auto *BO = dyn_cast_or_null<BinaryOperator>(E);
    if (!BO)
      return nullptr;
    if (BO->getOpcode() != BO_Add)
      return nullptr;

    const Expr *LHS = ignoreCasts(BO->getLHS());
    const Expr *RHS = ignoreCasts(BO->getRHS());

    if (LHS && LHS->getType()->isPointerType() && RHS && RHS->getType()->isIntegerType())
      return RHS;
    if (RHS && RHS->getType()->isPointerType() && LHS && LHS->getType()->isIntegerType())
      return LHS;
    return nullptr;
  }

  static bool isFixedWidthReadFunction(StringRef Name, uint64_t &Width) {
    if (Name == "bfd_get_16") {
      Width = 2;
      return true;
    }
    if (Name == "bfd_get_32") {
      Width = 4;
      return true;
    }
    if (Name == "bfd_get_64") {
      Width = 8;
      return true;
    }
    if (Name == "bfd_getb16" || Name == "bfd_getl16") {
      Width = 2;
      return true;
    }
    if (Name == "bfd_getb32" || Name == "bfd_getl32") {
      Width = 4;
      return true;
    }
    if (Name == "bfd_getb64" || Name == "bfd_getl64") {
      Width = 8;
      return true;
    }
    return false;
  }

  static bool getConcreteUnsigned(ProgramStateRef State, SVal V, uint64_t &Out) {
    std::optional<nonloc::ConcreteInt> CI = V.getAs<nonloc::ConcreteInt>();
    if (!CI)
      return false;
    const llvm::APSInt &Value = CI->getValue();
    if (Value.isSigned() && Value.isNegative())
      return false;
    Out = Value.getZExtValue();
    return true;
  }

public:
  FixedWidthReadBoundsChecker()
      : BT(std::make_unique<BugType>(this,
                                     "Fixed-width read without width-aware bounds guard",
                                     "Custom")) {}

  void checkPreCall(const CallEvent &Call, CheckerContext &C) const {
    const IdentifierInfo *II = Call.getCalleeIdentifier();
    if (!II)
      return;

    uint64_t ReadWidth = 0;
    StringRef FuncName = II->getName();
    if (!isFixedWidthReadFunction(FuncName, ReadWidth))
      return;

    if (Call.getNumArgs() < 2)
      return;

    const Expr *PtrArgExpr = Call.getArgExpr(1);
    PtrArgExpr = ignoreCasts(PtrArgExpr);
    if (!PtrArgExpr)
      return;

    const Expr *OffsetExpr = stripAdditiveOffsetExpr(PtrArgExpr);
    if (!OffsetExpr)
      return;

    SVal OffsetVal = C.getSVal(OffsetExpr);
    uint64_t Offset = 0;
    if (!getConcreteUnsigned(C.getState(), OffsetVal, Offset))
      return;

    SVal PtrArgVal = Call.getArgSVal(1);
    const MemRegion *MR = PtrArgVal.getAsRegion();
    if (!MR)
      return;

    const auto *TVR = dyn_cast<TypedValueRegion>(MR);
    if (!TVR)
      return;

    QualType RegionTy = TVR->getValueType();
    const Type *Ty = RegionTy.getTypePtrOrNull();
    if (!Ty)
      return;

    const ConstantArrayType *CAT = nullptr;
    if (const auto *PT = Ty->getAs<PointerType>())
      CAT = dyn_cast_or_null<ConstantArrayType>(PT->getPointeeType().getTypePtrOrNull());
    else
      CAT = dyn_cast<ConstantArrayType>(Ty);
    if (!CAT)
      return;

    QualType ElemTy = CAT->getElementType();
    if (ElemTy.isNull())
      return;

    uint64_t ElemSize = C.getASTContext().getTypeSizeInChars(ElemTy).getQuantity();
    uint64_t ElemCount = CAT->getSize().getZExtValue();
    uint64_t BufferSize = ElemSize * ElemCount;

    if (Offset > BufferSize)
      return;

    if (Offset + ReadWidth <= BufferSize)
      return;

    ExplodedNode *N = C.generateNonFatalErrorNode();
    if (!N)
      return;

    auto Report = std::make_unique<PathSensitiveBugReport>(
        *BT,
        "Fixed-width read may cross the end of the buffer: the starting offset is inside the buffer but the full read width is not proven to fit.",
        N);
    Report->addRange(PtrArgExpr->getSourceRange());
    if (OffsetExpr)
      Report->addRange(OffsetExpr->getSourceRange());
    C.emitReport(std::move(Report));
  }
};

} // namespace

extern "C" void clang_registerCheckers(CheckerRegistry &Registry) {
  Registry.addChecker<FixedWidthReadBoundsChecker>(
      "custom.FixedWidthReadBoundsChecker",
      "Detects fixed-width reads whose full width can exceed the remaining buffer extent.",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] = CLANG_VERSION_STRING;
