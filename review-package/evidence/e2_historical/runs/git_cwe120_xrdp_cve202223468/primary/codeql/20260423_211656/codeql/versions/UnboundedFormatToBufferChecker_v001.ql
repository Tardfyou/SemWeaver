/**
 * @name UnboundedFormatToBufferChecker
 * @description 检测在补丁涉及位置使用无界格式化函数将内容写入本地缓冲区的模式，首稿直接匹配 `xrdp/xrdp_login_wnd.c` 中相关函数里的 `g_sprintf` 调用，并将目标缓冲区写入点作为结果返回。
 * @kind problem
 * @problem.severity warning
 * @precision medium
 * @id cpp/custom/buffer_overflow
 * @tags security
 *       correctness
 */

/**
 * @name UnboundedFormatToBufferChecker
 * @description Detects patch-local unbounded formatted writes to a local buffer via g_sprintf in xrdp_login_wnd.c.
 * @kind problem
 * @problem.severity warning
 * @precision medium
 * @id cpp/custom/unbounded_format_to_buffer_checker
 */

import cpp

predicate inPatchFile(FunctionCall call) {
  call.getFile().getRelativePath().matches("%xrdp/xrdp_login_wnd.c")
}

predicate isPatchedApiCall(FunctionCall call) {
  call.getTarget().hasName("g_sprintf")
}

predicate destinationIsLocalBuffer(FunctionCall call) {
  exists(VariableAccess va |
    va = call.getArgument(0) and
    va.getTarget().isLocal()
  )
}

from FunctionCall call
where
  inPatchFile(call) and
  isPatchedApiCall(call) and
  destinationIsLocalBuffer(call)
select call,
  "Unbounded formatted write into a local buffer via g_sprintf; the patch replaces this sink with a size-bounded g_snprintf call."