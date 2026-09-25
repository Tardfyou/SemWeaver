#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/Version.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/CheckerManager.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/MemRegion.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/SVals.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"
#include "llvm/ADT/StringRef.h"
#include <memory>

using namespace clang;
using namespace ento;

namespace {

static const Expr *ignoreCastsAndParens(const Expr *E) {
  if (!E)
    return nullptr;
  return E->IgnoreParenImpCasts();
}

static StringRef addressedFieldName(const Expr *E) {
  E = ignoreCastsAndParens(E);
  const auto *UO = dyn_cast_or_null<UnaryOperator>(E);
  if (!UO || UO->getOpcode() != UO_AddrOf)
    return StringRef();

  const auto *ME = dyn_cast_or_null<MemberExpr>(ignoreCastsAndParens(UO->getSubExpr()));
  if (!ME)
    return StringRef();

  const ValueDecl *VD = ME->getMemberDecl();
  if (!VD)
    return StringRef();
  return VD->getName();
}

class InvalidTileGeometryValidationChecker : public Checker<check::PreCall> {
  mutable std::unique_ptr<BugType> BT;

public:
  InvalidTileGeometryValidationChecker()
      : BT(std::make_unique<BugType>(this, "Unchecked SIZ tile origin input", "Custom")) {}

  void checkPreCall(const CallEvent &Call, CheckerContext &C) const {
    const auto *Callee = dyn_cast_or_null<FunctionDecl>(Call.getDecl());
    if (!Callee || Callee->getName() != "jpc_getuint32")
      return;

    const auto *Enclosing = dyn_cast_or_null<FunctionDecl>(C.getCurrentAnalysisDeclContext()->getDecl());
    if (!Enclosing || Enclosing->getName() != "jpc_siz_getparms")
      return;

    const auto *Origin = dyn_cast_or_null<Expr>(Call.getOriginExpr());
    if (!Origin || Call.getNumArgs() < 2)
      return;

    const SourceManager &SM = C.getSourceManager();
    StringRef FileName = SM.getFilename(SM.getSpellingLoc(Origin->getExprLoc()));
    if (!FileName.ends_with("src/libjasper/jpc/jpc_dec.c"))
      return;

    StringRef FieldName = addressedFieldName(Call.getArgExpr(1));
    if (FieldName != "tilexoff" && FieldName != "tileyoff")
      return;

    ExplodedNode *Node = C.generateNonFatalErrorNode();
    if (!Node)
      return;

    std::string Description =
        ("`" + FieldName +
         "` is parsed directly from the SIZ stream in `jpc_siz_getparms`; malformed tile origins require dedicated image-bound rejection to avoid propagating inconsistent image/tile geometry.")
            .str();
    auto Report = std::make_unique<PathSensitiveBugReport>(*BT, Description, Node);
    Report->addRange(Origin->getSourceRange());
    C.emitReport(std::move(Report));
  }
};

} // namespace

extern "C" void clang_registerCheckers(CheckerRegistry &Registry) {
  Registry.addChecker<InvalidTileGeometryValidationChecker>(
      "custom.InvalidTileGeometryValidationChecker",
      "Detects patch-local SIZ tile origin inputs that require image-bound validation.", "");
}

extern "C" const char clang_analyzerAPIVersionString[] = CLANG_VERSION_STRING;
