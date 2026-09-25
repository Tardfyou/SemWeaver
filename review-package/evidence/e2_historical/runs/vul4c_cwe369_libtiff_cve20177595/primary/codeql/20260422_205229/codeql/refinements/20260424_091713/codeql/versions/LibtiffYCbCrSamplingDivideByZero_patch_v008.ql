/**
 * @name LibtiffYCbCrSamplingDivideByZero
 * @description 检测在 libtiff 的 YCbCr/JPEG 设置路径中，来自 `td->td_ycbcrsubsampling` 的水平/垂直采样值写入 JPEG 状态后，若缺少对任一采样值为零的支配性提前返回校验，就继续进入以该状态作为除数的使用点的缺陷模式；若存在补丁同类 guard，则应静默。
 * @kind problem
 * @problem.severity warning
 * @precision medium
 * @id cpp/custom/unknown
 * @tags security
 *       correctness
 */

import cpp
import semmle.code.cpp.controlflow.Dominance

predicate inPatchScope(Element e) {
  e.getFile().getRelativePath().matches("%tif_jpeg.c")
}

predicate isSubsamplingIndex(Expr idx) {
  exists(IntLiteral i |
    i = idx and
    (i.getValue().toInt() = 0 or i.getValue().toInt() = 1)
  )
}

predicate isSamplingMetadataRead(Expr e) {
  exists(ArrayExpr arr, FieldAccess base |
    e = arr and
    arr.getArrayBase() = base and
    base.getTarget().hasName("td_ycbcrsubsampling") and
    isSubsamplingIndex(arr.getArrayOffset())
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

predicate isSamplingStateField(Field f) {
  f.hasName("h_sampling") or f.hasName("v_sampling")
}

predicate stateFieldAssignedFromMetadata(Field f) {
  exists(AssignExpr assign, FieldAccess lhs |
    inPatchScope(assign) and
    lhs = assign.getLValue() and
    lhs.getTarget() = f and
    isSamplingStateField(f) and
    isSamplingMetadataRead(assign.getRValue())
  )
}

predicate isSamplingStateRead(Expr e) {
  exists(FieldAccess fa |
    fa = e and
    stateFieldAssignedFromMetadata(fa.getTarget())
  )
}

predicate sameStateFieldRead(Expr e1, Expr e2) {
  exists(FieldAccess fa1, FieldAccess fa2 |
    fa1 = e1 and
    fa2 = e2 and
    fa1.getTarget() = fa2.getTarget() and
    sameValueExpr(fa1.getQualifier(), fa2.getQualifier())
  )
}

predicate isZeroLiteral(Expr e) {
  exists(IntLiteral i |
    i = e and i.getValue().toInt() = 0
  )
}

predicate zeroCheckExpr(Expr checked, Expr cond) {
  exists(EqualityOperation eq |
    eq = cond and
    (
      sameValueExpr(eq.getLeftOperand(), checked) and isZeroLiteral(eq.getRightOperand())
      or
      sameValueExpr(eq.getRightOperand(), checked) and isZeroLiteral(eq.getLeftOperand())
    )
  )
}

predicate zeroBranchReturns(IfStmt ifs) {
  exists(ReturnStmt ret |
    ret = ifs.getThen().getAChild*()
  )
}

predicate guardChecksStateReadZero(IfStmt ifs, Expr checked) {
  isSamplingStateRead(checked) and
  (
    zeroCheckExpr(checked, ifs.getCondition())
    or
    exists(LogicalOrExpr lor |
      lor = ifs.getCondition() and
      (
        zeroCheckExpr(checked, lor.getLeftOperand()) or
        zeroCheckExpr(checked, lor.getRightOperand())
      )
    )
  )
}

predicate hasPatchGuard(Expr sink) {
  exists(IfStmt ifs, Expr checked, BasicBlock guardBb, BasicBlock sinkBb |
    inPatchScope(ifs) and
    guardChecksStateReadZero(ifs, checked) and
    sampledStateUsedByDivision(sink, checked) and
    zeroBranchReturns(ifs) and
    guardBb = ifs.getBasicBlock() and
    sinkBb = sink.getBasicBlock() and
    dominates(guardBb, sinkBb)
  )
}

predicate sampledStateUsedByDivision(Expr sink, Expr checked) {
  exists(DivExpr div |
    sink = div and
    sameStateFieldRead(div.getRightOperand(), checked)
  )
}

from Expr sink
where inPatchScope(sink)
  and exists(Expr checked | sampledStateUsedByDivision(sink, checked))
  and exists(Field f | stateFieldAssignedFromMetadata(f))
  and not hasPatchGuard(sink)
select sink, "YCbCr subsampling values from file metadata are stored into JPEG sampling state and reach a division that uses the sampling value as divisor without a dominating early return on zero sampling values."