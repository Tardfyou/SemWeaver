import unittest

from src.tools.artifact_review import ArtifactReviewTool


class ArtifactReviewSpecificityTests(unittest.TestCase):
    def test_refine_rejects_exact_enclosing_function_filter(self):
        code = r'''
class Checker : public Checker<check::ASTCodeBody> {
public:
  void checkASTCodeBody(const Decl *D, AnalysisManager &, BugReporter &) const {
    const auto *FD = dyn_cast<FunctionDecl>(D);
    if (!FD || FD->getNameAsString() != "mlx5hws_definer_calc_layout")
      return;
  }
};
'''
        findings, _warnings = ArtifactReviewTool()._review_csa_refine(code)
        self.assertTrue(any("enclosing function" in item for item in findings))

    def test_refine_allows_ast_body_without_patch_function_name_gate(self):
        code = r'''
class Checker : public Checker<check::ASTCodeBody> {
public:
  void checkASTCodeBody(const Decl *D, AnalysisManager &, BugReporter &) const {
    const auto *FD = dyn_cast<FunctionDecl>(D);
    if (!FD || !FD->hasBody())
      return;
  }
};
'''
        findings, _warnings = ArtifactReviewTool()._review_csa_refine(code)
        self.assertFalse(any("enclosing function" in item for item in findings))

    def test_refine_rejects_unqualified_positional_output_tracking(self):
        code = r'''
REGISTER_MAP_WITH_PROGRAMSTATE(PendingOutputMap, const MemRegion *, SymbolRef)
class Checker : public Checker<check::PostCall> {
  void checkPostCall(const CallEvent &Call, CheckerContext &C) const {
    const auto *CE = dyn_cast_or_null<CallExpr>(Call.getOriginExpr());
    if (!CE || CE->getNumArgs() < 1) return;
    const MemRegion *MR = getMemRegionFromExpr(CE->getArg(0), C);
    SymbolRef Status = Call.getReturnValue().getAsSymbol();
    if (MR && Status)
      C.addTransition(C.getState()->set<PendingOutputMap>(MR, Status));
  }
};
// Detect use of an output parameter from a failed producer call.
'''
        findings, _warnings = ArtifactReviewTool()._review_csa_refine(code)
        self.assertTrue(any("output-parameter" in item for item in findings))

    def test_refine_allows_output_tracking_with_argument_shape_proof(self):
        code = r'''
REGISTER_MAP_WITH_PROGRAMSTATE(PendingOutputMap, const MemRegion *, SymbolRef)
class Checker : public Checker<check::PostCall> {
  void checkPostCall(const CallEvent &Call, CheckerContext &C) const {
    const auto *CE = dyn_cast_or_null<CallExpr>(Call.getOriginExpr());
    SymbolRef Status = Call.getReturnValue().getAsSymbol();
    for (const Expr *Arg : CE->arguments()) {
      const auto *Address = dyn_cast<UnaryOperator>(Arg->IgnoreParenImpCasts());
      if (!Address || Address->getOpcode() != UO_AddrOf ||
          !Address->getSubExpr()->getType()->isPointerType())
        continue;
      const MemRegion *MR = getOutputRegion(Arg, C);
      C.addTransition(C.getState()->set<PendingOutputMap>(MR, Status));
    }
  }
};
// Detect use of an output parameter from a failed producer call.
'''
        findings, _warnings = ArtifactReviewTool()._review_csa_refine(code)
        self.assertFalse(any("任意调用" in item for item in findings))

    def test_refine_rejects_descendant_only_branch_consumer(self):
        code = r'''
REGISTER_MAP_WITH_PROGRAMSTATE(PendingOutputMap, const MemRegion *, SymbolRef)
class Checker : public Checker<check::PostCall, check::BranchCondition> {
  void checkPostCall(const CallEvent &, CheckerContext &) const;
  void checkBranchCondition(const Stmt *Condition, CheckerContext &C) const {
    const DeclRefExpr *DRE = findSpecificTypeInChildren<DeclRefExpr>(Condition);
    if (!DRE) return;
    const MemRegion *Region = getMemRegionFromExpr(DRE, C);
    if (C.getState()->get<PendingOutputMap>(Region)) reportUncheckedUse(Condition, C);
  }
};
// Fallible output parameter consumer tracking.
'''
        findings, _warnings = ArtifactReviewTool()._review_csa_refine(code)
        self.assertTrue(any("direct branch consumer" in item for item in findings))

    def test_refine_allows_descendant_search_with_location_fallback(self):
        code = r'''
REGISTER_MAP_WITH_PROGRAMSTATE(PendingOutputMap, const MemRegion *, SymbolRef)
class Checker : public Checker<check::PostCall, check::BranchCondition,
                               check::Location> {
  void checkPostCall(const CallEvent &, CheckerContext &) const;
  void checkBranchCondition(const Stmt *Condition, CheckerContext &C) const {
    const DeclRefExpr *DRE = findSpecificTypeInChildren<DeclRefExpr>(Condition);
  }
  void checkLocation(SVal, bool, const Stmt *, CheckerContext &) const;
};
// Fallible output parameter consumer tracking.
'''
        findings, _warnings = ArtifactReviewTool()._review_csa_refine(code)
        self.assertFalse(any("direct branch consumer" in item for item in findings))

    def test_refine_allows_two_step_root_expression_handling(self):
        code = r'''
REGISTER_MAP_WITH_PROGRAMSTATE(PendingOutputMap, const MemRegion *, SymbolRef)
class Checker : public Checker<check::PostCall, check::BranchCondition> {
  void checkPostCall(const CallEvent &, CheckerContext &) const;
  void checkBranchCondition(const Stmt *Condition, CheckerContext &C) const {
    const Expr *ConditionExpr = dyn_cast_or_null<Expr>(Condition);
    const Expr *Root = ConditionExpr ? ConditionExpr->IgnoreParenImpCasts() : nullptr;
    const DeclRefExpr *DRE = dyn_cast_or_null<DeclRefExpr>(Root);
    if (!DRE) DRE = findSpecificTypeInChildren<DeclRefExpr>(Condition);
  }
};
// Fallible output parameter consumer tracking.
'''
        findings, _warnings = ArtifactReviewTool()._review_csa_refine(code)
        self.assertFalse(any("direct branch consumer" in item for item in findings))

    def test_refine_requires_location_for_output_load_region(self):
        code = r'''
REGISTER_MAP_WITH_PROGRAMSTATE(PendingOutputMap, const MemRegion *, SymbolRef)
class Checker : public Checker<check::PostCall, check::BranchCondition> {
  void checkPostCall(const CallEvent &, CheckerContext &) const;
  void checkBranchCondition(const Stmt *, CheckerContext &) const;
};
// Track a fallible output parameter consumer.
'''
        findings, _warnings = ArtifactReviewTool()._review_csa_refine(code)
        self.assertTrue(any("output load consumer" in item for item in findings))

    def test_refine_rejects_region_lookup_on_output_value_expression(self):
        code = r'''
REGISTER_MAP_WITH_PROGRAMSTATE(PendingOutputMap, const MemRegion *, SymbolRef)
class Checker : public Checker<check::PostCall, check::Location> {
  void checkPostCall(const CallEvent &Call, CheckerContext &C) const {
    const Expr *Arg = Call.getOriginExpr();
    const auto *Address = dyn_cast<UnaryOperator>(Arg->IgnoreParenImpCasts());
    const Expr *Output = Address->getSubExpr()->IgnoreParenImpCasts();
    const MemRegion *MR = getMemRegionFromExpr(Output, C);
  }
  void checkLocation(SVal, bool, const Stmt *, CheckerContext &) const;
};
// Track a fallible output parameter consumer.
'''
        findings, _warnings = ArtifactReviewTool()._review_csa_refine(code)
        self.assertTrue(any("address-taken region binding" in item for item in findings))

    def test_refine_rejects_widening_checker_without_var_decl_consumer(self):
        code = r'''
class Checker : public Checker<check::PreStmt<BinaryOperator>> {
  bool isConsumedAsWiderInteger(const BinaryOperator *B, CheckerContext &C) {
    auto Parents = C.getASTContext().getParents(*B);
    for (const auto &Parent : Parents)
      if (const auto *E = Parent.get<Expr>()) return E->getType()->isIntegerType();
    return false;
  }
};
// Potential integer overflow from left shift without upcasting to a wider integer.
'''
        findings, _warnings = ArtifactReviewTool()._review_csa_refine(code)
        self.assertTrue(any("wide VarDecl consumer" in item for item in findings))

    def test_refine_rejects_late_negated_registration_unwrap(self):
        code = r'''
// Detect redundant cleanup after registration failure.
const auto *Registration = dyn_cast<CallExpr>(ParentIf->getCond()->IgnoreParenImpCasts());
const Expr *Condition = ParentIf->getCond()->IgnoreParenImpCasts();
if (const auto *Negated = dyn_cast<UnaryOperator>(Condition)) handleCleanup();
'''
        findings, _warnings = ArtifactReviewTool()._review_csa_refine(code)
        self.assertTrue(any("negated registration" in item for item in findings))

    def test_refine_rejects_guard_only_off_by_one_capacity_model(self):
        code = r'''
class Checker : public Checker<check::BranchCondition> {
  void checkBranchCondition(const Stmt *, CheckerContext &) const;
};
const auto *Index = dyn_cast<ArraySubscriptExpr>(ExprNode);
auto Size = ArrayType->getSize();
// Off-by-one array boundary guard.
'''
        findings, _warnings = ArtifactReviewTool()._review_csa_refine(code)
        self.assertTrue(any("downstream capacity binding" in item for item in findings))


if __name__ == "__main__":
    unittest.main()
