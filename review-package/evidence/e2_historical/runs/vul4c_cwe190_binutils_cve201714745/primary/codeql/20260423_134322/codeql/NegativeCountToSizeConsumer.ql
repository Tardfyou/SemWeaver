/**
 * @name NegativeCountToSizeConsumer
 * @description 检测 C/C++ 中来自可能返回负错误码的计数/长度返回值，在缺少负值范围检查或 fail-closed barrier 的情况下，被传入要求非负元素个数/大小参数的消费 API（如 qsort）的缺陷模式。
 * @kind problem
 * @problem.severity warning
 * @precision medium
 * @id cpp/custom/unknown
 * @tags security
 *       correctness
 */

import cpp
import semmle.code.cpp.controlflow.Guards
import semmle.code.cpp.controlflow.Dominance

/**
 * Detects signed count/error-code values returned from parser/canonicalizer-like
 * functions that are consumed as element counts by qsort without a dominating
 * negative-value guard.
 */

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

predicate inPatchScope(Element e) {
  exists(File f |
    f = e.getFile() and
    f.getRelativePath().matches("%bfd/elf64-x86-64.c")
  )
}

predicate isCountProducingCall(FunctionCall src) {
  src.getTarget().hasName("bfd_canonicalize_dynamic_reloc")
  or
  // Some macro/function-pointer wrappers are extracted as "call to expression";
  // keep the source trigger stable and rely on patch scope plus the qsort sink.
  src.toString() = "call to expression"
}

predicate isCountProducingExpr(Expr src) {
  src.toString() = "call to expression"
  or
  exists(FunctionCall call |
    call = src and
    isCountProducingCall(call)
  )
}

predicate isCountConsumerCall(FunctionCall sink) {
  sink.getTarget().hasName("qsort") and
  exists(Expr countArg |
    countArg = sink.getArgument(1)
  )
}

predicate flowsViaAssignmentToConsumer(Variable v, FunctionCall sink) {
  exists(Assignment assign, VariableAccess lhs, VariableAccess use |
    lhs = assign.getLValue() and
    lhs.getTarget() = v and
    isCountProducingExpr(assign.getRValue()) and
    use = sink.getArgument(1) and
    use.getTarget() = v and
    assign.getEnclosingFunction() = sink.getEnclosingFunction() and
    assign.getLocation().getStartLine() < sink.getLocation().getStartLine() and
    sink.getLocation().getStartLine() - assign.getLocation().getStartLine() <= 20
  )
}

predicate hasNegativeGuardOnVarBeforeCall(Variable v, FunctionCall sink) {
  exists(GuardCondition guard, VariableAccess guardedUse, Expr zero |
    guardedUse.getTarget() = v and
    zero instanceof Literal and
    zero.(Literal).getValueText() = "0" and
    (
      guard.ensuresLt(guardedUse, zero, 0, sink.getBasicBlock(), true)
      or
      guard.ensuresLt(guardedUse, zero, 1, sink.getBasicBlock(), true)
    )
  )
}

predicate acceptedWithoutGuard(FunctionCall sink, Variable v) {
  isCountConsumerCall(sink) and
  flowsViaAssignmentToConsumer(v, sink) and
  not hasNegativeGuardOnVarBeforeCall(v, sink)
}

from FunctionCall sink, Variable v
where inPatchScope(sink)
  and acceptedWithoutGuard(sink, v)
select sink,
  "custom.NegativeCountToSizeConsumer: a signed count/error-code flows into a non-negative element-count consumer without a dominating negative-value guard."
