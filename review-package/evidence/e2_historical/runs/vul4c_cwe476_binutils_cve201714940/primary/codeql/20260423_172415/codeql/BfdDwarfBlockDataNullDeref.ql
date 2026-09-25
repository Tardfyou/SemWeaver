/**
 * @name BfdDwarfBlockDataNullDeref
 * @description 在 `bfd/dwarf2.c` 的补丁作用域内，定位 `if` 条件起始行上对 `attr.u.blk->data` 的直接解引用；该形态对应漏洞版本单行条件 `*attr.u.blk->data == ...`，并排除修复版本的 `&&` 保护条件。
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

predicate hasVulnerableShape(IfStmt ifs, UnaryOperation deref) {
  exists(Expr cond, Expr operand |
    cond = ifs.getCondition() and
    deref.getEnclosingStmt() = ifs and
    deref.getOperator() = "*" and
    operand = deref.getOperand() and
    isAttrBlkDataExpr(operand) and
    // Vulnerable version keeps the dereference on the condition's first line.
    deref.getLocation().getStartLine() = cond.getLocation().getStartLine() and
    // Fixed version introduces a short-circuit guard and prints as "... && ...".
    not cond.toString().regexpMatch(".*&&.*")
  )
}

from IfStmt ifs, UnaryOperation deref
where
  inPatchedFile(ifs) and
  hasVulnerableShape(ifs, deref)
select deref, "Dereference of attr.u.blk->data without an in-condition non-null guard."
