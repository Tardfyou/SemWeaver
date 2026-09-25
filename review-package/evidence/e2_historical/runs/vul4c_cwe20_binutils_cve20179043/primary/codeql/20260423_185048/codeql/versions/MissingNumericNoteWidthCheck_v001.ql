/**
 * @name MissingNumericNoteWidthCheck
 * @description 在 `binutils/readelf.c` 的构建属性 numeric note 解码逻辑中，定位由 `pnote->namesz - (name - pnote->namedata)` 计算出的 `bytes`，并在同一函数/补丁邻域内匹配其被用于 `while (bytes--)` 循环驱动、循环体对 `val` 进行移位/累积解码、且在该消费点之前不存在 `bytes > sizeof(val)` 之类显式宽度上界拒绝的模式。
 * @kind problem
 * @problem.severity warning
 * @precision medium
 * @id cpp/custom/unknown
 * @tags security
 *       correctness
 */

import cpp

predicate inTargetFile(Element e) {
  e.getFile().getRelativePath() = "binutils/readelf.c"
}

predicate exprContainsVariableAccess(Expr root, VariableAccess access) {
  access = root or root.getAChild*() = access
}

predicate exprContainsFieldAccess(Expr root, FieldAccess access) {
  access = root or root.getAChild*() = access
}

predicate sameValueExpr(Expr left, Expr right) {
  left = right
  or
  exists(VariableAccess l, VariableAccess r |
    exprContainsVariableAccess(left, l) and
    exprContainsVariableAccess(right, r) and
    l.getTarget() = r.getTarget()
  )
  or
  exists(FieldAccess l, FieldAccess r |
    exprContainsFieldAccess(left, l) and
    exprContainsFieldAccess(right, r) and
    l.getTarget() = r.getTarget() and
    sameValueExpr(l.getQualifier(), r.getQualifier())
  )
}

predicate isNameszDerivedBytes(Variable bytes, Expr init) {
  exists(FieldAccess namesz, VariableAccess nameVar, FieldAccess namedata |
    namesz.getTarget().getName() = "namesz" and
    namedata.getTarget().getName() = "namedata" and
    nameVar.getTarget().getName() = "name" and
    exprContainsFieldAccess(init, namesz) and
    exprContainsFieldAccess(init, namedata) and
    exprContainsVariableAccess(init, nameVar)
  )
}

predicate isNumericDecodeLoop(Stmt loopStmt, Variable bytes, Variable val) {
  exists(VariableAccess bytesUse, VariableAccess valUse, AssignExpr assign |
    bytesUse.getTarget() = bytes and
    valUse.getTarget() = val and
    loopStmt.getAChild*() = bytesUse and
    loopStmt.getAChild*() = assign and
    assign.getAChild*() = valUse and
    assign.toString().regexpMatch(".*<<.*\\|.*")
  )
}

predicate hasWidthGuard(Function f, Variable bytes, Variable val) {
  exists(IfStmt ifs, VariableAccess bytesUse, VariableAccess valUse, SizeofExprOperator sz |
    ifs.getEnclosingFunction() = f and
    exprContainsVariableAccess(ifs.getCondition(), bytesUse) and
    bytesUse.getTarget() = bytes and
    exprContainsVariableAccess(sz.getExprOperand(), valUse) and
    valUse.getTarget() = val and
    ifs.getCondition().toString().regexpMatch(".*>.*") and
    ifs.getThen().toString().regexpMatch("(?s).*return[\\t\\n\\r ]+FALSE.*")
  )
}

from Variable bytes, Variable val, VariableAccess triggerUse, Expr init, Stmt loopStmt, Function f
where
  inTargetFile(bytes) and
  bytes.getName() = "bytes" and
  val.getName() = "val" and
  bytes.getEnclosingFunction() = f and
  val.getEnclosingFunction() = f and
  triggerUse.getTarget() = bytes and
  triggerUse.getEnclosingFunction() = f and
  exists(AssignExpr assign |
    assign.getTarget() = triggerUse and
    init = assign.getRValue() and
    isNameszDerivedBytes(bytes, init)
  ) and
  isNumericDecodeLoop(loopStmt, bytes, val) and
  loopStmt.getEnclosingFunction() = f and
  not hasWidthGuard(f, bytes, val)
select triggerUse, "`bytes` derived from `namesz` drives numeric note decoding without a width check against `sizeof(val)`."