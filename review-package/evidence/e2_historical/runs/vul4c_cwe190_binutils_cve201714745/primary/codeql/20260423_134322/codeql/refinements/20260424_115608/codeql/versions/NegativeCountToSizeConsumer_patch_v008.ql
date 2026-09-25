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

predicate mayReturnNegativeCount(FunctionCall src) {
  exists(Function f, Parameter outParam |
    f = src.getTarget() and
    f.getType().getUnspecifiedType() instanceof IntegralType and
    src.getNumberOfArguments() >= 2 and
    outParam = f.getAParameter() and
    outParam.getType() instanceof PointerType
  )
}

predicate isElementCountConsumer(FunctionCall sink, Expr countArg) {
  exists(Function f |
    f = sink.getTarget() and
    f.hasGlobalName("qsort") and
    sink.getNumberOfArguments() = 4 and
    countArg = sink.getArgument(1)
  )
}

predicate assignFromNegativeErrorCodeSource(Assignment assign, Variable v, FunctionCall src) {
  exists(VariableAccess lhs |
    lhs = assign.getLValue() and
    lhs.getTarget() = v and
    src = assign.getRValue() and
    mayReturnNegativeCount(src)
  )
}

predicate countFlowsToConsumer(Variable v, FunctionCall src, FunctionCall sink) {
  exists(Assignment assign, VariableAccess use |
    assignFromNegativeErrorCodeSource(assign, v, src) and
    use = sink.getArgument(1) and
    use.getTarget() = v and
    assign.getEnclosingFunction() = sink.getEnclosingFunction() and
    assign.getLocation().getStartLine() < sink.getLocation().getStartLine()
  )
}

predicate guardChecksNegative(GuardCondition guard, VariableAccess guardedUse, Expr zero, FunctionCall sink) {
  guard.ensuresLt(guardedUse, zero, 0, sink.getBasicBlock(), true)
  or
  guard.ensuresLt(guardedUse, zero, 1, sink.getBasicBlock(), true)
}

predicate guardBranchReturnsBeforeSink(GuardCondition guard, Variable v, FunctionCall sink) {
  exists(IfStmt ifs, VariableAccess guardedUse, Expr zero, ReturnStmt ret, BasicBlock thenBlock |
    ifs.getCondition() = guard and
    guardedUse.getTarget() = v and
    isZeroLiteral(zero) and
    guardChecksNegative(guard, guardedUse, zero, sink) and
    thenBlock = ifs.getThen().getBasicBlock() and
    ret.getBasicBlock() = thenBlock and
    ret = ifs.getThen().getAChild*()
  )
}

predicate hasNegativeValueBarrier(Variable v, FunctionCall sink) {
  exists(GuardCondition guard, IfStmt ifs |
    guardBranchReturnsBeforeSink(guard, v, sink) and
    ifs.getCondition() = guard and
    ifs.getBasicBlock().dominates(sink.getBasicBlock())
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
