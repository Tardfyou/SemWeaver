/**
 * @name ImageListDeletedAliasUseAfterFree
 * @description 检测在 C/C++ 代码中，某个对象通过容器/链表删除 API（如 DeleteImageFromList）被释放或失效后，仍存在与被删节点别名相等的指针在后续路径中被当作有效对象使用，且删除点附近缺少将别名显式置空或等效 fail-closed 屏障的模式。
 * @kind problem
 * @problem.severity warning
 * @precision medium
 * @id cpp/custom/use_after_free
 * @tags security
 *       correctness
 */

import cpp

predicate inPatchScope(Element e) {
  (e.getFile().getRelativePath() = "coders/mat.c" or
   e.getFile().getRelativePath().matches("%/coders/mat.c")) and
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

predicate isNullLiteral(Expr e) {
  e.toString() = "0"
  or e.toString().matches("%NULL%")
}

predicate shareAssignedPointerValue(FunctionCall delCall, Variable aliasVar, Variable deletedVar) {
  exists(AssignExpr aliasAssign, AssignExpr deletedAssign, VariableAccess aliasSource, VariableAccess deletedSource |
    aliasAssign.getRValue() = aliasSource and
    deletedAssign.getRValue() = deletedSource and
    aliasSource.getTarget() = deletedSource.getTarget() and
    aliasVar = aliasAssign.getLValue().(VariableAccess).getTarget() and
    deletedVar = deletedAssign.getLValue().(VariableAccess).getTarget() and
    aliasAssign.getEnclosingFunction() = delCall.getEnclosingFunction() and
    deletedAssign.getEnclosingFunction() = delCall.getEnclosingFunction() and
    aliasAssign.getLocation().getStartLine() < delCall.getLocation().getStartLine() and
    deletedAssign.getLocation().getStartLine() < delCall.getLocation().getStartLine()
  )
}

predicate isAliasInvalidationAssignment(ExprStmt stmt, Variable aliasVar, Variable deletedVar) {
  exists(AssignExpr assign, VariableAccess lhs, VariableAccess rhs |
    stmt.getExpr() = assign and
    lhs = assign.getLValue() and
    rhs = assign.getRValue() and
    lhs.getTarget() = aliasVar and
    rhs.getTarget() = deletedVar
  )
  or
  exists(AssignExpr assign, VariableAccess lhs, Expr rhs |
    stmt.getExpr() = assign and
    lhs = assign.getLValue() and
    rhs = assign.getRValue() and
    lhs.getTarget() = aliasVar and
    isNullLiteral(rhs)
  )
}

predicate hasPatchGuard(FunctionCall delCall, Variable aliasVar, Variable deletedVar) {
  exists(IfStmt ifs, BinaryOperation comparison, VariableAccess aliasAccess, VariableAccess deletedAccess, ExprStmt thenStmt |
    ifs.getEnclosingFunction() = delCall.getEnclosingFunction() and
    comparison = ifs.getCondition() and
    comparison.getOperator() = "==" and
    exprContainsVariableAccess(comparison, aliasAccess) and
    aliasAccess.getTarget() = aliasVar and
    exprContainsVariableAccess(comparison, deletedAccess) and
    exprContainsVariableAccess(delCall.getArgument(0), deletedAccess) and
    ifs.getThen().getAChild*() = thenStmt and
    isAliasInvalidationAssignment(thenStmt, aliasVar, deletedVar) and
    ifs.getLocation().getStartLine() <= delCall.getLocation().getStartLine()
  )
}

predicate isSuspiciousUse(Variable aliasVar, Expr use) {
  exists(VariableAccess va |
    va = use and
    va.getTarget() = aliasVar
  )
}

predicate acceptedWithoutGuard(FunctionCall delCall, Variable aliasVar, Variable deletedVar, Expr use) {
  exists(VariableAccess argAccess |
    exprContainsVariableAccess(delCall.getArgument(0), argAccess) and
    argAccess.getTarget() = deletedVar
  ) and
  shareAssignedPointerValue(delCall, aliasVar, deletedVar) and
  isSuspiciousUse(aliasVar, use) and
  use.getEnclosingFunction() = delCall.getEnclosingFunction() and
  delCall.getLocation().getEndLine() < use.getLocation().getStartLine() and
  not hasPatchGuard(delCall, aliasVar, deletedVar)
}

from FunctionCall delCall, Variable aliasVar, Variable deletedVar, Expr use
where
  inPatchScope(delCall) and
  isDeleteImageFromListCall(delCall) and
  acceptedWithoutGuard(delCall, aliasVar, deletedVar, use) and
  inPatchScope(use)
select use, "A pointer alias remains usable after deleting the aliased list element; missing alias invalidation can lead to use-after-free."
