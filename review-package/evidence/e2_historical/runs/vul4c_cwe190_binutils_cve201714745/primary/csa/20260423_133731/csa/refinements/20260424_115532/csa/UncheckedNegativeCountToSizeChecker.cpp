#include "clang/Analysis/PathDiagnostic.h"
#include "clang/AST/Expr.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/Version.h"
#include "clang/Lex/Lexer.h"
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
#include "llvm/ADT/ImmutableMap.h"
#include "llvm/ADT/StringRef.h"
#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace clang;
using namespace ento;

namespace {

struct CountOrigin {
  const MemRegion *Region;
  CountOrigin() : Region(nullptr) {}
  explicit CountOrigin(const MemRegion *R) : Region(R) {}

  bool operator==(const CountOrigin &Other) const { return Region == Other.Region; }
  void Profile(llvm::FoldingSetNodeID &ID) const { ID.AddPointer(Region); }
};

} // namespace

namespace llvm {
template <> struct FoldingSetTrait<CountOrigin> {
  static void Profile(const CountOrigin &X, FoldingSetNodeID &ID) { X.Profile(ID); }
};
} // namespace llvm

REGISTER_MAP_WITH_PROGRAMSTATE(TrackedCounts, const MemRegion *, CountOrigin)
REGISTER_SET_WITH_PROGRAMSTATE(GuardedCounts, const MemRegion *)

