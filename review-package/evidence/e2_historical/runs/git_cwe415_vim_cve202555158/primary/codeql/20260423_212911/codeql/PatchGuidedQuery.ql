/**
 * @name PatchGuidedQuery
 * @description PatchGuidedQuery
 * @kind problem
 * @problem.severity warning
 * @precision medium
 * @id cpp/custom/unknown
 * @tags security
 *       correctness
 */

/**
 * @name PatchGuidedQuery
 * @description Detects patch-local ownership transfer sites where tuple_append_tv(tuple, rettv) is followed by a successful path without resetting rettv->v_type to VAR_UNKNOWN, which can leave the caller freeing a transferred value.
 * @kind problem
 * @problem.severity warning
 * @precision medium
 * @id cpp/custom/patch-guided-query
 */

import cpp

predicate inTupleFile(Element e) {
  e.getFile().getRelativePath() = "src/tuple.c"
}

predicate exprContainsVariableAccess(Expr root, VariableAccess access) {
  access = root or root.getAChild*() = access
}

predicate isTupleAppendOnRettv(FunctionCall call, Variable rettvVar) {
  inTupleFile(call) and
  call.getTarget().hasName("tuple_append_tv") and
  exists(VariableAccess rettvArg |
    rettvArg = call.getArgument(1) and
    rettvArg.getTarget() = rettvVar
  )
}

predicate assignsUnknownToRettvTypeAfter(Function f, FunctionCall call, Variable rettvVar) {
  exists(Assignment assign, FieldAccess lhs, VariableAccess qual, Expr rhs |
    assign.getEnclosingFunction() = f and
    lhs = assign.getLValue() and
    qual = lhs.getQualifier() and
    qual.getTarget() = rettvVar and
    lhs.getTarget().hasName("v_type") and
    rhs = assign.getRValue() and
    exprContainsVariableAccess(rhs, any(VariableAccess va | va.getTarget().hasName("VAR_UNKNOWN"))) and
    assign.getLocation().getStartLine() > call.getLocation().getStartLine()
  )
}

from FunctionCall call, Function f, Variable rettvVar
where
  f = call.getEnclosingFunction() and
  isTupleAppendOnRettv(call, rettvVar) and
  not assignsUnknownToRettvTypeAfter(f, call, rettvVar)
select call, "`tuple_append_tv(..., rettv)` transfers the first item into the tuple, but no later assignment sets `rettv->v_type` to `VAR_UNKNOWN` in this function."