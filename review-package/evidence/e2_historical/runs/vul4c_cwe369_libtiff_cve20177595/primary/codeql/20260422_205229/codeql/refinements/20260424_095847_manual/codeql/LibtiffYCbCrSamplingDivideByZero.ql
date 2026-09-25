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

predicate isSamplingStateField(Expr e) {
  exists(FieldAccess fa |
    fa = e and
    (
      fa.getTarget().hasName("h_sampling")
      or
      fa.getTarget().hasName("v_sampling")
    )
  )
}

predicate sameSamplingState(Expr left, Expr right) {
  sameValueExpr(left, right)
  or
  exists(AssignExpr assign |
    inPatchScope(assign) and
    sameValueExpr(assign.getLValue(), left) and
    sameValueExpr(assign.getRValue(), right)
  )
  or
  exists(AssignExpr assign |
    inPatchScope(assign) and
    sameValueExpr(assign.getRValue(), left) and
    sameValueExpr(assign.getLValue(), right)
  )
}

predicate samplingStateExpr(Expr e) {
  exists(Expr rhs |
    directSamplingAssignmentTarget(e, rhs)
  )
  or
  exists(Expr state |
    isSamplingStateField(state) and
    sameSamplingState(e, state) and
    exists(Expr rhs |
      directSamplingAssignmentTarget(state, rhs)
    )
  )
}

predicate isSamplingDerivedExpr(Expr e) {
  samplingStateExpr(e)
}

