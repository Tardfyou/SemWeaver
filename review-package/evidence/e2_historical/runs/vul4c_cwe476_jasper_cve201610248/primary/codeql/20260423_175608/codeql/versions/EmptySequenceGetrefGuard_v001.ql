/**
 * @name EmptySequenceGetrefGuard
 * @description 在 `src/libjasper/jpc/jpc_tsfb.c` 的 `jpc_tsfb_synthesize` 中，定位把 `jas_seq2d_getref(a, ...)` 作为实参传给 `jpc_tsfb_synthesize2` 的表达式，并要求其位于以 `tsfb->numlvls > 0` 为主导条件的返回/条件表达式中，同时不存在同一条件里对 `jas_seq2d_size(a)` 的非空/非零约束，从而报告空序列上获取内部引用的补丁相关缺陷。
 * @kind problem
 * @problem.severity warning
 * @precision medium
 * @id cpp/custom/null_dereference
 * @tags security
 *       correctness
 */

import cpp

predicate exprContains(Expr root, Expr sub) {
  sub = root or root.getAChild*() = sub
}

predicate sameVariableExpr(Expr e1, Expr e2) {
  exists(VariableAccess v1, VariableAccess v2 |
    exprContains(e1, v1) and
    exprContains(e2, v2) and
    v1.getTarget() = v2.getTarget()
  )
}

predicate inPatchScope(FunctionCall call) {
  exists(Function f |
    f.getName() = "jpc_tsfb_synthesize" and
    call.getEnclosingFunction() = f
  )
}

predicate isGetrefOnSeqArg(FunctionCall getrefCall, Expr seqArg) {
  getrefCall.getTarget().hasName("jas_seq2d_getref") and
  seqArg = getrefCall.getArgument(0)
}

predicate isPatchLocalTrigger(FunctionCall workerCall, FunctionCall getrefCall, Expr seqArg) {
  inPatchScope(workerCall) and
  workerCall.getTarget().hasName("jpc_tsfb_synthesize2") and
  isGetrefOnSeqArg(getrefCall, seqArg) and
  exists(int i |
    i >= 0 and
    i < workerCall.getNumberOfArguments() and
    workerCall.getArgument(i) = getrefCall
  )
}

predicate fixedHasBarrier(FunctionCall workerCall, Expr seqArg) {
  exists(ConditionalExpr cond, FunctionCall sizeCall |
    exprContains(cond.getThen(), workerCall) and
    sizeCall.getTarget().hasName("jas_seq2d_size") and
    exprContains(cond.getCondition(), sizeCall) and
    sameVariableExpr(sizeCall.getArgument(0), seqArg)
  )
}

from FunctionCall workerCall, FunctionCall getrefCall, Expr seqArg
where isPatchLocalTrigger(workerCall, getrefCall, seqArg)
  and not fixedHasBarrier(workerCall, seqArg)
select getrefCall, "Obtains an internal sequence reference via jas_seq2d_getref and forwards it to jpc_tsfb_synthesize2 in jpc_tsfb_synthesize without a jas_seq2d_size(a) non-empty guard."