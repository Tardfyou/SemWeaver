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

predicate isNameszDerivedBytes(Expr init) {
  exists(FieldAccess namesz, VariableAccess nameVar, FieldAccess namedata |
    namesz.getTarget().getName() = "namesz" and
    namedata.getTarget().getName() = "namedata" and
    nameVar.getTarget().getName() = "name" and
    exprContainsFieldAccess(init, namesz) and
    exprContainsFieldAccess(init, namedata) and
    exprContainsVariableAccess(init, nameVar)
  )
}

predicate exprContainsShiftByAccumulator(Expr root, Variable shift) {
  exists(VariableAccess shiftUse |
    shiftUse.getTarget() = shift and
    shiftUse = root.getAChild*()
  )
}

predicate loopConsumesBytes(Stmt loopStmt, Variable bytes) {
  exists(VariableAccess bytesUse |
    bytesUse.getTarget() = bytes and
    bytesUse = loopStmt.getAChild*()
  )
}

predicate loopAccumulatesShiftedValue(Stmt loopStmt, Variable val, Variable shift) {
  exists(AssignExpr assign, VariableAccess valUse |
    assign = loopStmt.getAChild*() and
    valUse = assign.getLValue().getAChild*() and
    valUse.getTarget() = val and
    exprContainsShiftByAccumulator(assign.getRValue(), shift)
  )
}

predicate loopAdvancesShiftByByteWidth(Stmt loopStmt, Variable shift) {
  exists(AssignExpr assign, VariableAccess shiftUse |
    assign = loopStmt.getAChild*() and
    shiftUse = assign.getLValue().getAChild*() and
    shiftUse.getTarget() = shift
  )
}

predicate isNumericDecodeLoop(Stmt loopStmt, Variable bytes, Variable val, Variable shift) {
  loopConsumesBytes(loopStmt, bytes) and
  loopAccumulatesShiftedValue(loopStmt, val, shift) and
  loopAdvancesShiftByByteWidth(loopStmt, shift)
}

predicate bytesInitializedFromRemainingName(Variable bytes, Expr init) {
  init = bytes.getInitializer() and
  isNameszDerivedBytes(init)
  or
  exists(AssignExpr assign, VariableAccess lhs |
    lhs.getTarget() = bytes and
    assign.getLValue() = lhs and
    init = assign.getRValue() and
    isNameszDerivedBytes(init)
  )
}

predicate returnsFrom(Stmt s) {
  exists(ReturnStmt ret |
    ret = s.getAChild*()
  )
}

predicate comparesBytesAgainstValueWidth(Expr condition, Variable bytes, Variable val) {
  exists(GreaterThanExpr cmp, VariableAccess bytesUse, VariableAccess valUse, SizeofExprOperator sz |
    cmp = condition.getAChild*() and
    exprContainsVariableAccess(cmp.getLeftOperand(), bytesUse) and
    bytesUse.getTarget() = bytes and
    sz = cmp.getRightOperand().getAChild*() and
    exprContainsVariableAccess(sz.getExprOperand(), valUse) and
    valUse.getTarget() = val
  )
}

predicate hasWidthGuard(Function f, Variable bytes, Variable val) {
  exists(IfStmt ifs |
    ifs.getEnclosingFunction() = f and
    comparesBytesAgainstValueWidth(ifs.getCondition(), bytes, val) and
    returnsFrom(ifs.getThen())
  )
}

from Variable bytes, Variable val, Variable shift, VariableAccess triggerUse, Expr init, Stmt loopStmt, Function f
where
  inTargetFile(triggerUse) and
  bytes.getName() = "bytes" and
  val.getName() = "val" and
  shift.getName() = "shift" and
  triggerUse.getTarget() = bytes and
  triggerUse.getEnclosingFunction() = f and
  bytesInitializedFromRemainingName(bytes, init) and
  exists(VariableAccess valUse |
    valUse.getTarget() = val and
    valUse.getEnclosingFunction() = f
  ) and
  exists(VariableAccess shiftUse |
    shiftUse.getTarget() = shift and
    shiftUse.getEnclosingFunction() = f
  ) and
  isNumericDecodeLoop(loopStmt, bytes, val, shift) and
  loopStmt.getEnclosingFunction() = f and
  triggerUse = loopStmt.getAChild*() and
  not hasWidthGuard(f, bytes, val)
select triggerUse, "`bytes` derived from remaining note name size drives numeric value decoding without a width check against `sizeof(val)`."