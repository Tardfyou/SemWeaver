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

predicate isFailClosedTerminator(Stmt s) {
  s instanceof ReturnStmt or
  exists(FunctionCall c |
    c.getEnclosingStmt() = s and
    c.getTarget().getName() = "ThrowReaderException"
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

predicate isZeroLiteral(Expr e) {
  e instanceof Literal and
  e.toString() = "0"
}

predicate isThreeLiteral(Expr e) {
  e instanceof Literal and
  e.toString() = "3"
}

predicate isDimensionEqualsZero(Expr e, Variable v) {
  exists(EqualityOperation eq, FieldAccess fa, Expr other |
    eq = e and
    fa = eq.getAnOperand() and
    dimensionAccessOnVar(fa, v) and
    other = eq.getAnOperand() and
    other != fa and
    isZeroLiteral(other)
  )
}

predicate isDimensionGreaterThanThree(Expr e, Variable v) {
  exists(GTExpr gt, FieldAccess fa, Expr rhs |
    gt = e and
    fa = gt.getLeftOperand() and
    dimensionAccessOnVar(fa, v) and
    rhs = gt.getRightOperand() and
    isThreeLiteral(rhs)
  )
}

predicate isInvalidDimensionCheck(Expr cond, Variable v) {
  isDimensionEqualsZero(cond, v)
  or
  isDimensionGreaterThanThree(cond, v)
  or
  exists(OrExpr orExpr |
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

predicate hasPatchGuard(AssignExpr assign, Variable v) {
  exists(IfStmt ifs, Stmt guarded, Stmt term |
    ifs.getEnclosingFunction() = assign.getEnclosingFunction() and
    bbDominates(assign.getBasicBlock(), ifs.getBasicBlock()) and
    guarded = ifs.getThen() and
    term = guarded.getAChild*() and
    isFailClosedTerminator(term) and
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