/**
 * @name UnboundedFormatToBufferChecker
 * @description Detects patch-local unbounded formatted writes to a local buffer via g_sprintf in xrdp_login_wnd.c.
 * @kind problem
 * @problem.severity warning
 * @precision medium
 * @id cpp/custom/unbounded_format_to_buffer_checker
 * @tags security
 *       correctness
 */

import cpp

predicate inPatchFile(FunctionCall call) {
  call.getFile().getRelativePath().matches("%xrdp/xrdp_login_wnd.c")
}

predicate isPatchedApiCall(FunctionCall call) {
  call.getTarget().hasName("g_sprintf")
}

predicate destinationIsLocalBuffer(FunctionCall call) {
  exists(VariableAccess va, LocalVariable lv |
    va = call.getArgument(0) and
    lv = va.getTarget()
  )
}

from FunctionCall call
where
  inPatchFile(call) and
  isPatchedApiCall(call) and
  destinationIsLocalBuffer(call)
select call,
  "Unbounded formatted write into a local buffer via g_sprintf; the patch replaces this sink with a size-bounded g_snprintf call."