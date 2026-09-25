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

predicate readsIntoDimension(AssignExpr assign, Variable v) {
  isDimensionRead(assign.getRValue()) and
  exists(FieldAccess fa |
    fa = assign.getLValue() and
    dimensionAccessOnVar(fa, v)
  )
}

predicate exprUsesDimensionValue(Expr e, Variable v) {
  exists(FieldAccess fa |
    fa = e.getAChild*() and
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

predicate isDimensionLessThanOne(Expr e, Variable v) {
  exists(RelationalOperation rel |
    rel = e and
    (
      rel instanceof LTExpr or
      rel instanceof LEExpr
    ) and
    (
      isDimensionAccess(rel.getLeftOperand(), v) and
      isZeroExpr(rel.getRightOperand())
      or
      isDimensionAccess(rel.getRightOperand(), v) and
      isZeroExpr(rel.getLeftOperand())
    )
  )
  or
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
  exists(RelationalOperation rel |
    rel = e and
    (
      rel instanceof GTExpr or
      rel instanceof GEExpr
    ) and
    (
      isDimensionAccess(rel.getLeftOperand(), v) and
      isThreeExpr(rel.getRightOperand())
      or
      isDimensionAccess(rel.getRightOperand(), v) and
      isThreeExpr(rel.getLeftOperand())
    )
  )
}

predicate isInvalidDimensionCheck(Expr cond, Variable v) {
  isDimensionLessThanOne(cond, v)
  or
  isDimensionGreaterThanThree(cond, v)
  or
  exists(LogicalOrExpr orExpr |
    orExpr = cond and
    (
      isDimensionLessThanOne(orExpr.getLeftOperand(), v) and
      isDimensionGreaterThanThree(orExpr.getRightOperand(), v)
      or
      isDimensionGreaterThanThree(orExpr.getLeftOperand(), v) and
      isDimensionLessThanOne(orExpr.getRightOperand(), v)
    )
  )
}

predicate guardFailsClosed(IfStmt ifs) {
  exists(Stmt guarded, Stmt branch |
    guarded = ifs.getThen() and
    branch = guarded.getAChild*() and
    (
      branch instanceof ReturnStmt
      or
      exists(FunctionCall c |
        c = branch and
        c.getTarget().getName() = "ThrowReaderException"
      )
    )
  )
  or
  exists(Stmt guarded, Stmt branch |
    guarded = ifs.getElse() and
    branch = guarded.getAChild*() and
    (
      branch instanceof ReturnStmt
      or
      exists(FunctionCall c |
        c = branch and
        c.getTarget().getName() = "ThrowReaderException"
      )
    )
  )
}

predicate guardRejectsInvalidDimension(IfStmt ifs, Variable v) {
  guardFailsClosed(ifs) and
  (
    isInvalidDimensionCheck(ifs.getCondition(), v)
    or
    exists(UnaryNotExpr notExpr |
      notExpr = ifs.getCondition() and
      exists(Expr negated |
        negated = notExpr.getOperand() and
        (
          isDimensionLessThanOne(negated, v)
          or
          isDimensionGreaterThanThree(negated, v)
        )
      )
    )
  )
}

predicate hasDimensionRangeGuard(AssignExpr assign, Variable v, IfStmt ifs) {
  ifs.getEnclosingFunction() = assign.getEnclosingFunction() and
  bbDominates(assign.getBasicBlock(), ifs.getBasicBlock()) and
  guardRejectsInvalidDimension(ifs, v)
}

predicate dimensionInvalidatesChannelLayout(Expr e, Variable v) {
  exists(BinaryOperation op |
    op = e.getAChild*() and
    (
      op instanceof MulExpr or
      op instanceof AddExpr or
      op instanceof SubExpr or
      op instanceof DivExpr
    ) and
    exprUsesDimensionValue(op, v)
  )
  or
  exists(ArrayExpr arr |
    arr = e.getAChild*() and
    exprUsesDimensionValue(arr.getArrayOffset(), v)
  )
  or
  exists(FunctionCall call |
    call = e.getAChild*() and
    exprUsesDimensionValue(call, v)
  )
}

predicate acceptedWithoutGuard(AssignExpr assign, Variable v, Expr anchor) {
  bbDominates(assign.getBasicBlock(), anchor.getBasicBlock()) and
  dimensionInvalidatesChannelLayout(anchor, v) and
  not exists(IfStmt ifs |
    hasDimensionRangeGuard(assign, v, ifs) and
    bbDominates(ifs.getBasicBlock(), anchor.getBasicBlock())
  )
}

from AssignExpr assign, Variable v, Expr anchor
where
  inPatchScope(assign.getEnclosingFunction()) and
  readsIntoDimension(assign, v) and
  acceptedWithoutGuard(assign, v, anchor)
select anchor, "User-controlled SGI header field 'dimension' influences later layout or indexing logic without a fail-closed guard enforcing the valid range 1..3."