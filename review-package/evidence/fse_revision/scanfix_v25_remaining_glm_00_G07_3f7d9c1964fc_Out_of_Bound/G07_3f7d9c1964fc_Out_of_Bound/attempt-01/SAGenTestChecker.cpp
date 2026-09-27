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
#include "clang/Lex/Lexer.h"
#include "llvm/ADT/DenseMap.h"

using namespace clang;
using namespace ento;
using namespace taint;

// Track variables/fields that hold a device-provided size (rss_max_key_size).
REGISTER_SET_WITH_PROGRAMSTATE(DeviceSizeSet, const MemRegion*)

namespace {

// True when the expression's source text mentions the device config member
// named by `Name`; used to recognize the offsetof(...) operand that selects
// the rss_max_key_size config field in a virtio config read.
static bool sourceMentionsDeviceMember(const Expr *E, StringRef Name,
                                       const ASTContext &Ctx) {
  if (!E)
    return false;
  const SourceManager &SM = Ctx.getSourceManager();
  SourceRange R = E->getSourceRange();
  if (!R.isValid())
    return false;
  CharSourceRange CharRange =
      CharSourceRange::getTokenRange(SM.getExpansionRange(R).getAsRange());
  std::string Text = Lexer::getSourceText(CharRange, SM, Ctx.getLangOpts()).str();
  return StringRef(Text).find(Name) != StringRef::npos;
}

// Detects whether a statement diverges from normal control flow (goto or
// return), so a guard's rejecting branch really stops the bad value.
class DivergingBlockFinder : public RecursiveASTVisitor<DivergingBlockFinder> {
public:
  bool Exits = false;
  bool VisitGotoStmt(GotoStmt *) { Exits = true; return true; }
  bool VisitReturnStmt(ReturnStmt *) { Exits = true; return true; }
};

// Collects translation-unit facts about device-provided lengths:
//  - struct fields assigned from virtio_cread{8,16,32}(dev,
//    offsetof(..., rss_max_key_size)); the stored value cannot exceed the
//    maximum value of the read width;
//  - guards that reject such fields above a constant bound and then leave
//    the function, so every continuing path keeps field <= bound.
class DeviceLenFactsCollector
    : public RecursiveASTVisitor<DeviceLenFactsCollector> {
public:
  llvm::DenseMap<const FieldDecl *, uint64_t> FieldMax;
  llvm::DenseMap<const FieldDecl *, uint64_t> GuardBound;
  llvm::DenseMap<const FieldDecl *, SourceLocation> ReadLoc;
  ASTContext &Ctx;

  explicit DeviceLenFactsCollector(ASTContext &CtxIn) : Ctx(CtxIn) {}

  bool VisitBinaryOperator(BinaryOperator *BO) {
    if (BO->getOpcode() != BO_Assign)
      return true;

    const auto *LME = dyn_cast<MemberExpr>(BO->getLHS()->IgnoreParenImpCasts());
    if (!LME)
      return true;
    const auto *FD = dyn_cast<FieldDecl>(LME->getMemberDecl());
    if (!FD)
      return true;

    const auto *CE = dyn_cast<CallExpr>(BO->getRHS()->IgnoreParenImpCasts());
    if (!CE)
      return true;
    const FunctionDecl *Callee = CE->getDirectCallee();
    if (!Callee)
      return true;
    StringRef FName = Callee->getName();
    if (!(FName == "virtio_cread8" || FName == "virtio_cread16" ||
          FName == "virtio_cread32"))
      return true;
    if (CE->getNumArgs() < 2)
      return true;
    if (!sourceMentionsDeviceMember(CE->getArg(1), "rss_max_key_size", Ctx))
      return true;

    // The stored value is an unsigned integer of the read width.
    uint64_t Bits = Ctx.getTypeSize(Callee->getReturnType());
    if (Bits == 0 || Bits > 64)
      return true;
    uint64_t Max = (Bits >= 64) ? ~0ULL : ((1ULL << Bits) - 1);
    auto It = FieldMax.find(FD);
    if (It == FieldMax.end() || Max > It->second)
      FieldMax[FD] = Max;
    ReadLoc[FD] = BO->getBeginLoc();
    return true;
  }

  bool VisitIfStmt(IfStmt *IS) {
    const auto *BO =
        dyn_cast<BinaryOperator>(IS->getCond()->IgnoreParenImpCasts());
    if (!BO)
      return true;

    const auto Op = BO->getOpcode();
    if (Op != BO_GT && Op != BO_GE && Op != BO_LT && Op != BO_LE)
      return true;

    const auto *LME = dyn_cast<MemberExpr>(BO->getLHS()->IgnoreParenImpCasts());
    const auto *RME = dyn_cast<MemberExpr>(BO->getRHS()->IgnoreParenImpCasts());
    const IntegerLiteral *BoundLit = nullptr;
    const FieldDecl *FD = nullptr;
    bool FieldOnLeft = false;

    if (LME) {
      FD = dyn_cast<FieldDecl>(LME->getMemberDecl());
      if (FD) {
        BoundLit =
            dyn_cast<IntegerLiteral>(BO->getRHS()->IgnoreParenImpCasts());
        FieldOnLeft = true;
      }
    }
    if (!FD && RME) {
      FD = dyn_cast<FieldDecl>(RME->getMemberDecl());
      if (FD)
        BoundLit =
            dyn_cast<IntegerLiteral>(BO->getLHS()->IgnoreParenImpCasts());
    }
    if (!FD || !BoundLit)
      return true;

    // Upper-bound rejection: `field > K` (or `K < field`) keeps field <= K;
    // `field >= K` (or `K <= field`) keeps field <= K - 1.  Other operand
    // orders only establish lower bounds and are ignored.
    uint64_t K = BoundLit->getValue().getZExtValue();
    uint64_t Bound = 0;
    if (FieldOnLeft) {
      if (Op == BO_GT)
        Bound = K;
      else if (Op == BO_GE)
        Bound = K > 0 ? K - 1 : 0;
      else
        return true;
    } else {
      if (Op == BO_LT)
        Bound = K;
      else if (Op == BO_LE)
        Bound = K > 0 ? K - 1 : 0;
      else
        return true;
    }

    // Only a guard over a collected device-read field counts, and it must
    // follow the read in source order.
    auto RL = ReadLoc.find(FD);
    if (RL == ReadLoc.end())
      return true;
    if (!Ctx.getSourceManager().isBeforeInTranslationUnit(RL->second,
                                                          IS->getBeginLoc()))
      return true;

    // The rejecting branch must leave the function; otherwise large values
    // continue past the guard.
    DivergingBlockFinder Finder;
    Finder.TraverseStmt(IS->getThen());
    if (!Finder.Exits)
      return true;

    auto It = GuardBound.find(FD);
    if (It == GuardBound.end() || Bound < It->second)
      GuardBound[FD] = Bound;
    return true;
  }
};

/* The checker callbacks are to be decided. */
class SAGenTestChecker : public Checker< check::Bind, check::PreCall > {
   mutable std::unique_ptr<BugType> BT;