predicate zeroCheckExpr(Expr checked, Expr cond) {
  exists(EqualityOperation eq |
    eq = cond and
    (
      sameSamplingState(eq.getLeftOperand(), checked) and isZeroExpr(eq.getRightOperand())
      or
      sameSamplingState(eq.getRightOperand(), checked) and isZeroExpr(eq.getLeftOperand())
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

predicate isNamedSamplingField(Expr e, string fieldName) {
  exists(FieldAccess fa |
    fa = e and
    fa.getTarget().hasName(fieldName)
  )
}

predicate matchesSamplingIndexRead(Expr rhs, string fieldName) {
  exists(ArrayExpr arr, FieldAccess fa |
    rhs = arr and
    arr.getArrayBase() = fa and
    fa.getTarget().hasName("td_ycbcrsubsampling") and
    (
      fieldName = "h_sampling" and isZeroExpr(arr.getArrayOffset())
      or
      fieldName = "v_sampling" and isOneExpr(arr.getArrayOffset())
    )
  )
}

predicate namedFieldZeroCheck(Expr cond, string fieldName) {
  exists(EqualityOperation eq |
    eq = cond and
    (
      isNamedSamplingField(eq.getLeftOperand(), fieldName) and isZeroExpr(eq.getRightOperand())
      or
      isNamedSamplingField(eq.getRightOperand(), fieldName) and isZeroExpr(eq.getLeftOperand())
    )
  )
  or
  exists(LogicalOrExpr lor |
    lor = cond and
    (
      namedFieldZeroCheck(lor.getLeftOperand(), fieldName)
      or
      namedFieldZeroCheck(lor.getRightOperand(), fieldName)
    )
  )
}

predicate hasLocalPatchGuard(Expr checked, Expr sink) {
  exists(IfStmt ifs, ReturnStmt ret, BasicBlock guardBb, BasicBlock sinkBb |
    inPatchScope(ifs) and
    ret = ifs.getThen().getAChild*() and
    zeroGuardCondition(checked, ifs.getCondition()) and
    guardBb = ifs.getBasicBlock() and
    sinkBb = sink.getBasicBlock() and
    dominates(guardBb, sinkBb)
  )
  or
  exists(Expr peer, IfStmt ifs, ReturnStmt ret, LogicalOrExpr lor, BasicBlock guardBb, BasicBlock sinkBb |
    inPatchScope(ifs) and
    checked instanceof FieldAccess and
    checked.(FieldAccess).getTarget().hasName("h_sampling") and
    isSamplingStateField(peer) and
    peer.(FieldAccess).getTarget().hasName("v_sampling") and
    lor = ifs.getCondition() and
    zeroGuardCondition(checked, lor.getLeftOperand()) and
    zeroGuardCondition(peer, lor.getRightOperand()) and
    ret = ifs.getThen().getAChild*() and
    guardBb = ifs.getBasicBlock() and
    sinkBb = sink.getBasicBlock() and
    dominates(guardBb, sinkBb)
  )
  or
  exists(Expr peer, IfStmt ifs, ReturnStmt ret, LogicalOrExpr lor, BasicBlock guardBb, BasicBlock sinkBb |
    inPatchScope(ifs) and
    checked instanceof FieldAccess and
    checked.(FieldAccess).getTarget().hasName("v_sampling") and
    isSamplingStateField(peer) and
    peer.(FieldAccess).getTarget().hasName("h_sampling") and
    lor = ifs.getCondition() and
    zeroGuardCondition(peer, lor.getLeftOperand()) and
    zeroGuardCondition(checked, lor.getRightOperand()) and
    ret = ifs.getThen().getAChild*() and
    guardBb = ifs.getBasicBlock() and
    sinkBb = sink.getBasicBlock() and
    dominates(guardBb, sinkBb)
  )
}

predicate hasValidatedSamplingInitialization(Expr checked) {
  exists(Function fn, AssignExpr assignH, AssignExpr assignV, IfStmt ifs, ReturnStmt ret |
    inPatchScope(fn) and
    checked instanceof FieldAccess and
    (
      checked.(FieldAccess).getTarget().hasName("h_sampling")
      or
      checked.(FieldAccess).getTarget().hasName("v_sampling")
    ) and
    assignH.getEnclosingFunction() = fn and
    isNamedSamplingField(assignH.getLValue(), "h_sampling") and
    matchesSamplingIndexRead(assignH.getRValue(), "h_sampling") and
    assignV.getEnclosingFunction() = fn and
    isNamedSamplingField(assignV.getLValue(), "v_sampling") and
    matchesSamplingIndexRead(assignV.getRValue(), "v_sampling") and
    ifs.getEnclosingFunction() = fn and
    namedFieldZeroCheck(ifs.getCondition(), "h_sampling") and
    namedFieldZeroCheck(ifs.getCondition(), "v_sampling") and
    ret = ifs.getThen().getAChild*() and
    assignH.getLocation().getStartLine() < ifs.getLocation().getStartLine() and
    assignV.getLocation().getStartLine() < ifs.getLocation().getStartLine()
  )
}

predicate hasPatchSamplingBarrier() {
  exists(IfStmt ifs, ReturnStmt ret |
    inPatchScope(ifs) and
    namedFieldZeroCheck(ifs.getCondition(), "h_sampling") and
    namedFieldZeroCheck(ifs.getCondition(), "v_sampling") and
    ret = ifs.getThen().getAChild*()
  )
}

predicate hasPatchGuard(Expr checked, Expr sink) {
  hasLocalPatchGuard(checked, sink)
  or
  hasValidatedSamplingInitialization(checked)
  or
  hasPatchSamplingBarrier()
}

predicate isDivOrModSink(Expr checked, Expr sink) {
  exists(DivExpr div |
    sink = div and
    sameSamplingState(div.getRightOperand(), checked)
  )
  or
  exists(RemExpr rem |
    sink = rem and
    sameSamplingState(rem.getRightOperand(), checked)
  )
}

from Expr source, Expr sink
where inPatchScope(sink)
  and isSamplingDerivedExpr(source)
  and isDivOrModSink(source, sink)
  and not hasPatchGuard(source, sink)
select sink, "YCbCr subsampling value derived from file metadata reaches a division or modulo operation without a dominating non-zero guard."
