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

predicate isNegatedFromBufferGuard(IfStmt guard) {
  isFromBufferExpr(guard.getCondition())
}

predicate freesBufferUnderOwnershipCheck(IfStmt guard, FunctionCall freeCall) {
  isNegatedFromBufferGuard(guard) and
  isBufferFreeCall(freeCall) and
  guard.getThen().getAChild*() = freeCall
}

predicate returnsFailureCode(ReturnStmt ret) {
  exists(Expr value |
    value = ret.getExpr() and
    value.toString() = "-1"
  )
}

predicate hasFailureReturn(BlockStmt block, ReturnStmt ret) {
  block.getAChild*() = ret and
  returnsFailureCode(ret)
}

predicate callPrecedesReturnInBlock(BlockStmt block, FunctionCall call, ReturnStmt ret) {
  block.getAChild*() = call and
  hasFailureReturn(block, ret) and
  call.getLocation().getStartLine() < ret.getLocation().getStartLine()
}

predicate hasOwnershipReset(BlockStmt block) {
  exists(AssignExpr assign, VariableAccess lhs |
    block.getAChild*() = assign and
    lhs = assign.getLValue().getAChild*() and
    lhs.getTarget().getName() = "buffer"
  )
}

predicate freesBufferBeforeFailureReturnWithoutReset(BlockStmt block, IfStmt freeGuard, FunctionCall freeCall, ReturnStmt ret) {
  freesBufferUnderOwnershipCheck(freeGuard, freeCall) and
  freeGuard.getParent*() = block and
  callPrecedesReturnInBlock(block, freeCall, ret) and
  not hasOwnershipReset(block)
}

predicate hasCleanupFreeAfterFailure(Function f, BlockStmt block, ReturnStmt ret, FunctionCall laterFree) {
  laterFree.getEnclosingFunction() = f and
  isBufferFreeCall(laterFree) and
  callPrecedesReturnInBlock(block, laterFree, ret) = false and
  not laterFree.getParent*() = block and
  not exists(IfStmt guard |
    guard.getThen().getAChild*() = laterFree and
    isNegatedFromBufferGuard(guard)
  )
}

from Function f, BlockStmt failBlock, IfStmt freeGuard, FunctionCall freeCall, ReturnStmt ret, FunctionCall laterFree
where
  inTargetFunction(f) and
  freeGuard.getEnclosingFunction() = f and
  freesBufferBeforeFailureReturnWithoutReset(failBlock, freeGuard, freeCall, ret) and
  hasCleanupFreeAfterFailure(f, failBlock, ret, laterFree)
select freeCall,
  "Patch-scoped risk: buffer is freed on an error path under the ownership check, then the path returns without invalidating ownership before later cleanup frees the same buffer again."