   public:
      SAGenTestChecker() : BT(new BugType(this, "Unbounded device-provided RSS key length used", "Memory Safety")) {}

      void checkBind(SVal Loc, SVal Val, const Stmt *S, CheckerContext &C) const;
      void checkPreCall(const CallEvent &Call, CheckerContext &C) const;

   private:

      // Helpers
      bool isVirtioCreadOfRssKeySize(const CallEvent &Call, CheckerContext &C) const;
      bool isVirtioCreadOfRssKeySize(const CallExpr *CE, CheckerContext &C) const;

      bool isKnownCopyLenSink(const CallEvent &Call, unsigned &DestIdx, unsigned &LenIdx, CheckerContext &C) const;

      const MemRegion* resolveExprRegion(const Expr *E, CheckerContext &C) const;

      const FieldDecl *deviceLenFieldOf(const Expr *LenE) const;
      void ensureDeviceFactsBuilt(CheckerContext &C) const;
      bool lenComesFromDeviceSize(const Expr *LenE, CheckerContext &C) const;

      bool getConstArraySizeOfExpr(llvm::APInt &ArraySize, const Expr *DestE, CheckerContext &C) const;

      bool lengthIsProvablyBounded(CheckerContext &C, const Expr *LenE, uint64_t Limit) const;

