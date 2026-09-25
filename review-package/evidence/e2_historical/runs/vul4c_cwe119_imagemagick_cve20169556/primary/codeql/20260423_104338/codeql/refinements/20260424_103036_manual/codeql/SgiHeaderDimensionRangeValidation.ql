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

predicate isZeroExpr(Expr e) {
  exists(Literal lit |
    lit = e and
    lit.getValue().toInt() = 0
  )
}

predicate isThreeExpr(Expr e) {
  exists(Literal lit |
    lit = e and
    lit.getValue().toInt() = 3
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

predicate dimensionFieldAccess(Expr e, Expr base) {
  exists(FieldAccess fa |
    fa = e and
    fa.getTarget().hasName("dimension") and
    sameValueExpr(fa.getQualifier(), base)
  )
}

predicate readsIntoDimension(AssignExpr assign, Expr base) {
  isDimensionRead(assign.getRValue()) and
  exists(FieldAccess fa |
    fa = assign.getLValue() and
    fa.getTarget().hasName("dimension") and
    base = fa.getQualifier()
  )
}

predicate isDimensionDependentHeaderField(Field f) {
  f.hasName("columns")
  or
  f.hasName("rows")
  or
  f.hasName("depth")
}

predicate followupHeaderFieldAccess(Expr e, Expr base) {
  exists(FieldAccess fa |
    fa = e and
    sameValueExpr(fa.getQualifier(), base) and
    isDimensionDependentHeaderField(fa.getTarget())
  )
}

predicate isFollowupHeaderRead(AssignExpr anchor, Expr base) {
  isDimensionRead(anchor.getRValue()) and
  followupHeaderFieldAccess(anchor.getLValue(), base)
}

predicate isDimensionZeroCheck(Expr e, Expr base) {
  exists(RelationalOperation rel |
    rel = e and
    (
      rel instanceof LTExpr or
      rel instanceof LEExpr
    ) and
    (
      dimensionFieldAccess(rel.getLeftOperand(), base) and
      isZeroExpr(rel.getRightOperand())
      or
      dimensionFieldAccess(rel.getRightOperand(), base) and
      isZeroExpr(rel.getLeftOperand())
    )
  )
  or
  exists(EqualityOperation eq |
    eq = e and
    (
      dimensionFieldAccess(eq.getLeftOperand(), base) and
      isZeroExpr(eq.getRightOperand())
      or
      dimensionFieldAccess(eq.getRightOperand(), base) and
      isZeroExpr(eq.getLeftOperand())
    )
  )
}

predicate isDimensionTooLargeCheck(Expr e, Expr base) {
  exists(RelationalOperation rel |
    rel = e and
    (
      rel instanceof GTExpr or
      rel instanceof GEExpr
    ) and
    (
      dimensionFieldAccess(rel.getLeftOperand(), base) and
      isThreeExpr(rel.getRightOperand())
      or
      dimensionFieldAccess(rel.getRightOperand(), base) and
      isThreeExpr(rel.getLeftOperand())
    )
  )
}

predicate isInvalidDimensionCheck(Expr cond, Expr base) {
  isDimensionZeroCheck(cond, base)
  or
  isDimensionTooLargeCheck(cond, base)
  or
  exists(LogicalOrExpr orExpr |
    orExpr = cond and
    (
      isDimensionZeroCheck(orExpr.getLeftOperand(), base) and
      isDimensionTooLargeCheck(orExpr.getRightOperand(), base)
      or
      isDimensionTooLargeCheck(orExpr.getLeftOperand(), base) and
      isDimensionZeroCheck(orExpr.getRightOperand(), base)
    )
  )
}

predicate isPatchStyleRangeBarrier(IfStmt ifs, Expr base) {
  isInvalidDimensionCheck(ifs.getCondition(), base) and
  guardFailsClosed(ifs)
}

predicate guardFailsClosed(IfStmt ifs) {
  exists(Stmt branch |
    branch = ifs.getThen().getAChild*() and
    (
      branch instanceof ReturnStmt
      or
      exists(FunctionCall c |
        c.getEnclosingStmt() = branch and
        c.getTarget().getName() = "ThrowReaderException"
      )
    )
  )
  or
  exists(Stmt branch |
    branch = ifs.getElse().getAChild*() and
    (
      branch instanceof ReturnStmt
      or
      exists(FunctionCall c |
        c.getEnclosingStmt() = branch and
        c.getTarget().getName() = "ThrowReaderException"
      )
    )
  )
}

predicate hasDimensionRangeGuard(AssignExpr assign, Expr base, AssignExpr anchor, IfStmt ifs) {
  ifs.getEnclosingFunction() = assign.getEnclosingFunction() and
  bbDominates(assign.getBasicBlock(), ifs.getBasicBlock()) and
  bbDominates(ifs.getBasicBlock(), anchor.getBasicBlock()) and
  isPatchStyleRangeBarrier(ifs, base)
}

predicate acceptedWithoutGuard(AssignExpr assign, Expr base, AssignExpr anchor) {
  bbDominates(assign.getBasicBlock(), anchor.getBasicBlock()) and
  isFollowupHeaderRead(anchor, base) and
  not exists(IfStmt ifs |
    hasDimensionRangeGuard(assign, base, anchor, ifs)
  )
}

from AssignExpr assign, Expr base, AssignExpr anchor
where
  inPatchScope(assign.getEnclosingFunction()) and
  readsIntoDimension(assign, base) and
  acceptedWithoutGuard(assign, base, anchor)
select anchor, "SGI header field 'dimension' is accepted into parser state and later followed by more header reads before a fail-closed guard enforces the valid range 1..3."
