/**
 * @name PatchGuidedQuery
 * @description Early buffer free on a failing path without invalidating ownership before later cleanup
 * @kind problem
 * @problem.severity warning
 * @precision medium
 * @id cpp/custom/double_free_patch_scoped
 * @tags security
 *       correctness
 */

import cpp

predicate inTargetFunction(Function f) {
  f.getName() = "_zip_dirent_read" and
  f.getFile().getRelativePath().matches("%lib/zip_dirent.c")
}

predicate isBufferExpr(Expr e) {
  exists(VariableAccess va |
    va = e.getAChild*() and
    va.getTarget().getName() = "buffer"
  )
}

predicate isFromBufferExpr(Expr e) {
  exists(VariableAccess va |
    va = e.getAChild*() and
    va.getTarget().getName() = "from_buffer"
  )
}

predicate isBufferFreeCall(FunctionCall call) {
  call.getTarget().hasName("_zip_buffer_free") and
  isBufferExpr(call.getArgument(0))
}

predicate isFromBufferOwnershipCheck(IfStmt guard) {
  isFromBufferExpr(guard.getCondition())
}

predicate guardOwnsBuffer(IfStmt guard, FunctionCall freeCall) {
  isFromBufferOwnershipCheck(guard) and
  isBufferFreeCall(freeCall) and
  guard.getThen().getAChild*() = freeCall
}

predicate isFailureReturn(ReturnStmt ret) {
  exists(UnaryMinusExpr minus |
    minus = ret.getExpr() and
    minus.getOperand().toString() = "1"
  )
}

predicate returnsFailureFromThenBranch(IfStmt outer, ReturnStmt ret) {
  outer.getThen().getAChild*() = ret and
  isFailureReturn(ret)
}

predicate hasBufferInvalidationBetween(IfStmt outer, FunctionCall freeCall, ReturnStmt ret) {
  exists(AssignExpr assign, VariableAccess lhs |
    outer.getThen().getAChild*() = assign and
    lhs = assign.getLValue().getAChild*() and
    lhs.getTarget().getName() = "buffer" and
    freeCall.getLocation().getStartLine() < assign.getLocation().getStartLine() and
    assign.getLocation().getStartLine() < ret.getLocation().getStartLine()
  )
}

predicate nestedOwnedFreeBeforeFailureReturn(IfStmt outer, IfStmt ownershipGuard, FunctionCall freeCall, ReturnStmt ret) {
  returnsFailureFromThenBranch(outer, ret) and
  guardOwnsBuffer(ownershipGuard, freeCall) and
  ownershipGuard.getParent*() = outer.getThen() and
  not hasBufferInvalidationBetween(outer, freeCall, ret)
}

predicate isCleanupSuccessBranch(IfStmt guard) {
  exists(FunctionCall condCall |
    condCall = guard.getCondition().getAChild*() and
    condCall.getTarget().hasName("_zip_buffer_ok") and
    isBufferExpr(condCall.getArgument(0))
  )
}

predicate cleanupFreeAfterGuard(IfStmt guard, FunctionCall laterFree) {
  isCleanupSuccessBranch(guard) and
  isBufferFreeCall(laterFree) and
  laterFree.getParent*() = guard.getParent*() and
  not laterFree.getParent*() = guard.getThen() and
  not exists(IfStmt ownershipGuard |
    ownershipGuard.getThen().getAChild*() = laterFree and
    isFromBufferOwnershipCheck(ownershipGuard)
  )
}

from Function f, IfStmt failOuter, IfStmt ownershipGuard, FunctionCall freeCall, ReturnStmt ret, IfStmt cleanupGuard, FunctionCall laterFree
where
  inTargetFunction(f) and
  failOuter.getEnclosingFunction() = f and
  cleanupGuard.getEnclosingFunction() = f and
  nestedOwnedFreeBeforeFailureReturn(failOuter, ownershipGuard, freeCall, ret) and
  cleanupFreeAfterGuard(cleanupGuard, laterFree)
select freeCall,
  "Buffer ownership is discharged on an error path under '!from_buffer', but the same local may still reach later unconditional cleanup without being invalidated."
