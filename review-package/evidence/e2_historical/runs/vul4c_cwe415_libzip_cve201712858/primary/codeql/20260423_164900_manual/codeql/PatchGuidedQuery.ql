/**
 * @name PatchGuidedQuery
 * @description Patch-scoped branch-local buffer cleanup pattern in _zip_dirent_read
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

predicate isFromBufferGuard(Expr cond) {
  exists(VariableAccess va |
    va = cond.getAChild*() and
    va.getTarget().getName() = "from_buffer"
  )
}

predicate isScopedBranchFree(FunctionCall freeCall, IfStmt guard) {
  freeCall.getTarget().hasName("_zip_buffer_free") and
  isBufferExpr(freeCall.getArgument(0)) and
  guard.getThen().getAChild*() = freeCall and
  isFromBufferGuard(guard.getCondition())
}

predicate hasLaterBufferFree(FunctionCall firstFree) {
  exists(FunctionCall later |
    later.getTarget().hasName("_zip_buffer_free") and
    later.getEnclosingFunction() = firstFree.getEnclosingFunction() and
    later.getLocation().getStartLine() > firstFree.getLocation().getStartLine() and
    isBufferExpr(later.getArgument(0))
  )
}

from Function f, FunctionCall freeCall, IfStmt guard
where
  inTargetFunction(f) and
  freeCall.getEnclosingFunction() = f and
  isScopedBranchFree(freeCall, guard) and
  hasLaterBufferFree(freeCall)
select freeCall,
  "Patch-scoped risk: _zip_buffer_free(buffer) is executed in a !from_buffer branch and later cleanup of buffer still exists in the same function."
