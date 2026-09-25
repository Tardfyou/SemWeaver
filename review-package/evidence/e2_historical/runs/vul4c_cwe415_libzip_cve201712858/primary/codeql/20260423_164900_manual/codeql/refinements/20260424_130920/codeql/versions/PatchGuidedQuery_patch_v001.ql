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

predicate isFromBufferCheck(IfStmt guard) {
  exists(VariableAccess va |
    va = guard.getCondition().getAChild*() and
    va.getTarget().getName() = "from_buffer"
  )
}

predicate freesBufferUnderOwnershipCheck(IfStmt guard, FunctionCall freeCall) {
  isFromBufferCheck(guard) and
  freeCall.getTarget().hasName("_zip_buffer_free") and
  isBufferExpr(freeCall.getArgument(0)) and
  guard.getThen().getAChild*() = freeCall
}

predicate isFailureBranch(IfStmt failureIf) {
  exists(Expr cond |
    cond = failureIf.getCondition() and
    cond.toString().regexpMatch(".*_zip_dirent_process_winzip_aes.*") and
    failureIf.getThen() instanceof BlockStmt
  )
}

predicate branchReturnsFailure(BlockStmt block) {
  exists(ReturnStmt ret |
    block.getAChild*() = ret and
    ret.getExpr().toString() = "-1"
  )
}

predicate hasOwnershipReset(BlockStmt block) {
  exists(AssignExpr assign, VariableAccess lhs |
    block.getAChild*() = assign and
    lhs = assign.getLValue().getAChild*() and
    lhs.getTarget().getName() = "buffer"
  )
}

predicate hasLaterUnconditionalBufferFree(Function f, IfStmt siteIf) {
  exists(FunctionCall later |
    later.getEnclosingFunction() = f and
    later.getTarget().hasName("_zip_buffer_free") and
    isBufferExpr(later.getArgument(0)) and
    later.getLocation().getStartLine() > siteIf.getLocation().getStartLine() and
    not exists(IfStmt guard | guard.getThen().getAChild*() = later)
  )
}

from Function f, IfStmt failureIf, BlockStmt failBlock, IfStmt freeGuard, FunctionCall freeCall
where
  inTargetFunction(f) and
  failureIf.getEnclosingFunction() = f and
  isFailureBranch(failureIf) and
  failBlock = failureIf.getThen().(BlockStmt) and
  branchReturnsFailure(failBlock) and
  freeGuard.getParent*() = failBlock and
  freesBufferUnderOwnershipCheck(freeGuard, freeCall) and
  not hasOwnershipReset(failBlock) and
  hasLaterUnconditionalBufferFree(f, failureIf)
select freeCall,
  "Patch-scoped risk: a failing branch frees buffer under the ownership check but returns without invalidating that ownership before later cleanup in the function."
