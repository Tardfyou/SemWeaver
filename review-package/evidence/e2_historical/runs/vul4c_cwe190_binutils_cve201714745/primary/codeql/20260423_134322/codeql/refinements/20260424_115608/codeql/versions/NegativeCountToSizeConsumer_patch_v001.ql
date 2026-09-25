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
 * Detects signed count/error-code values returned from count-producing routines
 * and then consumed as non-negative element counts without a fail-closed
 * negative-value barrier.
 */

predicate exprContainsVariableAccess(Expr root, VariableAccess access) {
  access = root or root.getAChild*() = access
}

predicate inPatchScope(Element e) {
  exists(File f |
    f = e.getFile() and
    f.getRelativePath().matches("%bfd/elf64-x86-64.c")
  )
}

predicate isZeroLiteral(Expr e) {
  e instanceof Literal and
  e.(Literal).getValueText() = "0"
}

predicate isNegativeErrorCodeSource(FunctionCall src) {
  exists(Parameter bufferParam |
    src.getTarget().getName().matches("%canonicalize%") and
    src.getTarget().getType().(FunctionType).getReturnType() instanceof IntegralType and
    bufferParam = src.getTarget().getAParameter() and
    bufferParam.getType() instanceof PointerType and
    src.getNumberOfArguments() >= 2
  )
}

predicate isElementCountConsumer(FunctionCall sink, Expr countArg) {
  sink.getTarget().hasGlobalName("qsort") and
  countArg = sink.getArgument(1)
}

predicate assignFromNegativeErrorCodeSource(Assignment assign, Variable v, FunctionCall src) {
  exists(VariableAccess lhs |
    lhs = assign.getLValue() and
    lhs.getTarget() = v and
    src = assign.getRValue() and
    isNegativeErrorCodeSource(src)
  )
}

predicate countFlowsToConsumer(Variable v, FunctionCall src, FunctionCall sink) {
  exists(Assignment assign, VariableAccess use |
    assignFromNegativeErrorCodeSource(assign, v, src) and
    use = sink.getArgument(1) and
    use.getTarget() = v and
    assign.getEnclosingFunction() = sink.getEnclosingFunction()
  )
}

predicate hasNegativeValueBarrier(Variable v, FunctionCall sink) {
  exists(GuardCondition guard, VariableAccess guardedUse, Expr zero |
    guardedUse.getTarget() = v and
    isZeroLiteral(zero) and
    (
      guard.ensuresLt(guardedUse, zero, 0, sink.getBasicBlock(), true) or
      guard.ensuresLt(guardedUse, zero, 1, sink.getBasicBlock(), true) or
      guard.ensuresEq(guardedUse, zero, -1, sink.getBasicBlock(), false)
    )
  )
}

predicate acceptedWithoutNegativeBarrier(FunctionCall sink, Variable v, FunctionCall src) {
  isElementCountConsumer(sink, sink.getArgument(1)) and
  countFlowsToConsumer(v, src, sink) and
  not hasNegativeValueBarrier(v, sink)
}

from FunctionCall sink, Variable v, FunctionCall src
where inPatchScope(sink)
  and acceptedWithoutNegativeBarrier(sink, v, src)
select sink,
  "custom.NegativeCountToSizeConsumer: a signed count/error-code returned from a count-producing routine flows into a non-negative element-count consumer without a dominating fail-closed negative-value barrier."
