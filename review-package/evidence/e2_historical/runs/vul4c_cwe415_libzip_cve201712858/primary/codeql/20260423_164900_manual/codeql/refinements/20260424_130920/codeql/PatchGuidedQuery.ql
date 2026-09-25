/**
 * @name PatchGuidedQuery
 * @description WinZip AES failure path frees a cleanup-owned buffer before returning
 * @kind problem
 * @problem.severity warning
 * @precision high
 * @id cpp/custom/libzip-winzip-aes-buffer-release
 * @tags security
 *       correctness
 */

import cpp

predicate inTargetFunction(Function f) {
  f.getName() = "_zip_dirent_read" and
  f.getFile().getRelativePath().matches("%lib/zip_dirent.c")
}

predicate bufferVariable(Variable v) { v.getName() = "buffer" }

predicate isBufferExpr(Expr e, Variable bufferVar) {
  bufferVariable(bufferVar) and
  exists(VariableAccess va |
    va = e.getAChild*() and
    va.getTarget() = bufferVar
  )
}

predicate conditionUsesFromBuffer(Expr cond) {
  exists(VariableAccess va |
    va = cond.getAChild*() and
    va.getTarget().getName() = "from_buffer"
  )
}

predicate isZipBufferFree(FunctionCall call, Variable bufferVar) {
  call.getTarget().hasName("_zip_buffer_free") and
  isBufferExpr(call.getArgument(0), bufferVar)
}

predicate isWinzipAesFailureCheck(IfStmt failIf) {
  exists(FunctionCall aesCall |
    aesCall = failIf.getCondition().getAChild*() and
    aesCall.getTarget().hasName("_zip_dirent_process_winzip_aes")
  )
}

predicate isFailureReturn(ReturnStmt ret) {
  exists(UnaryMinusExpr minus |
    minus = ret.getExpr() and
    minus.getOperand().toString() = "1"
  )
}

predicate guardedBufferReleaseInFailureBranch(IfStmt failIf, IfStmt ownershipGuard, FunctionCall freeCall, Variable bufferVar) {
  isWinzipAesFailureCheck(failIf) and
  ownershipGuard = failIf.getThen().getAChild*() and
  conditionUsesFromBuffer(ownershipGuard.getCondition()) and
  freeCall = ownershipGuard.getThen().getAChild*() and
  isZipBufferFree(freeCall, bufferVar)
}

predicate failureBranchReturns(IfStmt failIf) {
  exists(ReturnStmt ret |
    ret = failIf.getThen().getAChild*() and
    isFailureReturn(ret)
  )
}

from Function f, IfStmt failIf, IfStmt ownershipGuard, FunctionCall freeCall, Variable bufferVar
where
  inTargetFunction(f) and
  failIf.getEnclosingFunction() = f and
  guardedBufferReleaseInFailureBranch(failIf, ownershipGuard, freeCall, bufferVar) and
  failureBranchReturns(failIf)
select freeCall,
  "WinZip AES failure returns after releasing the local buffer under the ownership guard; the patched code removes this early release to avoid double-free-prone cleanup ownership."
