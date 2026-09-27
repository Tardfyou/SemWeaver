#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Checkers/Taint.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/Environment.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/MemRegion.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramState.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramStateTrait.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/SymExpr.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/StmtVisitor.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/StaticAnalyzer/Checkers/utility.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Type.h"
#include "clang/AST/Decl.h"
#include "clang/Lex/Lexer.h"
#include "llvm/ADT/SmallVector.h"
#include <memory>

using namespace clang;
using namespace ento;
using namespace taint;

// Track whether a specific count field (as a FieldRegion bound to a specific base
// object) has been initialized. Presence in the map means initialized; value unused.
REGISTER_MAP_WITH_PROGRAMSTATE(CountFieldInitMap, const FieldRegion*, char)

namespace {

class SAGenTestChecker : public Checker<
    check::Bind,
    check::PreCall> {
   mutable std::unique_ptr<BugType> BT;

   public:
      SAGenTestChecker() : BT(new BugType(this, "Write into __counted_by flexible array before count init", "Memory Error")) {}

      void checkBind(SVal Loc, SVal Val, const Stmt *S, CheckerContext &C) const;
      void checkPreCall(const CallEvent &Call, CheckerContext &C) const;

   private:
      // Helpers
      bool isMemTransferCall(const CallEvent &Call, unsigned &DestIndex, unsigned &SizeIndex, CheckerContext &C) const;
      const FieldRegion *peelToFieldRegion(const MemRegion *R) const;
      const FieldDecl *findFieldByNameInRecord(const RecordDecl *RD, StringRef Name) const;
      const FieldDecl *getCountFieldFromAttr(const FieldDecl *FamFD, CheckerContext &C) const;
      bool isFlexibleArrayField(const FieldDecl *FD) const;
      bool isPossiblyNonZeroWrite(const CallEvent &Call, unsigned SizeIndex, CheckerContext &C) const;
      void reportWriteBeforeCountInit(const CallEvent &Call, const Expr *DestExpr, CheckerContext &C) const;
};

void SAGenTestChecker::checkBind(SVal Loc, SVal Val, const Stmt *S, CheckerContext &C) const {
  // Mark any field store as "initialized" for that field region.
  const MemRegion *L = Loc.getAsRegion();
  if (!L)
    return;

  const auto *FR = dyn_cast<FieldRegion>(L);
  if (!FR)
    return;

  ProgramStateRef State = C.getState();
  // We don't filter which field here; the later query will look up the exact
  // count field region.
  State = State->set<CountFieldInitMap>(FR, 1);
  C.addTransition(State);
}

bool SAGenTestChecker::isMemTransferCall(const CallEvent &Call, unsigned &DestIndex, unsigned &SizeIndex, CheckerContext &C) const {
  DestIndex = SizeIndex = 0;

  const auto *FD = dyn_cast_or_null<FunctionDecl>(Call.getDecl());
  if (!FD)
    return false;

  const IdentifierInfo *II = FD->getIdentifier();
  if (!II)
    return false;

  StringRef Name = II->getName();
  if (Name.equals("memcpy") || Name.equals("__builtin_memcpy")) {
    DestIndex = 0; SizeIndex = 2; return true;
  }
  if (Name.equals("memmove") || Name.equals("__builtin_memmove")) {
    DestIndex = 0; SizeIndex = 2; return true;
  }
  if (Name.equals("memset") || Name.equals("__builtin_memset")) {
    DestIndex = 0; SizeIndex = 2; return true;
  }
  return false;
}

const FieldRegion *SAGenTestChecker::peelToFieldRegion(const MemRegion *R) const {
  if (!R) return nullptr;
  // Strip element regions to get down to a field region (e.g., array decay).
  while (isa<ElementRegion>(R)) {
    R = cast<ElementRegion>(R)->getSuperRegion();
    if (!R) return nullptr;
  }
  return dyn_cast<FieldRegion>(R);
}

const FieldDecl *SAGenTestChecker::findFieldByNameInRecord(const RecordDecl *RD, StringRef Name) const {
  if (!RD || Name.empty())
    return nullptr;
  for (const FieldDecl *FD : RD->fields()) {
    if (FD->getIdentifier() && FD->getName().equals(Name))
      return FD;
  }
  return nullptr;
}

const FieldDecl *SAGenTestChecker::getCountFieldFromAttr(const FieldDecl *FamFD, CheckerContext &C) const {
  if (!FamFD)
    return nullptr;

  const auto *Attr = FamFD->getAttr<CountedByAttr>();
  if (!Attr)
    return nullptr;

  const SourceManager &SM = C.getSourceManager();
  StringRef Text = Lexer::getSourceText(
      CharSourceRange::getTokenRange(Attr->getRange()), SM, C.getLangOpts());
  size_t AttrName = Text.find("counted_by");
  if (AttrName == StringRef::npos)
    return nullptr;
  size_t Open = Text.find('(', AttrName);
  size_t Close = Open == StringRef::npos ? StringRef::npos : Text.find(')', Open + 1);
  if (Open == StringRef::npos || Close == StringRef::npos)
    return nullptr;

  StringRef CountName = Text.slice(Open + 1, Close).trim();
  const auto *RD = dyn_cast<RecordDecl>(FamFD->getDeclContext());
  return findFieldByNameInRecord(RD, CountName);
}

bool SAGenTestChecker::isFlexibleArrayField(const FieldDecl *FD) const {
  if (!FD)
    return false;
  QualType T = FD->getType();
  if (const auto *AT = dyn_cast_or_null<ArrayType>(T.getTypePtrOrNull())) {
    // Flexible array is an IncompleteArrayType (i.e., "type name[];").
    return isa<IncompleteArrayType>(AT);
  }
  return false;
}

bool SAGenTestChecker::isPossiblyNonZeroWrite(const CallEvent &Call, unsigned SizeIndex, CheckerContext &C) const {
  if (SizeIndex >= Call.getNumArgs())
    return true; // be conservative

  // Try to evaluate the size argument to a concrete integer.
  SVal SizeSV = Call.getArgSVal(SizeIndex);
  if (auto CI = SizeSV.getAs<nonloc::ConcreteInt>()) {
    const llvm::APSInt &V = CI->getValue();
    return !V.isZero();
  }

  // Unknown or symbolic: be conservative.
  return true;
}

void SAGenTestChecker::reportWriteBeforeCountInit(const CallEvent &Call, const Expr *DestExpr, CheckerContext &C) const {
  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto R = std::make_unique<PathSensitiveBugReport>(
      *BT, "Write to __counted_by flexible array before initializing its count field", N);
  if (DestExpr)
    R->addRange(DestExpr->getSourceRange());
  C.emitReport(std::move(R));
}

void SAGenTestChecker::checkPreCall(const CallEvent &Call, CheckerContext &C) const {
  unsigned DestIndex = 0, SizeIndex = 0;
  if (!isMemTransferCall(Call, DestIndex, SizeIndex, C))
    return;

  // Destination SVal and region
  SVal DestSV = Call.getArgSVal(DestIndex);
  const MemRegion *MR = DestSV.getAsRegion();
  if (!MR)
    return;

  const FieldRegion *FamFR = peelToFieldRegion(MR);
  if (!FamFR)
    return;

  const FieldDecl *FamFD = FamFR->getDecl();
  if (!FamFD)
    return;

  // Must be a __counted_by flexible array
  if (!FamFD->hasAttr<CountedByAttr>())
    return;

  if (!isFlexibleArrayField(FamFD))
    return;

  // Resolve the counting field from the attribute
  const FieldDecl *CountFD = getCountFieldFromAttr(FamFD, C);
  if (!CountFD)
    return;

  // Build the FieldRegion for the count field on the same base object
  const MemRegion *BaseR = FamFR->getSuperRegion();
  if (!BaseR)
    return;

  const SubRegion *BaseForFR = dyn_cast<SubRegion>(BaseR);
  if (!BaseForFR)
    return;

  MemRegionManager &RM = C.getSValBuilder().getRegionManager();
  const auto *CountFR = RM.getFieldRegion(CountFD, BaseForFR);
  if (!CountFR)
    return;

  ProgramStateRef State = C.getState();
  // The count-field store is the barrier that makes this write safe.
  if (State->get<CountFieldInitMap>(CountFR))
    return;

  // Warn only if the write size can be non-zero
  if (!isPossiblyNonZeroWrite(Call, SizeIndex, C))
    return;

  // Report
  const Expr *DestExpr = Call.getArgExpr(DestIndex);
  reportWriteBeforeCountInit(Call, DestExpr, C);
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects writes into __counted_by flexible arrays before initializing their count field",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
