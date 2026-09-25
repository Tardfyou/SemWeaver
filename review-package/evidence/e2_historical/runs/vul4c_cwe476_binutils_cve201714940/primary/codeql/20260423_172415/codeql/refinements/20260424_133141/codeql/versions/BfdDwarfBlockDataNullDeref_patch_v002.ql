/**
 * @name BfdDwarfBlockDataNullDeref
 * @description 在 `bfd/dwarf2.c` 的补丁作用域内，定位 `if` 条件中对 `attr.u.blk->data` 的直接解引用；该形态对应先解引用 DWARF block 数据再判定操作码，并排除修复版本的非空保护条件。
 * @kind problem
 * @problem.severity warning
 * @precision medium
 * @id cpp/custom/null_dereference
 * @tags security
 *       correctness
 */

import cpp

predicate inPatchedFile(Element e) {
  e.getFile().getRelativePath().regexpMatch("(^|.*/)bfd/dwarf2\\.c$")
}

predicate isAttrBlkDataExpr(Expr e) {
  exists(PointerFieldAccess dataField, FieldAccess blkField, FieldAccess uField, VariableAccess attrVar |
    dataField = e and
    dataField.getTarget().hasName("data") and
    blkField = dataField.getQualifier() and
    blkField.getTarget().hasName("blk") and
    uField = blkField.getQualifier() and
    uField.getTarget().hasName("u") and
    attrVar = uField.getQualifier() and
    attrVar.getTarget().hasName("attr")
  )
}

predicate isNonNullGuardForAttrBlkData(Expr e) {
  exists(BinaryOperation cmp, Expr lhs, Expr rhs |
    cmp = e and
    cmp.getOperator() = "!=" and
    lhs = cmp.getLeftOperand() and
    rhs = cmp.getRightOperand() and
    isAttrBlkDataExpr(lhs) and
    rhs instanceof NullPointerConstant
  )
  or
  exists(BinaryOperation cmp, Expr lhs, Expr rhs |
    cmp = e and
    cmp.getOperator() = "!=" and
    lhs = cmp.getLeftOperand() and
    rhs = cmp.getRightOperand() and
    lhs instanceof NullPointerConstant and
    isAttrBlkDataExpr(rhs)
  )
}

predicate conditionHasNonNullGuard(Expr cond) {
  exists(Expr guard |
    guard = cond.getAChild*() and
    isNonNullGuardForAttrBlkData(guard)
  )
}

predicate hasVulnerableShape(IfStmt ifs, UnaryOperation deref) {
  exists(Expr cond, Expr operand |
    cond = ifs.getCondition() and
    deref = cond.getAChild*() and
    deref.getOperator() = "*" and
    operand = deref.getOperand() and
    isAttrBlkDataExpr(operand) and
    not conditionHasNonNullGuard(cond)
  )
}

from IfStmt ifs, UnaryOperation deref
where
  inPatchedFile(ifs) and
  hasVulnerableShape(ifs, deref)
select deref, "Dereference of attr.u.blk->data without an in-condition non-null guard."