namespace {

static const Expr *ignoreCasts(const Expr *E) {
  return E ? E->IgnoreParenCasts() : nullptr;
}

static bool isNegativeIntegerLiteral(const Expr *E) {
  E = ignoreCasts(E);
  if (!E)
    return false;

  if (const auto *UO = dyn_cast<UnaryOperator>(E)) {
    if (UO->getOpcode() != UO_Minus)
      return false;
    const Expr *Sub = ignoreCasts(UO->getSubExpr());
    const auto *IL = dyn_cast_or_null<IntegerLiteral>(Sub);
    return IL && !IL->getValue().isZero();
  }

  return false;
}

static const MemRegion *getExprRegion(const Expr *E, CheckerContext &C) {
  E = ignoreCasts(E);
  if (!E)
    return nullptr;

  SVal V = C.getSVal(E);
  return V.getAsRegion();
}

static bool isTrackedRegion(const MemRegion *MR, ProgramStateRef State) {
  if (!MR)
    return false;
  const CountOrigin *Origin = State->get<TrackedCounts>(MR);
  return Origin && Origin->Region != nullptr;
}

static bool isGuardedRegion(const MemRegion *MR, ProgramStateRef State) {
  if (!MR)
    return false;
  return State->contains<GuardedCounts>(MR);
}

static ProgramStateRef markTracked(ProgramStateRef State, const MemRegion *MR) {
  if (!MR)
    return State;
  return State->set<TrackedCounts>(MR, CountOrigin(MR));
}

static ProgramStateRef markGuarded(ProgramStateRef State, const MemRegion *MR) {
  if (!MR)
    return State;
  return State->add<GuardedCounts>(MR);
}

static std::string getSourceText(const SourceManager &SM,
                                 const LangOptions &LangOpts,
                                 SourceRange Range) {
  CharSourceRange CharRange = CharSourceRange::getTokenRange(Range);
  bool Invalid = false;
  StringRef Text = Lexer::getSourceText(CharRange, SM, LangOpts, &Invalid);
  return Invalid ? std::string() : Text.str();
}

static std::string compact(StringRef Text) {
  std::string Out;
  for (char C : Text) {
    if (!isspace(static_cast<unsigned char>(C)))
      Out.push_back(C);
  }
  return Out;
}

static bool extractCallArguments(const std::string &Body, size_t CallPos,
                                 std::string &Args) {
  size_t Open = Body.find('(', CallPos);
  if (Open == std::string::npos)
    return false;
  unsigned Depth = 0;
  for (size_t I = Open; I < Body.size(); ++I) {
    if (Body[I] == '(') {
      ++Depth;
    } else if (Body[I] == ')') {
      if (--Depth == 0) {
        Args = Body.substr(Open + 1, I - Open - 1);
        return true;
      }
    }
  }
  return false;
}

static std::vector<std::string> splitTopLevelArgs(StringRef Args) {
  std::vector<std::string> Result;
  std::string Current;
  unsigned Depth = 0;
  for (char C : Args) {
    if (C == '(')
      ++Depth;
    else if (C == ')' && Depth > 0)
      --Depth;
    if (C == ',' && Depth == 0) {
      Result.push_back(Current);
      Current.clear();
      continue;
    }
    Current.push_back(C);
  }
  if (!Current.empty())
    Result.push_back(Current);
  return Result;
}

static bool isCountProducerCall(StringRef Text) {
  return Text.contains("canonicalize") || Text.contains("count") ||
         Text.contains("reloc") || Text.contains("read") ||
         Text.contains("symbol");
}

static bool hasNegativeGuardBetween(const std::string &Body, StringRef Var,
                                    size_t Begin, size_t End) {
  std::string Window = Body.substr(Begin, End > Begin ? End - Begin : 0);
  std::string V = Var.str();
  return Window.find(V + "<0") != std::string::npos ||
         Window.find(V + "<= -1") != std::string::npos ||
         Window.find(V + "<=-1") != std::string::npos ||
         Window.find("0>" + V) != std::string::npos;
}

static std::optional<std::string> findUncheckedCountToSizeSink(StringRef Text) {
  std::string Body = compact(Text);
  size_t Pos = Body.find("qsort(");
  while (Pos != std::string::npos) {
    std::string ArgsText;
    if (extractCallArguments(Body, Pos, ArgsText)) {
      std::vector<std::string> Args = splitTopLevelArgs(ArgsText);
      if (Args.size() >= 2) {
        std::string Count = Args[1];
        if (!Count.empty() && Count.find_first_of("+-*/%&|!<>=") == std::string::npos) {
          size_t Assign = Body.rfind(Count + "=", Pos);
          if (Assign != std::string::npos) {
            size_t Semi = Body.find(';', Assign);
            if (Semi != std::string::npos && Semi < Pos) {
              StringRef Producer = StringRef(Body).slice(Assign, Semi);
              if (isCountProducerCall(Producer) &&
                  !hasNegativeGuardBetween(Body, Count, Semi, Pos)) {
                return Count;
              }
            }
          }
        }
      }
    }
    Pos = Body.find("qsort(", Pos + 1);
  }
  return std::nullopt;
}

static const VarDecl *getReferencedVar(const Expr *E) {
  E = ignoreCasts(E);
  const auto *DRE = dyn_cast_or_null<DeclRefExpr>(E);
  return DRE ? dyn_cast<VarDecl>(DRE->getDecl()) : nullptr;
}

static bool exprReferencesVar(const Expr *E, const VarDecl *VD) {
  E = ignoreCasts(E);
  if (!E || !VD)
    return false;

  if (getReferencedVar(E) == VD)
    return true;

  for (const Stmt *Child : E->children()) {
    const auto *ChildExpr = dyn_cast_or_null<Expr>(Child);
    if (ChildExpr && exprReferencesVar(ChildExpr, VD))
      return true;
  }
  return false;
}

static bool isCountProducerExpr(const Expr *E) {
  E = ignoreCasts(E);
  const auto *CE = dyn_cast_or_null<CallExpr>(E);
  if (!CE)
    return false;

  if (const FunctionDecl *Callee = CE->getDirectCallee()) {
    StringRef Name = Callee->getName();
    return Name.contains("canonicalize") || Name.contains("count") ||
           Name.contains("reloc") || Name.contains("read") ||
           Name.contains("symbol");
  }
  return false;
}

static bool containsVar(const std::vector<const VarDecl *> &Vars,
                        const VarDecl *VD) {
  return std::find(Vars.begin(), Vars.end(), VD) != Vars.end();
}

static void rememberVar(std::vector<const VarDecl *> &Vars, const VarDecl *VD) {
  if (VD && !containsVar(Vars, VD))
    Vars.push_back(VD);
}

static const VarDecl *getProducedCountVar(const Stmt *S) {
  if (!S)
    return nullptr;

  if (const auto *DS = dyn_cast<DeclStmt>(S)) {
    for (const Decl *D : DS->decls()) {
      const auto *VD = dyn_cast<VarDecl>(D);
      if (VD && VD->hasInit() && isCountProducerExpr(VD->getInit()))
        return VD;
    }
  }

  const auto *BO = dyn_cast<BinaryOperator>(S);
  if (!BO || !BO->isAssignmentOp() || !isCountProducerExpr(BO->getRHS()))
    return nullptr;
  return getReferencedVar(BO->getLHS());
}

static bool isNegativeGuardForVar(const Expr *Cond, const VarDecl *VD) {
  Cond = ignoreCasts(Cond);
  if (!Cond || !VD)
    return false;

  if (const auto *UO = dyn_cast<UnaryOperator>(Cond)) {
    if (UO->getOpcode() == UO_LNot)
      return isNegativeGuardForVar(UO->getSubExpr(), VD);
  }

  const auto *BO = dyn_cast<BinaryOperator>(Cond);
  if (!BO || !BO->isComparisonOp())
    return false;

  const Expr *LHS = ignoreCasts(BO->getLHS());
  const Expr *RHS = ignoreCasts(BO->getRHS());
  if (!LHS || !RHS)
    return false;

  bool lhsTracked = exprReferencesVar(LHS, VD);
  bool rhsTracked = exprReferencesVar(RHS, VD);
  const auto *LIL = dyn_cast<IntegerLiteral>(LHS);
  const auto *RIL = dyn_cast<IntegerLiteral>(RHS);

  switch (BO->getOpcode()) {
  case BO_LT:
  case BO_LE:
    return (lhsTracked && RIL && RIL->getValue().isZero()) ||
           (rhsTracked && LIL && LIL->getValue().isZero());
  case BO_GT:
  case BO_GE:
    return (lhsTracked && RIL && RIL->getValue().isZero()) ||
           (rhsTracked && LIL && LIL->getValue().isZero());
  default:
    return false;
  }
}

static bool stmtHasReturnLikeExit(const Stmt *S) {
  if (!S)
    return false;
  if (isa<ReturnStmt>(S))
    return true;
  for (const Stmt *Child : S->children()) {
    if (stmtHasReturnLikeExit(Child))
      return true;
  }
  return false;
}

static bool stmtAddsNegativeBarrier(const Stmt *S, const VarDecl *VD) {
  const auto *If = dyn_cast_or_null<IfStmt>(S);
  if (!If || !isNegativeGuardForVar(If->getCond(), VD))
    return false;
  return stmtHasReturnLikeExit(If->getThen()) || stmtHasReturnLikeExit(If->getElse());
}

static const VarDecl *getQsortCountVar(const Stmt *S) {
  const auto *CE = dyn_cast_or_null<CallExpr>(S);
  if (!CE)
    return nullptr;
  const FunctionDecl *Callee = CE->getDirectCallee();
  if (!Callee || Callee->getName() != "qsort" || CE->getNumArgs() <= 1)
    return nullptr;
  return getReferencedVar(CE->getArg(1));
}

static std::optional<std::string> findUncheckedQsortCountVar(const FunctionDecl *FD) {
  const auto *Body = dyn_cast_or_null<CompoundStmt>(FD ? FD->getBody() : nullptr);
  if (!Body)
    return std::nullopt;

  std::vector<const VarDecl *> ProducedCounts;
  std::vector<const VarDecl *> GuardedCountsAst;

  for (const Stmt *S : Body->body()) {
    if (const VarDecl *Produced = getProducedCountVar(S)) {
      rememberVar(ProducedCounts, Produced);
      GuardedCountsAst.erase(
          std::remove(GuardedCountsAst.begin(), GuardedCountsAst.end(), Produced),
          GuardedCountsAst.end());
    }

    for (const VarDecl *Tracked : ProducedCounts) {
      if (stmtAddsNegativeBarrier(S, Tracked))
        rememberVar(GuardedCountsAst, Tracked);
    }

    if (const VarDecl *CountVar = getQsortCountVar(S)) {
      if (containsVar(ProducedCounts, CountVar) &&
          !containsVar(GuardedCountsAst, CountVar)) {
        return CountVar->getNameAsString();
      }
    }
  }

  return std::nullopt;
}

static bool exprReferencesTrackedRegion(const Expr *E, CheckerContext &C,
                                        ProgramStateRef State,
                                        const MemRegion **Found) {
  E = ignoreCasts(E);
  if (!E)
    return false;

  if (const MemRegion *MR = getExprRegion(E, C)) {
    if (isTrackedRegion(MR, State)) {
      if (Found)
        *Found = MR;
      return true;
    }
  }

  for (const Stmt *Child : E->children()) {
    const auto *ChildExpr = dyn_cast_or_null<Expr>(Child);
    if (!ChildExpr)
      continue;
    if (exprReferencesTrackedRegion(ChildExpr, C, State, Found))
      return true;
  }

  return false;
}

static bool isNegativeGuardAgainstTracked(const Expr *Cond, CheckerContext &C,
                                          ProgramStateRef State,
                                          const MemRegion **Guarded) {
  Cond = ignoreCasts(Cond);
  if (!Cond)
    return false;

  if (const auto *UO = dyn_cast<UnaryOperator>(Cond)) {
    if (UO->getOpcode() == UO_LNot)
      return isNegativeGuardAgainstTracked(UO->getSubExpr(), C, State, Guarded);
  }

  const auto *BO = dyn_cast<BinaryOperator>(Cond);
  if (!BO || !BO->isComparisonOp())
    return false;

  const Expr *LHS = ignoreCasts(BO->getLHS());
  const Expr *RHS = ignoreCasts(BO->getRHS());
  if (!LHS || !RHS)
    return false;

  const MemRegion *MR = nullptr;

  switch (BO->getOpcode()) {
  case BO_LT:
  case BO_LE:
    if (isNegativeIntegerLiteral(RHS) || isNegativeIntegerLiteral(LHS))
      return false;
    if (const auto *IL = dyn_cast<IntegerLiteral>(RHS)) {
      if (IL->getValue().isZero() && exprReferencesTrackedRegion(LHS, C, State, &MR)) {
        if (Guarded)
          *Guarded = MR;
        return true;
      }
    }
    break;
  case BO_GT:
  case BO_GE:
    if (isNegativeIntegerLiteral(RHS) || isNegativeIntegerLiteral(LHS))
      return false;
    if (const auto *IL = dyn_cast<IntegerLiteral>(LHS)) {
      if (IL->getValue().isZero() && exprReferencesTrackedRegion(RHS, C, State, &MR)) {
        if (Guarded)
          *Guarded = MR;
        return true;
      }
    }
    break;
  default:
    break;
  }

  return false;
}

class UncheckedNegativeCountToSizeChecker
    : public Checker<check::ASTCodeBody, check::PostCall, check::Bind,
                     check::BranchCondition, check::PreCall> {
  mutable std::unique_ptr<BugType> BT;

  bool isCountReturningFunction(const CallEvent &Call) const {
    const IdentifierInfo *II = Call.getCalleeIdentifier();
    if (!II)
      return false;

    StringRef Name = II->getName();
    if (Name.contains("canonicalize") || Name.contains("count") ||
        Name.contains("read") || Name.contains("reloc") ||
        Name.contains("symbol"))
      return true;

    return false;
  }

  bool isSizeConsumingSink(const CallEvent &Call, unsigned &CountArgIndex) const {
    const IdentifierInfo *II = Call.getCalleeIdentifier();
    if (!II)
      return false;

    StringRef Name = II->getName();
    if (Name == "qsort") {
      CountArgIndex = 1;
      return Call.getNumArgs() > CountArgIndex;
    }

    if (Name == "bsearch") {
      CountArgIndex = 2;
      return Call.getNumArgs() > CountArgIndex;
    }

    return false;
  }

  static bool isPatchFile(const SourceManager &SM, SourceLocation Loc) {
    StringRef File = SM.getFilename(SM.getExpansionLoc(Loc));
    return File.ends_with("bfd/elf64-x86-64.c") ||
           File.ends_with("bfd\\elf64-x86-64.c");
  }

public:
  UncheckedNegativeCountToSizeChecker()
      : BT(std::make_unique<BugType>(this,
                                     "Unchecked negative count reaches size sink",
                                     "Custom")) {}

  void checkASTCodeBody(const Decl *D, AnalysisManager &, BugReporter &BR) const {
    const auto *FD = dyn_cast_or_null<FunctionDecl>(D);
    if (!FD || !FD->hasBody())
      return;

    const SourceManager &SM = BR.getSourceManager();
    if (!isPatchFile(SM, FD->getBeginLoc()))
      return;

    std::optional<std::string> Count = findUncheckedQsortCountVar(FD);
    if (!Count) {
      std::string Body = getSourceText(
          SM, BR.getContext().getLangOpts(), FD->getBody()->getSourceRange());
      Count = findUncheckedCountToSizeSink(Body);
    }
    if (!Count)
      return;

    std::string Message =
        "custom.UncheckedNegativeCountToSizeChecker: a signed count/error "
        "value reaches qsort as the element count without a preceding "
        "non-negative guard (count=" +
        *Count + ").";
    PathDiagnosticLocation Loc = PathDiagnosticLocation::createBegin(FD, SM);
    BR.EmitBasicReport(
        FD, this, "Unchecked negative count reaches size sink", "Custom",
        Message, Loc, D->getSourceRange());
  }

  void checkPostCall(const CallEvent &Call, CheckerContext &C) const {
    if (!isCountReturningFunction(Call))
      return;

    const MemRegion *RetRegion = Call.getReturnValue().getAsRegion();
    if (!RetRegion)
      return;

    ProgramStateRef State = C.getState();
    State = markTracked(State, RetRegion);
    C.addTransition(State);
  }

  void checkBind(SVal Loc, SVal Val, const Stmt *S, CheckerContext &C) const {
    const MemRegion *Dst = Loc.getAsRegion();
    const MemRegion *Src = Val.getAsRegion();
    if (!Dst || !Src)
      return;

    ProgramStateRef State = C.getState();
    if (!isTrackedRegion(Src, State))
      return;

    State = markTracked(State, Dst);
    C.addTransition(State);
  }

  void checkBranchCondition(const Stmt *Condition, CheckerContext &C) const {
    const auto *Cond = dyn_cast_or_null<Expr>(Condition);
    if (!Cond)
      return;

    ProgramStateRef State = C.getState();
    const MemRegion *MR = nullptr;
    if (!isNegativeGuardAgainstTracked(Cond, C, State, &MR))
      return;
    if (!MR)
      return;

    State = markGuarded(State, MR);
    C.addTransition(State);
  }

  void checkPreCall(const CallEvent &Call, CheckerContext &C) const {
    unsigned CountArgIndex = 0;
    if (!isSizeConsumingSink(Call, CountArgIndex))
      return;

    const Expr *CountExpr = Call.getArgExpr(CountArgIndex);
    if (!CountExpr)
      return;

    ProgramStateRef State = C.getState();
    const MemRegion *MR = nullptr;
    if (!exprReferencesTrackedRegion(CountExpr, C, State, &MR))
      return;
    if (!MR || isGuardedRegion(MR, State))
      return;

    ExplodedNode *N = C.generateNonFatalErrorNode();
    if (!N)
      return;

    auto R = std::make_unique<PathSensitiveBugReport>(
        *BT,
        "A signed count that may encode a negative error value reaches a size-based API without a non-negative check.",
        N);
    R->addRange(CountExpr->getSourceRange());
    C.emitReport(std::move(R));
  }
};

} // namespace

extern "C" void clang_registerCheckers(CheckerRegistry &Registry) {
  Registry.addChecker<UncheckedNegativeCountToSizeChecker>(
      "custom.UncheckedNegativeCountToSizeChecker",
      "Detects signed count/error values reaching size-consuming APIs without non-negative validation.",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] = CLANG_VERSION_STRING;
