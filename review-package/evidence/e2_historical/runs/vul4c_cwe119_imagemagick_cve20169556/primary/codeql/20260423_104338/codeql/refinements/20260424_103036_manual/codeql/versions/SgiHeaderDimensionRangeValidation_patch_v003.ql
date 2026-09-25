/**
 * @name SgiHeaderDimensionRangeValidation
 * @description 检测在 SGI 图像头解析过程中，将读取到的 dimension 字段写入状态后，若未经过 1..3 范围约束且不存在导致异常退出的 guard/barrier 就继续接受并参与后续解析的缺陷模式。
 * @kind problem
 * @problem.severity warning
 * @precision medium
 * @id cpp/custom/buffer_overflow
 * @tags security
 *       correctness
 */

import cpp
import semmle.code.cpp.controlflow.Dominance

predicate inPatchScope(Function f) {
  f.getFile().getRelativePath().matches("%coders/sgi.c") and
  f.getName() = "ReadSGIImage"
}

predicate isDimensionRead(Expr e) {
  exists(FunctionCall call |
    call = e and
    call.getTarget().getName() = "ReadBlobMSBShort"
  )
}

predicate readsIntoDimension(AssignExpr assign, Variable v) {
  isDimensionRead(assign.getRValue()) and
  exists(FieldAccess fa |
    fa = assign.getLValue() and
    fa.getTarget().getName() = "dimension" and
    exists(Expr q, VariableAccess va |
      q = fa.getQualifier() and
      va = q and
      va.getTarget() = v
    )
  )
}

predicate exprUsesDimensionValue(Expr e, Variable v) {
  exists(FieldAccess fa |
    fa = e.getAChild*() and
    fa.getTarget().getName() = "dimension" and
    exists(Expr q, VariableAccess va |
      q = fa.getQualifier() and
      va = q and
      va.getTarget() = v
    )
  )
}

predicate dimensionAccessOnVar(FieldAccess fa, Variable v) {
  fa.getTarget().getName() = "dimension" and
  exists(Expr q, VariableAccess va |
    q = fa.getQualifier() and
    va = q and
    va.getTarget() = v
  )
}

predicate isDimensionAccess(Expr e, Variable v) {
  exists(FieldAccess fa |
    fa = e and
    dimensionAccessOnVar(fa, v)
  )
}

predicate isZeroExpr(Expr e) {
  e instanceof Literal and
  e.getValue().toInt() = 0
}

predicate isThreeExpr(Expr e) {
  e instanceof Literal and
  e.getValue().toInt() = 3
}

predicate isDimensionEqualsZero(Expr e, Variable v) {
  exists(EqualityOperation eq |
    eq = e and
    (
      isDimensionAccess(eq.getLeftOperand(), v) and
      isZeroExpr(eq.getRightOperand())
      or
      isDimensionAccess(eq.getRightOperand(), v) and
      isZeroExpr(eq.getLeftOperand())
    )
  )
}

predicate isDimensionGreaterThanThree(Expr e, Variable v) {
  exists(GTExpr gt |
    gt = e and
    isDimensionAccess(gt.getLeftOperand(), v) and
    isThreeExpr(gt.getRightOperand())
  )
}

predicate isInvalidDimensionCheck(Expr cond, Variable v) {
  isDimensionEqualsZero(cond, v)
  or
  isDimensionGreaterThanThree(cond, v)
  or
  exists(LogicalOrExpr orExpr |
    orExpr = cond and
    (
      isDimensionEqualsZero(orExpr.getLeftOperand(), v) and
      isDimensionGreaterThanThree(orExpr.getRightOperand(), v)
      or
      isDimensionGreaterThanThree(orExpr.getLeftOperand(), v) and
      isDimensionEqualsZero(orExpr.getRightOperand(), v)
    )
  )
}

predicate guardFailsClosed(IfStmt ifs) {
  exists(ReturnStmt ret |
    ret = ifs.getThen().getAChild*()
  )
  or
  exists(FunctionCall c |
    c = ifs.getThen().getAChild*() and
    c.getTarget().getName() = "ThrowReaderException"
  )
}

predicate hasPatchGuard(AssignExpr assign, Variable v) {
  exists(IfStmt ifs |
    ifs.getEnclosingFunction() = assign.getEnclosingFunction() and
    bbDominates(assign.getBasicBlock(), ifs.getBasicBlock()) and
    guardFailsClosed(ifs) and
    isInvalidDimensionCheck(ifs.getCondition(), v)
  )
}

predicate acceptedWithoutGuard(AssignExpr assign, Variable v, Expr anchor) {
  bbDominates(assign.getBasicBlock(), anchor.getBasicBlock()) and
  exprUsesDimensionValue(anchor, v) and
  not hasPatchGuard(assign, v)
}

from AssignExpr assign, Variable v, Expr anchor
where
  inPatchScope(assign.getEnclosingFunction()) and
  readsIntoDimension(assign, v) and
  acceptedWithoutGuard(assign, v, anchor)
select anchor, "Structured image-header field 'dimension' is read from input and later used without a fail-closed range check restricting it to the valid SGI range 1..3."