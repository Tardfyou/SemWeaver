/**
 * @name BfdDwarfBlockDataNullDeref
 * @description 在 `bfd/dwarf2.c` 的补丁作用域内，定位对嵌套字段 `attr.u.blk->data` 的直接解引用/首字节读取，并要求该解引用位于与补丁相同的分支条件上下文，且不存在同一条件中的先行 `attr.u.blk->data != NULL` 短路保护，从而稳定命中漏洞版本并在修复版本静默。
 * @kind problem
 * @problem.severity warning
 * @precision medium
 * @id cpp/custom/null_dereference
 * @tags security
 *       correctness
 */

import cpp

/**
 * Patch-local null dereference detector for bfd/dwarf2.c.
 * Triggers on dereferences of attr.u.blk->data and excludes cases
 * where the dereference is guarded by a non-null check on the same field.
 */

predicate inPatchedFile(Element e) {
  e.getFile().getRelativePath().regexpMatch(".*/bfd/dwarf2\\.c")
  or
  e.getFile().getRelativePath() = "bfd/dwarf2.c"
}

predicate sameVariable(Expr a, Expr b) {
  exists(VariableAccess va, VariableAccess vb |
    (va = a or a.getAChild*() = va) and
    (vb = b or b.getAChild*() = vb) and
    va.getTarget() = vb.getTarget()
  )
}

predicate sameFieldChain(Expr a, Expr b) {
  a = b
  or
  exists(FieldAccess fa, FieldAccess fb |
    fa = a and
    fb = b and
    fa.getTarget() = fb.getTarget() and
    sameFieldChain(fa.getQualifier(), fb.getQualifier())
  )
  or
  exists(PointerFieldAccess pa, PointerFieldAccess pb |
    pa = a and
    pb = b and
    pa.getTarget() = pb.getTarget() and
    sameFieldChain(pa.getQualifier(), pb.getQualifier())
  )
  or
  sameVariable(a, b)
}

predicate isBlkDataExpr(Expr e) {
  exists(PointerFieldAccess dataField, PointerFieldAccess blkField, FieldAccess uField, VariableAccess attrVar |
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

predicate isPatchLocalTrigger(Expr deref) {
  exists(UnaryOperation u, Expr operand |
    u = deref and
    u.getOperator() = "*" and
    operand = u.getOperand() and
    isBlkDataExpr(operand)
  )
}

predicate nullCheckMatches(Expr checked, Expr derefTarget) {
  sameFieldChain(checked, derefTarget)
}

predicate hasNonNullGuard(Expr deref) {
  exists(IfStmt ifs, BinaryOperation cmp, Expr checked, Expr derefTarget |
    deref.getEnclosingStmt() = ifs.getThen() and
    cmp = ifs.getCondition() and
    (
      cmp.getOperator() = "!="
      or
      cmp.getOperator() = "=="
    ) and
    checked = cmp.getAnOperand() and
    isBlkDataExpr(checked) and
    exists(UnaryOperation u |
      u = deref and
      derefTarget = u.getOperand() and
      nullCheckMatches(checked, derefTarget)
    ) and
    exists(Expr other |
      other = cmp.getAnOperand() and
      other != checked and
      other instanceof ZeroLiteral
    ) and
    cmp.getOperator() = "!="
  )
}

from Expr deref
where inPatchedFile(deref)
  and isPatchLocalTrigger(deref)
  and not hasNonNullGuard(deref)
select deref, "Dereference of attr.u.blk->data without a local non-null guard."