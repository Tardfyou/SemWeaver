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

predicate isDwarfBlockDataField(Field f) {
  f.hasName("data") and
  f.getDeclaringType().hasName("dwarf_block")
}

predicate isDwarfBlockDataExpr(Expr e) {
  exists(PointerFieldAccess dataField |
    dataField = e and
    isDwarfBlockDataField(dataField.getTarget())
  )
}

predicate isDwarfBlockDataDeref(UnaryOperation deref) {
  deref.getOperator() = "*" and
  isDwarfBlockDataExpr(deref.getOperand())
}

predicate comparesDerefToOpcode(Expr cond, UnaryOperation deref) {
  exists(BinaryOperation cmp |
    cmp = cond.getAChild*() and
    cmp.getOperator() = "==" and
    isDwarfBlockDataDeref(deref) and
    (
      cmp.getLeftOperand() = deref or
      cmp.getRightOperand() = deref
    )
  )
}

predicate isNonNullGuardForSameBlockData(Expr e, Expr dataExpr) {
  exists(BinaryOperation cmp, Expr other |
    cmp = e and
    cmp.getOperator() = "!=" and
    (
      cmp.getLeftOperand() = dataExpr and
      other = cmp.getRightOperand() or
      cmp.getRightOperand() = dataExpr and
      other = cmp.getLeftOperand()
    ) and
    other instanceof NullPointerConstant
  )
}

predicate conditionHasNonNullGuard(Expr cond, Expr dataExpr) {
  exists(Expr guard |
    guard = cond.getAChild*() and
    isNonNullGuardForSameBlockData(guard, dataExpr)
  )
}

predicate hasVulnerableShape(IfStmt ifs, UnaryOperation deref) {
  exists(Expr cond, Expr dataExpr |
    cond = ifs.getCondition() and
    dataExpr = deref.getOperand() and
    comparesDerefToOpcode(cond, deref) and
    not conditionHasNonNullGuard(cond, dataExpr)
  )
}

from IfStmt ifs, UnaryOperation deref
where
  inPatchedFile(ifs) and
  hasVulnerableShape(ifs, deref)
select deref, "Dereference of attr.u.blk->data without an in-condition non-null guard."
