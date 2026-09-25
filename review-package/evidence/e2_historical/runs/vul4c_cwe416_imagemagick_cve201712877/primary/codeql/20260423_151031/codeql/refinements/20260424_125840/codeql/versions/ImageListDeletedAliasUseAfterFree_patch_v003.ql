/**
 * @name ImageListDeletedAliasUseAfterFree
 * @description 检测链表节点删除后，仍保留指向被删节点的别名并在后续继续解引用/作为有效图像参与操作的模式；补丁通过在别名与被删节点相等时显式失效该别名来阻断后续使用。
 * @kind problem
 * @problem.severity warning
 * @precision medium
 * @id cpp/custom/use_after_free
 * @tags security
 *       correctness
 */

import cpp

predicate inPatchScope(Element e) {
  e.getFile().getRelativePath().matches("%/coders/mat.c") and
  exists(Function f | e.getEnclosingElement*() = f and f.getName() = "ReadMATImage")
}

predicate exprContainsVariableAccess(Expr root, VariableAccess access) {
  access = root or root.getAChild*() = access
}

predicate isDeleteImageFromListCall(FunctionCall call) {
  exists(Function f |
    call.getTarget() = f and
    f.getName() = "DeleteImageFromList"
  )
}

predicate isAddressOfVariable(Expr e, Variable v) {
  exists(AddressOfExpr addr, VariableAccess va |
    e = addr and
    va = addr.getOperand() and
    va.getTarget() = v
  )
}

predicate aliasAssignedFromDeletedValue(Variable aliasVar, Variable deletedVar, AssignExpr assign) {
  exists(VariableAccess lhs, VariableAccess rhs |
    lhs = assign.getLValue() and
    rhs = assign.getRValue() and
    lhs.getTarget() = aliasVar and
    rhs.getTarget() = deletedVar
  )
}

predicate aliasMayDenoteDeletedValueBeforeCall(FunctionCall delCall, Variable aliasVar, Variable deletedVar) {
  exists(AssignExpr assign |
    aliasAssignedFromDeletedValue(aliasVar, deletedVar, assign) and
    assign.getEnclosingFunction() = delCall.getEnclosingFunction() and
    assign.getEnclosingStmt() = delCall.getEnclosingStmt().getAPredecessor*()
  )
}

predicate aliasReboundAfterDelete(FunctionCall delCall, Variable aliasVar, Expr use) {
  exists(AssignExpr assign, VariableAccess lhs |
    assign.getEnclosingFunction() = delCall.getEnclosingFunction() and
    lhs = assign.getLValue() and
    lhs.getTarget() = aliasVar and
    assign.getEnclosingStmt() = delCall.getEnclosingStmt().getASuccessor*() and
    use.getEnclosingStmt() = assign.getEnclosingStmt().getASuccessor*()
  )
}

predicate aliasInvalidatedOnDelete(FunctionCall delCall, Variable aliasVar, Variable deletedVar) {
  exists(IfStmt guard, EqualityOperation eq, VariableAccess deletedAccess, VariableAccess aliasAccess,
    AssignExpr clear, VariableAccess clearLhs, Expr clearedValue |
    guard.getEnclosingFunction() = delCall.getEnclosingFunction() and
    guard.getCondition() = eq and
    (
      deletedAccess = eq.getAnOperand() and
      aliasAccess = eq.getAnOperand() and
      deletedAccess != aliasAccess
    ) and
    deletedAccess.getTarget() = deletedVar and
    aliasAccess.getTarget() = aliasVar and
    clear = guard.getThen().getAChild*() and
    clearLhs = clear.getLValue() and
    clearLhs.getTarget() = aliasVar and
    clearedValue = clear.getRValue() and
    clearedValue.toString() = "(0)" and
    guard.getEnclosingStmt() = delCall.getEnclosingStmt().getAPredecessor*()
  )
}

predicate isDangerousAliasUse(Expr use, Variable aliasVar) {
  exists(VariableAccess base |
    exprContainsVariableAccess(use, base) and
    base.getTarget() = aliasVar and
    (
      exists(FieldAccess fa |
        use = fa and
        fa.getQualifier() = base
      )
      or
      exists(PointerFieldAccess pfa |
        use = pfa and
        pfa.getQualifier() = base
      )
      or
      exists(FunctionCall call, int i |
        use = call and
        i >= 0 and
        i < call.getNumberOfArguments() and
        exprContainsVariableAccess(call.getArgument(i), base)
      )
    )
  )
}

predicate acceptedDeletedAliasUse(FunctionCall delCall, Variable aliasVar, Variable deletedVar, Expr use) {
  exists(VariableAccess argAccess |
    exprContainsVariableAccess(delCall.getArgument(0), argAccess) and
    argAccess.getTarget() = deletedVar
  ) and
  aliasMayDenoteDeletedValueBeforeCall(delCall, aliasVar, deletedVar) and
  isDangerousAliasUse(use, aliasVar) and
  use.getEnclosingFunction() = delCall.getEnclosingFunction() and
  use.getEnclosingStmt() = delCall.getEnclosingStmt().getASuccessor*() and
  not aliasReboundAfterDelete(delCall, aliasVar, use) and
  not aliasInvalidatedOnDelete(delCall, aliasVar, deletedVar)
}

from FunctionCall delCall, Variable aliasVar, Variable deletedVar, Expr use
where
  inPatchScope(delCall) and
  isDeleteImageFromListCall(delCall) and
  exists(Variable v | isAddressOfVariable(delCall.getArgument(0), v) and v = deletedVar) and
  acceptedDeletedAliasUse(delCall, aliasVar, deletedVar, use) and
  inPatchScope(use)
select use, "An alias to a list element remains in use after that element is deleted from the list, suggesting a use-after-free unless the alias is invalidated or rebound first."