      void report(const CallEvent &Call, const Expr *LenE, CheckerContext &C) const;

      // Cross-function facts (see DeviceLenFactsCollector), built once per
      // translation unit: device-read fields, their maximum value, and the
      // tightest guard rejecting larger values.
      mutable bool DeviceFactsBuilt = false;
      mutable llvm::DenseMap<const FieldDecl *, uint64_t> DeviceFieldMax;
      mutable llvm::DenseMap<const FieldDecl *, uint64_t> FieldGuardBound;
};

bool SAGenTestChecker::isVirtioCreadOfRssKeySize(const CallEvent &Call, CheckerContext &C) const {
  const Expr *Origin = Call.getOriginExpr();
  if (!Origin)
    return false;

  // Match function name virtio_cread8/16/32
  bool IsCread =
      ExprHasName(Origin, "virtio_cread8", C) ||
      ExprHasName(Origin, "virtio_cread16", C) ||
      ExprHasName(Origin, "virtio_cread32", C);
  if (!IsCread)
    return false;

  if (Call.getNumArgs() < 2)
    return false;

  const Expr *OffsetExpr = Call.getArgExpr(1);
  if (!OffsetExpr)
    return false;

  // The offset expression should contain "rss_max_key_size"
  return ExprHasName(OffsetExpr, "rss_max_key_size", C);
}

bool SAGenTestChecker::isVirtioCreadOfRssKeySize(const CallExpr *CE, CheckerContext &C) const {
  if (!CE)
    return false;

  // Check callee name using source text
  if (!(ExprHasName(CE, "virtio_cread8", C) ||
        ExprHasName(CE, "virtio_cread16", C) ||
        ExprHasName(CE, "virtio_cread32", C)))
    return false;

  if (CE->getNumArgs() < 2)
    return false;

  const Expr *OffsetExpr = CE->getArg(1);
  if (!OffsetExpr)
    return false;

  return ExprHasName(OffsetExpr, "rss_max_key_size", C);
}

bool SAGenTestChecker::isKnownCopyLenSink(const CallEvent &Call, unsigned &DestIdx, unsigned &LenIdx, CheckerContext &C) const {
  const Expr *Origin = Call.getOriginExpr();
  if (!Origin)
    return false;

  // memcpy(dest, src, len)
  if (ExprHasName(Origin, "memcpy", C)) {
    if (Call.getNumArgs() >= 3) {
      DestIdx = 0;
      LenIdx = 2;
      return true;
    }
    return false;
  }

  // memmove(dest, src, len)
  if (ExprHasName(Origin, "memmove", C)) {
    if (Call.getNumArgs() >= 3) {
      DestIdx = 0;
      LenIdx = 2;
      return true;
    }
    return false;
  }

  // virtio_cread_bytes(dev, off, buf, len)
  if (ExprHasName(Origin, "virtio_cread_bytes", C)) {
    if (Call.getNumArgs() >= 4) {
      DestIdx = 2;
      LenIdx = 3;
      return true;
    }
    return false;
  }

  return false;
}

const MemRegion* SAGenTestChecker::resolveExprRegion(const Expr *E, CheckerContext &C) const {
  if (!E)
    return nullptr;

  // Keep the exact sub-region so a tagged field is not confused with other
  // fields of the same base object.
  const MemRegion *MR = getMemRegionFromExpr(E, C);
  if (MR)
    return MR;

  // Try find a DeclRefExpr child
  if (const auto *DRE = findSpecificTypeInChildren<DeclRefExpr>(E)) {
    MR = getMemRegionFromExpr(DRE, C);
    if (MR)
      return MR;
  }

  // Try find a MemberExpr child
  if (const auto *ME = findSpecificTypeInChildren<MemberExpr>(E)) {
    MR = getMemRegionFromExpr(ME, C);
    if (MR)
      return MR;
  }

  return nullptr;
}

const FieldDecl *SAGenTestChecker::deviceLenFieldOf(const Expr *LenE) const {
  // A struct field whose value is known to be read from virtio config
  // space at the rss_max_key_size offset (translation-unit fact).
  if (!LenE)
    return nullptr;
  const Expr *Core = LenE->IgnoreParenImpCasts();
  if (const auto *ME = dyn_cast<MemberExpr>(Core))
    if (const auto *FD = dyn_cast<FieldDecl>(ME->getMemberDecl()))
      if (DeviceFieldMax.count(FD))
        return FD;
  return nullptr;
}

void SAGenTestChecker::ensureDeviceFactsBuilt(CheckerContext &C) const {
  if (DeviceFactsBuilt)
    return;
  DeviceFactsBuilt = true;
  DeviceLenFactsCollector Collector(C.getASTContext());
  Collector.TraverseDecl(C.getASTContext().getTranslationUnitDecl());
  DeviceFieldMax = std::move(Collector.FieldMax);
  FieldGuardBound = std::move(Collector.GuardBound);
}

bool SAGenTestChecker::lenComesFromDeviceSize(const Expr *LenE, CheckerContext &C) const {
  if (!LenE)
    return false;

  // Path-sensitive fact: the length's region holds a value assigned from a
  // virtio_cread{8,16,32}(..., rss_max_key_size) call (DeviceSizeSet tag).
  if (const MemRegion *LenReg = resolveExprRegion(LenE, C)) {
    ProgramStateRef State = C.getState();
    if (State->contains<DeviceSizeSet>(LenReg->getBaseRegion()))
      return true;
  }

  // Translation-unit fact: the length names a struct field whose value is
  // assigned from virtio_cread{8,16,32}(..., rss_max_key_size).
  ensureDeviceFactsBuilt(C);
  return deviceLenFieldOf(LenE) != nullptr;
}

bool SAGenTestChecker::getConstArraySizeOfExpr(llvm::APInt &ArraySize, const Expr *DestE, CheckerContext &C) const {
  if (!DestE)
    return false;

  // First try the provided helper on the expression directly.
  if (getArraySizeFromExpr(ArraySize, DestE))
    return true;

  // Check if the expression type itself is a constant array.
  QualType QT = DestE->getType();
  if (!QT.isNull()) {
    if (const auto *CAT = dyn_cast<ConstantArrayType>(QT.getTypePtr())) {
      ArraySize = CAT->getSize();
      return true;
    }
  }

  // Walk the destination member chain from the outermost field inward: the
  // first field with a constant array type gives the destination capacity
  // (e.g. vi->ctrl->ctrl.rss.key names the u8[VIRTIO_NET_RSS_MAX_KEY_SIZE]
  // key field).
  for (const Expr *E = DestE->IgnoreParenImpCasts(); E;) {
    if (const auto *ME = dyn_cast<MemberExpr>(E)) {
      if (const auto *FD = dyn_cast<FieldDecl>(ME->getMemberDecl())) {
        QualType FTy = FD->getType();
        if (const auto *CAT = dyn_cast<ConstantArrayType>(FTy.getTypePtr())) {
          ArraySize = CAT->getSize();
          return true;
        }
      }
      E = ME->getBase()->IgnoreParenImpCasts();
      continue;
    }
    if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(E)) {
      E = ASE->getBase()->IgnoreParenImpCasts();
      continue;
    }
    if (const auto *UO = dyn_cast<UnaryOperator>(E)) {
      if (UO->getOpcode() == UO_AddrOf || UO->getOpcode() == UO_Deref) {
        E = UO->getSubExpr()->IgnoreParenImpCasts();
        continue;
      }
      break;
    }
    if (const auto *DRE = dyn_cast<DeclRefExpr>(E)) {
      if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
        QualType VDTy = VD->getType();
        if (const auto *CAT = dyn_cast<ConstantArrayType>(VDTy.getTypePtr())) {
          ArraySize = CAT->getSize();
          return true;
        }
      }
    }
    break;
  }

  // Try DeclRefExpr child
  if (const auto *DRE = findSpecificTypeInChildren<DeclRefExpr>(DestE)) {
    if (getArraySizeFromExpr(ArraySize, DRE))
      return true;

    // Also try type of the referenced declaration
    if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
      QualType VDTy = VD->getType();
      if (const auto *CAT = dyn_cast<ConstantArrayType>(VDTy.getTypePtr())) {
        ArraySize = CAT->getSize();
        return true;
      }
    }
  }

  return false;
}

