/**
 * @name EmptySequenceGetrefWithoutSizeGuard
 * @description 在 `src/libjasper/jpc/jpc_tsfb.c` 的 `jpc_tsfb_synthesize` 中，定位当条件仅检查 `tsfb->numlvls > 0` 时，对参数 `a` 调用 `jas_seq2d_getref(a, jas_seq2d_xstart(a), jas_seq2d_ystart(a))` 并将结果传给 `jpc_tsfb_synthesize2` 的表达式；若该触点未受同一条件中的 `jas_seq2d_size(a)` 非零约束，则报告其可能从空 `jas_seq2d_t` 获取无效内部引用并导致后续空指针解引用。
 * @kind problem
 * @problem.severity warning
 * @precision medium
 * @id cpp/custom/null_dereference
 * @tags security
 *       correctness
 */

import cpp

predicate exprContains(Expr root, Expr child) {
  child = root or root.getAChild*() = child
}

predicate sameVariableExpr(Expr e1, Expr e2) {
  exists(VariableAccess v1, VariableAccess v2 |
    exprContains(e1, v1) and
    exprContains(e2, v2) and
    v1.getTarget() = v2.getTarget()
  )
}

predicate inPatchFunction(Function f) {
  f.getName() = "jpc_tsfb_synthesize" and
  f.getFile().getRelativePath().matches("%src/libjasper/jpc/jpc_tsfb.c")
}

predicate isPatchedSinkCall(FunctionCall sinkCall, Expr seqArg) {
  sinkCall.getTarget().hasName("jpc_tsfb_synthesize2") and
  exists(FunctionCall getrefCall |
    getrefCall = sinkCall.getArgument(0) and
    getrefCall.getTarget().hasName("jas_seq2d_getref") and
    seqArg = getrefCall.getArgument(0)
  )
}

predicate hasSizeBarrierFor(FunctionCall sinkCall, Expr seqArg) {
  exists(FunctionCall sizeCall |
    sizeCall.getTarget().hasName("jas_seq2d_size") and
    sizeCall.getEnclosingFunction() = sinkCall.getEnclosingFunction() and
    sameVariableExpr(sizeCall.getArgument(0), seqArg) and
    exists(Expr condition |
      exprContains(condition, sizeCall) and
      exprContains(condition, sinkCall)
    )
  )
}

from FunctionCall sinkCall, Expr seqArg
where
  inPatchFunction(sinkCall.getEnclosingFunction()) and
  isPatchedSinkCall(sinkCall, seqArg) and
  not hasSizeBarrierFor(sinkCall, seqArg)
select sinkCall, "`jas_seq2d_getref` result derived from a possibly empty sequence is passed to `jpc_tsfb_synthesize2` without a `jas_seq2d_size(...)` guard."