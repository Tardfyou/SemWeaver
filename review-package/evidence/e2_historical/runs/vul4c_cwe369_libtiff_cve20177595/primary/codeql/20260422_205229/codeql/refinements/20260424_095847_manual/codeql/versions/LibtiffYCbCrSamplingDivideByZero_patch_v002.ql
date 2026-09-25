/**
 * @name LibtiffYCbCrSamplingDivideByZero
 * @description 检测在 libtiff 的 YCbCr/JPEG 设置路径中，来自 `td->td_ycbcrsubsampling` 的采样值赋给状态字段后，未经支配性的非零校验即流入后续除法或取模等分母位置的缺陷模式；若存在补丁同类的零值检查并提前返回，则应静默。
 * @kind problem
 * @problem.severity warning
 * @precision medium
 * @id cpp/custom/unknown
 * @tags security
 *       correctness
 */

import cpp
import semmle.code.cpp.controlflow.Guards
import semmle.code.cpp.controlflow.Dominance

predicate inPatchScope(Element e) {
  e.getFile().getRelativePath().matches("%tif_jpeg.c")
}

predicate isZeroExpr(Expr e) {
  exists(Literal lit |
    lit = e and
    lit.getValue().toInt() = 0
  )
}

predicate isOneExpr(Expr e) {
  exists(Literal lit |
    lit = e and
    lit.getValue().toInt() = 1
  )
}

predicate isSamplingFieldRead(Expr e) {
  exists(ArrayExpr arr, FieldAccess fa |
    e = arr and
    arr.getArrayBase() = fa and
    fa.getTarget().hasName("td_ycbcrsubsampling") and
    (
      isZeroExpr(arr.getArrayOffset())
      or
      isOneExpr(arr.getArrayOffset())
    )
  )
}

predicate sameValueExpr(Expr left, Expr right) {
  left = right
  or
  exists(VariableAccess l, VariableAccess r |
    l = left and
    r = right and
    l.getTarget() = r.getTarget()
  )
  or
  exists(FieldAccess l, FieldAccess r |
    l = left and
    r = right and
    l.getTarget() = r.getTarget() and
    sameValueExpr(l.getQualifier(), r.getQualifier())
  )
}

predicate directSamplingAssignmentTarget(Expr lhs, Expr rhs) {
  exists(AssignExpr assign |
    inPatchScope(assign) and
    lhs = assign.getLValue() and
    rhs = assign.getRValue() and
    isSamplingFieldRead(rhs)
  )
}

predicate samplingStateExpr(Expr e) {
  exists(VariableAccess va, Expr rhs |
    va = e and
    directSamplingAssignmentTarget(va, rhs)
  )
  or
  exists(FieldAccess fa, Expr rhs |
    fa = e and
    directSamplingAssignmentTarget(fa, rhs)
  )
}

predicate samplingDerivedStep(Expr from, Expr to) {
  samplingStateExpr(from) and
  sameValueExpr(from, to)
  or
  exists(AssignExpr assign |
    inPatchScope(assign) and
    from = assign.getRValue() and
    to = assign.getLValue() and
    sameValueExpr(assign.getRValue(), from)
  )
}

predicate isSamplingDerivedExpr(Expr e) {
  samplingStateExpr(e)
  or
  exists(Expr mid |
    samplingDerivedStep(mid, e) and
    samplingStateExpr(mid)
  )
}

predicate zeroCheckExpr(Expr checked, Expr cond) {
  exists(EqualityOperation eq |
    eq = cond and
    (
      sameValueExpr(eq.getLeftOperand(), checked) and isZeroExpr(eq.getRightOperand())
      or
      sameValueExpr(eq.getRightOperand(), checked) and isZeroExpr(eq.getLeftOperand())
    )
  )
}

predicate zeroGuardCondition(Expr checked, Expr cond) {
  zeroCheckExpr(checked, cond)
  or
  exists(LogicalOrExpr lor |
    lor = cond and
    (
      zeroGuardCondition(checked, lor.getLeftOperand())
      or
      zeroGuardCondition(checked, lor.getRightOperand())
    )
  )
}

predicate hasPatchGuard(Expr checked, Expr sink) {
  exists(IfStmt ifs, ReturnStmt ret, BasicBlock guardBb, BasicBlock sinkBb |
    inPatchScope(ifs) and
    ret = ifs.getThen().getAChild*() and
    zeroGuardCondition(checked, ifs.getCondition()) and
    guardBb = ifs.getBasicBlock() and
    sinkBb = sink.getBasicBlock() and
    dominates(guardBb, sinkBb)
  )
  or
  exists(FieldAccess peer, IfStmt ifs, ReturnStmt ret, LogicalOrExpr lor, BasicBlock guardBb, BasicBlock sinkBb |
    inPatchScope(ifs) and
    peer.getTarget().hasName("v_sampling") and
    checked instanceof FieldAccess and
    checked.(FieldAccess).getTarget().hasName("h_sampling") and
    lor = ifs.getCondition() and
    zeroGuardCondition(checked, lor.getLeftOperand()) and
    zeroGuardCondition(peer, lor.getRightOperand()) and
    ret = ifs.getThen().getAChild*() and
    guardBb = ifs.getBasicBlock() and
    sinkBb = sink.getBasicBlock() and
    dominates(guardBb, sinkBb)
  )
  or
  exists(FieldAccess peer, IfStmt ifs, ReturnStmt ret, LogicalOrExpr lor, BasicBlock guardBb, BasicBlock sinkBb |
    inPatchScope(ifs) and
    peer.getTarget().hasName("h_sampling") and
    checked instanceof FieldAccess and
    checked.(FieldAccess).getTarget().hasName("v_sampling") and
    lor = ifs.getCondition() and
    zeroGuardCondition(peer, lor.getLeftOperand()) and
    zeroGuardCondition(checked, lor.getRightOperand()) and
    ret = ifs.getThen().getAChild*() and
    guardBb = ifs.getBasicBlock() and
    sinkBb = sink.getBasicBlock() and
    dominates(guardBb, sinkBb)
  )
}

predicate isDivOrModSink(Expr checked, Expr sink) {
  exists(DivExpr div |
    sink = div and
    sameValueExpr(div.getRightOperand(), checked)
  )
  or
  exists(RemExpr rem |
    sink = rem and
    sameValueExpr(rem.getRightOperand(), checked)
  )
}

from Expr source, Expr sink
where inPatchScope(sink)
  and isSamplingDerivedExpr(source)
  and isDivOrModSink(source, sink)
  and not hasPatchGuard(source, sink)
select sink, "YCbCr subsampling value derived from file metadata reaches a division or modulo operation without a dominating non-zero guard."