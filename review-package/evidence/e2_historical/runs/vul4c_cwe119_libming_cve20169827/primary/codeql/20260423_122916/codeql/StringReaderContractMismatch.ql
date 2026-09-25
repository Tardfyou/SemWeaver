/**
 * @name StringReaderContractMismatch
 * @description 检测在补丁作用域内，将通用字节读取 API 的返回值赋给随后按 C 字符串语义接受或使用的位置，且不存在字符串感知读取、显式终止或等价 fail-closed 约束的情况。
 * @kind problem
 * @problem.severity warning
 * @precision medium
 * @id cpp/custom/buffer_overflow
 * @tags security
 *       correctness
 */

import cpp
import semmle.code.cpp.dataflow.new.DataFlow

/**
 * Detects raw byte-reader results that are stored into a field and later
 * consumed as a C string near the patched parsing logic. The fixed code uses
 * `readString`, so calls rooted in `readString` are treated as a string-aware
 * barrier and excluded.
 */

predicate inPatchFile(Element e) {
  e.getFile().getRelativePath() = "util/parser.c"
}

predicate inPatchScope(Element e) {
  inPatchFile(e) and
  exists(Function f | f.getName() = "parseSWF_PROTECT" and f.getADeclarationEntry() = e.getEnclosingElement*())
}

predicate isRawByteReader(FunctionCall call) {
  call.getTarget().hasName("readBytes")
}

predicate isStringAwareReader(FunctionCall call) {
  call.getTarget().hasName("readString")
}

predicate isStringConsumer(FunctionCall call) {
  exists(Function target |
    target = call.getTarget() and
    (
      target.hasName("strlen") or
      target.hasName("strcmp") or
      target.hasName("strncmp") or
      target.hasName("strcasecmp") or
      target.hasName("strncasecmp") or
      target.hasName("strcpy") or
      target.hasName("strncpy") or
      target.hasName("strcat") or
      target.hasName("strncat") or
      target.hasName("strchr") or
      target.hasName("strrchr") or
      target.hasName("strstr") or
      target.hasName("puts")
    )
  )
}

predicate isTrackedStringArgument(FunctionCall call, int index) {
  isStringConsumer(call) and index = 0
  or
  exists(Function target |
    target = call.getTarget() and
    (
      target.hasName("strcmp") or
      target.hasName("strncmp") or
      target.hasName("strcasecmp") or
      target.hasName("strncasecmp") or
      target.hasName("strcpy") or
      target.hasName("strncpy") or
      target.hasName("strcat") or
      target.hasName("strncat")
    ) and
    (index = 0 or index = 1)
  )
}

module RawBytesToStringConfig implements DataFlow::ConfigSig {
  predicate isSource(DataFlow::Node source) {
    exists(FunctionCall call |
      isRawByteReader(call) and
      inPatchScope(call) and
      source.asExpr() = call
    )
  }

  predicate isSink(DataFlow::Node sink) {
    exists(FunctionCall call, int index |
      inPatchFile(call) and
      isTrackedStringArgument(call, index) and
      sink.asExpr() = call.getArgument(index)
    )
  }

  predicate isBarrier(DataFlow::Node node) {
    exists(FunctionCall call |
      isStringAwareReader(call) and
      node.asExpr() = call
    )
  }
}

module RawBytesToStringFlow = DataFlow::Global<RawBytesToStringConfig>;

predicate storedIntoAField(Expr rhs) {
  exists(AssignExpr assign, FieldAccess lhs |
    assign.getRValue() = rhs and
    lhs = assign.getLValue() and
    inPatchScope(assign)
  )
}

from DataFlow::Node source, DataFlow::Node sink, FunctionCall reader, FunctionCall consumer, int index
where
  RawBytesToStringFlow::flow(source, sink) and
  reader = source.asExpr() and
  isRawByteReader(reader) and
  inPatchScope(reader) and
  storedIntoAField(reader) and
  isTrackedStringArgument(consumer, index) and
  sink.asExpr() = consumer.getArgument(index)
select consumer,
  "Result of raw byte reader $@ flows into a string consumer here; use a string-aware reader or ensure NUL-termination before string use.",
  reader,
  reader.getTarget().getName()