bool SAGenTestChecker::lengthIsProvablyBounded(CheckerContext &C, const Expr *LenE, uint64_t Limit) const {
  if (!LenE)
    return false;

  ProgramStateRef State = C.getState();
  SVal LenSVal = State->getSVal(LenE, C.getLocationContext());
  if (SymbolRef Sym = LenSVal.getAsSymbol()) {
    if (const llvm::APSInt *Max = inferSymbolMaxVal(Sym, C)) {
      // If we can infer a maximum and it's <= Limit, it's safe
      uint64_t MaxZ = Max->getZExtValue();
      return MaxZ <= Limit;
    }
  }
  return false;
}

void SAGenTestChecker::report(const CallEvent &Call, const Expr *LenE, CheckerContext &C) const {
  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto R = std::make_unique<PathSensitiveBugReport>(
      *BT, "Unbounded device-provided RSS key length used", N);

  if (LenE)
    R->addRange(LenE->getSourceRange());
  if (const Expr *OE = Call.getOriginExpr())
    R->addRange(OE->getSourceRange());

  C.emitReport(std::move(R));
}

void SAGenTestChecker::checkBind(SVal Loc, SVal Val, const Stmt *S, CheckerContext &C) const {
  ProgramStateRef State = C.getState();

  const MemRegion *LHSReg = Loc.getAsRegion();
  if (LHSReg)
    LHSReg = LHSReg->getBaseRegion();

  // Propagate tag on simple assignments: if RHS region is in set, add LHS
  if (LHSReg) {
    if (const MemRegion *RHSReg = Val.getAsRegion()) {
      RHSReg = RHSReg->getBaseRegion();
      if (RHSReg && State->contains<DeviceSizeSet>(RHSReg)) {
        State = State->add<DeviceSizeSet>(LHSReg);
        C.addTransition(State);
        return;
      }
    }
  }

  // If RHS is a call to virtio_cread{8,16,32}(..., rss_max_key_size), mark LHS
  if (LHSReg && S) {
    // Find a CallExpr under this statement (assignment RHS)
    if (const auto *CE = findSpecificTypeInChildren<CallExpr>(S)) {
      if (isVirtioCreadOfRssKeySize(CE, C)) {
        State = State->add<DeviceSizeSet>(LHSReg);
        C.addTransition(State);
        return;
      }
    }
  }
}

void SAGenTestChecker::checkPreCall(const CallEvent &Call, CheckerContext &C) const {
  unsigned DestIdx = 0, LenIdx = 0;
  if (!isKnownCopyLenSink(Call, DestIdx, LenIdx, C))
    return;

  const Expr *LenE = Call.getArgExpr(LenIdx);
  if (!lenComesFromDeviceSize(LenE, C))
    return;

  // Determine destination capacity
  const Expr *DestE = Call.getArgExpr(DestIdx);
  llvm::APInt ArrSize;
  if (!getConstArraySizeOfExpr(ArrSize, DestE, C))
    return; // Can't prove destination capacity; avoid false positives

  uint64_t Capacity = ArrSize.getZExtValue();
  if (lengthIsProvablyBounded(C, LenE, Capacity))
    return; // Proved safe on this path

  // Report: device-provided rss key length used without bound check
  report(Call, LenE, C);
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects unbounded device-provided RSS key length usage in copy-like APIs",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
