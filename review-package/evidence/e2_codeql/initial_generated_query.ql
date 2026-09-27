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
  e.getFile().getRelativePath().matches("%/coders/mat.c") and
  exists(Function f | e.getEnclosingElement*() = f and f.getName() = "ReadMATImage")
}

predicate exprContainsVariableAccess(Expr root, VariableAccess access) {
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
  exists(IfStmt ifs, Expr cond, VariableAccess a1, VariableAccess a2, ExprStmt thenStmt |
    ifs.getEnclosingFunction() = delCall.getEnclosingFunction() and
    cond = ifs.getCondition() and
    exprContainsVariableAccess(cond, a1) and
    exprContainsVariableAccess(cond, a2) and
    a1.getTarget() = aliasVar and
    a2.getTarget() = deletedVar and
    thenStmt = ifs.getThen().(ExprStmt) and
    isAliasInvalidationAssignment(thenStmt, aliasVar, deletedVar) and
    ifs.getLocation().getStartLine() <= delCall.getLocation().getStartLine()
  )
}

predicate isSuspiciousUse(Variable aliasVar, Expr use) {
  exists(VariableAccess va |
    exprContainsVariableAccess(use, va) and
    va.getTarget() = aliasVar
  ) and
  not exists(AssignExpr assign, VariableAccess lhs |
    assign = use and
    lhs = assign.getLValue() and
    lhs.getTarget() = aliasVar
  )
}

predicate acceptedWithoutGuard(FunctionCall delCall, Variable aliasVar, Variable deletedVar, Expr use) {
  exists(VariableAccess argAccess |
    exprContainsVariableAccess(delCall.getArgument(0), argAccess) and
    argAccess.getTarget() = deletedVar
  ) and
  isSuspiciousUse(aliasVar, use) and
  use.getEnclosingFunction() = delCall.getEnclosingFunction() and
  delCall.getLocation().getEndLine() < use.getLocation().getStartLine() and
  not hasPatchGuard(delCall, aliasVar, deletedVar)
}

from FunctionCall delCall, Variable aliasVar, Variable deletedVar, Expr use
where
  inPatchScope(delCall) and
  isDeleteImageFromListCall(delCall) and
  aliasVar.getName() = "image2" and
  deletedVar.getName() = "tmp" and
  acceptedWithoutGuard(delCall, aliasVar, deletedVar, use) and
  inPatchScope(use)
select use, "A pointer alias remains usable after deleting the aliased list element; missing alias invalidation can lead to use-after-free."
