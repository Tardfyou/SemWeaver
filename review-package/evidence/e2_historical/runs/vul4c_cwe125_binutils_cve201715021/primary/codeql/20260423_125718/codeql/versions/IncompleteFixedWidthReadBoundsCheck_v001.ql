/**
 * @name IncompleteFixedWidthReadBoundsCheck
 * @description 检测在固定宽度内存/缓冲区读取前，仅验证读取起始偏移未越界、但未验证 offset + access_width 不超过缓冲区大小的缺陷，并将新增的完整范围检查建模为 guard/barrier 以在修复后静默。
 * @kind problem
 * @problem.severity warning
 * @precision medium
 * @id cpp/custom/buffer_overflow
 * @tags security
 *       correctness
 */

import cpp
import semmle.code.cpp.controlflow.Guards

/**
 * Detects fixed-width reads that are protected only by a start-offset check
 * instead of a width-aware bounds check.
 */

predicate inPatchScope(FunctionCall call) {
  call.getFile().getRelativePath().matches("%bfd/opncls.c") and
  call.getEnclosingFunction().getName() = "bfd_get_alt_debug_link_info"
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

predicate isFixedWidthReadCall(FunctionCall call, int width, Expr offset) {
  call.getTarget().hasName("bfd_get_32") and
  width = 4 and
  exists(AddExpr add |
    call.getNumberOfArguments() >= 2 and
    add = call.getArgument(1) and
    offset = add.getAnOperand()
  )
}

predicate getsSectionSize(Expr e) {
  exists(FunctionCall sizeCall |
    sizeCall = e and
    sizeCall.getTarget().hasName("bfd_get_section_size")
  )
}

predicate hasStartOnlyGuard(FunctionCall call, Expr offset, Expr capacity) {
  exists(GuardCondition guard |
    getsSectionSize(capacity) and
    (
      guard.ensuresLt(offset, capacity, 0, call.getBasicBlock(), true)
      or
      guard.ensuresLt(offset, capacity, 1, call.getBasicBlock(), true)
    )
  )
}

predicate hasWidthAwareGuard(FunctionCall call, Expr offset, Expr capacity, int width) {
  exists(GuardCondition guard |
    getsSectionSize(capacity) and
    (
      guard.ensuresLt(offset, capacity, width - 1, call.getBasicBlock(), true)
      or
      guard.ensuresLt(offset, capacity, width, call.getBasicBlock(), true)
    )
  )
}

predicate patchGuidedCandidate(FunctionCall call) {
  exists(Expr offset, Expr capacity, int width |
    isFixedWidthReadCall(call, width, offset) and
    getsSectionSize(capacity) and
    hasStartOnlyGuard(call, offset, capacity) and
    not hasWidthAwareGuard(call, offset, capacity, width)
  ) and
  inPatchScope(call)
}

from FunctionCall call
where patchGuidedCandidate(call)
select call, "Fixed-width read may overrun the buffer because only the starting offset is checked, not the full read width."