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
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/APInt.h"
#include <limits>
#include <cstdint>

using namespace clang;
using namespace ento;
using namespace taint;

// Program state: possibly-uninitialized heap allocations, keyed by the
// pointer symbol returned by the allocating call so the identity survives
// copies and stores.
REGISTER_MAP_WITH_PROGRAMSTATE(UninitAllocMap, SymbolRef, bool)
// Program state: allocation byte size when the size argument is constant.
REGISTER_MAP_WITH_PROGRAMSTATE(AllocByteSizeMap, SymbolRef, uint64_t)

namespace {

class SAGenTestChecker : public Checker<
    check::PostCall,
    check::PreCall,
    check::Bind,
    check::RegionChanges
> {
   mutable std::unique_ptr<BugType> BT;

   public:
      SAGenTestChecker() : BT(new BugType(this, "Kernel info leak (copy_to_user)", "Security")) {}

      void checkPostCall(const CallEvent &Call, CheckerContext &C) const;
      void checkPreCall(const CallEvent &Call, CheckerContext &C) const;
      void checkBind(SVal Loc, SVal Val, const Stmt *StoreE, CheckerContext &C) const;
      ProgramStateRef checkRegionChanges(ProgramStateRef State,
                                         const InvalidatedSymbols *Invalidated,
                                         ArrayRef<const MemRegion *> ExplicitRegions,
                                         ArrayRef<const MemRegion *> Regions,
                                         const LocationContext *LCtx,
                                         const CallEvent *Call) const;

   private:
      // Helpers
      // Identity of a heap allocation: the pointer symbol returned by the
      // allocating call. Assignments preserve this symbol, so allocations are
      // tracked without an explicit alias map.
      static SymbolRef getPointerSymbol(SVal V) {
        // Pointer values are loc::MemRegionVal over a SymbolicRegion; Clang 18
        // has no loc::SymbolVal, so derive the symbol from the region.
        if (const MemRegion *R = V.getAsRegion())
          if (const auto *SR = dyn_cast<SymbolicRegion>(R))
            return SR->getSymbol();
        return nullptr;
      }

      static bool getArgConstUInt(const CallEvent &Call, unsigned Idx,
                                  CheckerContext &C, uint64_t &Out) {
        if (Idx >= Call.getNumArgs())
          return false;
        const Expr *E = Call.getArgExpr(Idx);
        if (!E)
          return false;
        llvm::APSInt V;
        if (!EvaluateExprToInt(V, E, C))
          return false;
        if (V.isSigned() && V.isNegative())
          return false;
        Out = V.getZExtValue();
        return true;
      }

      // Evaluate an expression to a concrete integer: use the
      // path-sensitive value first, then AST constant folding.
      static bool EvaluateExprToInt(llvm::APSInt &Out, const Expr *E,
                                    CheckerContext &C) {
        if (!E)
          return false;
        if (auto CI = C.getSVal(E).getAs<nonloc::ConcreteInt>()) {
          Out = CI->getValue();
          return true;
        }
        Expr::EvalResult ER;
        if (E->EvaluateAsInt(ER, C.getASTContext())) {
          Out = ER.Val.getInt();
          return true;
        }
        return false;
      }

      // True when the callee carries the given identifier name; these kernel
      // APIs name the allocation-initialization semantics being modeled.
      static bool callHasName(const CallEvent &Call, CheckerContext &C, StringRef Name) {
        if (const auto *FD = dyn_cast_or_null<FunctionDecl>(Call.getDecl())) {
          const IdentifierInfo *II = FD->getIdentifier();
          if (II && II->isStr(Name))
            return true;
        }
        const Expr *Origin = Call.getOriginExpr();
        if (!Origin)
          return false;
        Origin = Origin->IgnoreParenImpCasts();
        if (const auto *DRE = dyn_cast<DeclRefExpr>(Origin)) {
          const IdentifierInfo *II = DRE->getDecl()->getIdentifier();
          return II && II->isStr(Name);
        }
        return false;
      }

      void markAllocUninitAndSize(const CallEvent &Call, CheckerContext &C,
                                  unsigned SizeArgIndex) const {
        ProgramStateRef State = C.getState();
        SymbolRef Sym = getPointerSymbol(Call.getReturnValue());
        if (!Sym)
          return;

        // Mark as possibly-uninitialized.
        State = State->set<UninitAllocMap>(Sym, true);

        // Try to record the allocation size if it's a constant.
        uint64_t SizeBytes = 0;
        if (getArgConstUInt(Call, SizeArgIndex, C, SizeBytes)) {
          State = State->set<AllocByteSizeMap>(Sym, SizeBytes);
        } else {
          State = State->remove<AllocByteSizeMap>(Sym);
        }

        C.addTransition(State);
      }

      void markAllocZeroedAndSize(const CallEvent &Call, CheckerContext &C,
                                  unsigned SizeArgIndex) const {
        ProgramStateRef State = C.getState();
        SymbolRef Sym = getPointerSymbol(Call.getReturnValue());
        if (!Sym)
          return;

        // Fully zero-initialized.
        State = State->remove<UninitAllocMap>(Sym);

        // Try to record size.
        uint64_t SizeBytes = 0;
        if (getArgConstUInt(Call, SizeArgIndex, C, SizeBytes)) {
          State = State->set<AllocByteSizeMap>(Sym, SizeBytes);
        } else {
          State = State->remove<AllocByteSizeMap>(Sym);
        }

        C.addTransition(State);
      }

      void markKcallocZeroedAndSize(const CallEvent &Call, CheckerContext &C,
                                    unsigned NmembIdx, unsigned SizeIdx) const {
        ProgramStateRef State = C.getState();
        SymbolRef Sym = getPointerSymbol(Call.getReturnValue());
        if (!Sym)
          return;

        State = State->remove<UninitAllocMap>(Sym);

        uint64_t Nmemb = 0, Sz = 0;
        if (getArgConstUInt(Call, NmembIdx, C, Nmemb) &&
            getArgConstUInt(Call, SizeIdx, C, Sz)) {
          __uint128_t Prod = static_cast<__uint128_t>(Nmemb) * static_cast<__uint128_t>(Sz);
          if (Prod <= std::numeric_limits<uint64_t>::max()) {
            State = State->set<AllocByteSizeMap>(Sym, static_cast<uint64_t>(Prod));
          } else {
            State = State->remove<AllocByteSizeMap>(Sym);
          }
        } else {
          State = State->remove<AllocByteSizeMap>(Sym);
        }

        C.addTransition(State);
      }

      void maybeHandleMemset(const CallEvent &Call, CheckerContext &C) const {
        // We only care about memset(ptr, 0, len) that fully covers the allocation.
        if (!callHasName(Call, C, "memset"))
          return;
        if (Call.getNumArgs() < 3)
          return;

        ProgramStateRef State = C.getState();

        // Check value == 0
        uint64_t Val = 0;
        if (!getArgConstUInt(Call, 1, C, Val) || Val != 0)
          return;

        // Length
        uint64_t Len = 0;
        if (!getArgConstUInt(Call, 2, C, Len))
          return;

        SymbolRef DstSym = getPointerSymbol(Call.getArgSVal(0));
        if (!DstSym)
          return;

        const bool *Uninit = State->get<UninitAllocMap>(DstSym);
        if (!Uninit || !*Uninit)
          return;

        const uint64_t *AllocSize = State->get<AllocByteSizeMap>(DstSym);
        if (!AllocSize)
          return;

        if (Len >= *AllocSize) {
          State = State->remove<UninitAllocMap>(DstSym);
          C.addTransition(State);
        }
      }

      void reportLeak(const CallEvent &Call, CheckerContext &C, const Expr *SrcArg) const {
        ExplodedNode *N = C.generateNonFatalErrorNode();
        if (!N)
          return;
        auto R = std::make_unique<PathSensitiveBugReport>(
            *BT,
            "copy_to_user from kmalloc() buffer may leak uninitialized bytes; use kzalloc() or clear the buffer",
            N);
        if (SrcArg)
          R->addRange(SrcArg->getSourceRange());
        else
          R->addRange(Call.getSourceRange());
        C.emitReport(std::move(R));
      }
};

// Track allocations and zeroing calls.
void SAGenTestChecker::checkPostCall(const CallEvent &Call, CheckerContext &C) const {
  // kmalloc(size, gfp)
  if (callHasName(Call, C, "kmalloc")) {
    markAllocUninitAndSize(Call, C, 0);
    return;
  }

  // kzalloc(size, gfp), kvzalloc(size, gfp)
  if (callHasName(Call, C, "kzalloc") || callHasName(Call, C, "kvzalloc")) {
    markAllocZeroedAndSize(Call, C, 0);
    return;
  }

  // kcalloc(nmemb, size, gfp)
  if (callHasName(Call, C, "kcalloc")) {
    markKcallocZeroedAndSize(Call, C, 0, 1);
    return;
  }

  // memset(ptr, 0, len)
  maybeHandleMemset(Call, C);
}

// Detect copy_to_user and frees.
void SAGenTestChecker::checkPreCall(const CallEvent &Call, CheckerContext &C) const {
  ProgramStateRef State = C.getState();

  // copy_to_user(user_dst, kernel_src, size)
  if (callHasName(Call, C, "copy_to_user")) {
    if (Call.getNumArgs() < 2)
      return;
    SymbolRef SrcSym = getPointerSymbol(Call.getArgSVal(1));
    if (!SrcSym)
      return;

    const bool *Uninit = State->get<UninitAllocMap>(SrcSym);
    if (!Uninit || !*Uninit)
      return;

    // Optional refinement: if copy length is known smaller than alloc size, skip.
    const uint64_t *AllocSize = State->get<AllocByteSizeMap>(SrcSym);
    uint64_t CopyLen = 0;
    bool HasCopyLen = getArgConstUInt(Call, 2, C, CopyLen);

    if (AllocSize && HasCopyLen && CopyLen < *AllocSize) {
      return; // likely copying only initialized prefix
    }

    reportLeak(Call, C, Call.getArgExpr(1));
    return;
  }

  // Free: kfree(ptr), kvfree(ptr)
  if (callHasName(Call, C, "kfree") || callHasName(Call, C, "kvfree")) {
    if (Call.getNumArgs() < 1)
      return;
    SymbolRef Sym = getPointerSymbol(Call.getArgSVal(0));
    if (!Sym)
      return;

    State = State->remove<UninitAllocMap>(Sym);
    State = State->remove<AllocByteSizeMap>(Sym);
    C.addTransition(State);
    return;
  }
}

// Assignments preserve the allocation pointer symbol, so following a kmalloc
// result to its use requires no alias bookkeeping.
void SAGenTestChecker::checkBind(SVal Loc, SVal Val, const Stmt *StoreE, CheckerContext &C) const {
  (void)Loc; (void)Val; (void)StoreE; (void)C;
}

// Cleanup on region invalidation.
ProgramStateRef SAGenTestChecker::checkRegionChanges(ProgramStateRef State,
                                                     const InvalidatedSymbols *Invalidated,
                                                     ArrayRef<const MemRegion *> ExplicitRegions,
                                                     ArrayRef<const MemRegion *> Regions,
                                                     const LocationContext *LCtx,
                                                     const CallEvent *Call) const {
  auto CleanupRegions = [&State](ArrayRef<const MemRegion *> Regs) {
    for (const MemRegion *R : Regs) {
      if (!R) continue;
      const MemRegion *B = R->getBaseRegion();
      if (!B) continue;
      if (const auto *SR = dyn_cast<SymbolicRegion>(B)) {
        SymbolRef Sym = SR->getSymbol();
        State = State->remove<UninitAllocMap>(Sym);
        State = State->remove<AllocByteSizeMap>(Sym);
      }
    }
  };
  CleanupRegions(ExplicitRegions);
  CleanupRegions(Regions);
  // Drop tracked allocations whose pointer symbol was invalidated.
  if (Invalidated)
    for (SymbolRef Sym : *Invalidated) {
      State = State->remove<UninitAllocMap>(Sym);
      State = State->remove<AllocByteSizeMap>(Sym);
    }
  return State;
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects copy_to_user from kmalloc() buffers that may contain uninitialized bytes",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
