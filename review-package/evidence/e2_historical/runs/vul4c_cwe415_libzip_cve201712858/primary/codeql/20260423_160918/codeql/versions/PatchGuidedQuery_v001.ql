/**
 * @name PatchGuidedQuery
 * @description PatchGuidedQuery
 * @kind problem
 * @problem.severity warning
 * @precision medium
 * @id cpp/custom/double_free
 * @tags security
 *       correctness
 */

import cpp

predicate inPatchScope(Function f) {
  f.getName() = "_zip_dirent_read"
  and f.getFile().getRelativePath().matches("%lib/zip_dirent.c")
}

predicate isZipBufferFreeCall(FunctionCall call, Expr freedExpr) {
  call.getTarget().hasName("_zip_buffer_free") and
  freedExpr = call.getArgument(0)
}

predicate sameUnderlyingVariable(Expr e1, Expr e2) {
  exists(VariableAccess v1, VariableAccess v2 |
    v1 = e1.getAChild*() and
    v2 = e2.getAChild*() and
    v1.getTarget() = v2.getTarget()
  )
  or
  exists(VariableAccess v |
    v = e1 and
    exists(VariableAccess v2 |
      v2 = e2.getAChild*() and
      v.getTarget() = v2.getTarget()
    )
  )
  or
  exists(VariableAccess v |
    v = e2 and
    exists(VariableAccess v1 |
      v1 = e1.getAChild*() and
      v1.getTarget() = v.getTarget()
    )
  )
}

predicate isFromBufferCheck(Expr cond) {
  exists(VariableAccess va |
    va = cond.getAChild*() and
    va.getTarget().getName() = "from_buffer"
  )
}

predicate inRemovedStyleFailureBranch(FunctionCall freeCall, FunctionCall aesCall) {
  aesCall.getTarget().hasName("_zip_dirent_process_winzip_aes") and
  exists(IfStmt outerIf, IfStmt innerIf |
    outerIf.getCondition() = aesCall and
    innerIf.getEnclosingStmt*() = outerIf.getThen() and
    freeCall.getEnclosingStmt() = innerIf.getThen() and
    isFromBufferCheck(innerIf.getCondition())
  )
}

predicate laterFreeOfSameValue(FunctionCall firstFree, Expr freedExpr, FunctionCall laterFree) {
  isZipBufferFreeCall(laterFree, _) and
  sameUnderlyingVariable(freedExpr, laterFree.getArgument(0)) and
  laterFree.getEnclosingFunction() = firstFree.getEnclosingFunction() and
  laterFree.getLocation().getStartLine() > firstFree.getLocation().getStartLine()
}

predicate patchGuidedCandidate(FunctionCall freeCall, FunctionCall aesCall, FunctionCall laterFree) {
  exists(Function f, Expr freedExpr |
    inPatchScope(f) and
    freeCall.getEnclosingFunction() = f and
    isZipBufferFreeCall(freeCall, freedExpr) and
    inRemovedStyleFailureBranch(freeCall, aesCall) and
    laterFreeOfSameValue(freeCall, freedExpr, laterFree)
  )
}

from FunctionCall freeCall, FunctionCall aesCall, FunctionCall laterFree
where patchGuidedCandidate(freeCall, aesCall, laterFree)
select freeCall, "Potential double free on an AES-processing error path: this conditional _zip_buffer_free may duplicate a later cleanup of the same buffer."