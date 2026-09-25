/**
 * @name ScanfUnboundedScansetInConvert
 * @description 在 `src/bin/jp2/convert.c` 中定位 `fscanf` 将未带宽度的赋值 scanset 转换 `%[...]` 写入固定大小字符数组的调用；修复版通过 `%31[...]` 之类宽度约束自然不再命中。
 * @kind problem
 * @problem.severity warning
 * @precision medium
 * @id cpp/custom/buffer_overflow
 * @tags security
 *       correctness
 */

import cpp

predicate inPatchScope(FunctionCall call) {
  call.getTarget().hasGlobalName("fscanf") and
  call.getFile().getRelativePath() = "src/bin/jp2/convert.c"
}

predicate exprContainsVariableAccess(Expr root, VariableAccess access) {
  access = root or root.getAChild*() = access
}

predicate isFixedCharArrayVariable(Variable v) {
  exists(ArrayType at, Type base |
    v.getType() = at and
    base = at.getBaseType() and
    base instanceof CharType
  )
}

predicate argumentUsesFixedCharArray(Expr arg) {
  exists(VariableAccess va |
    exprContainsVariableAccess(arg, va) and
    isFixedCharArrayVariable(va.getTarget())
  )
}

predicate formatHasAssigningUnboundedScanset(StringLiteral fmt) {
  exists(string value, int i |
    value = fmt.getValue() and
    i >= 0 and i < value.length() - 1 and
    value.charAt(i) = "%" and
    value.charAt(i + 1) = "["
  )
}

predicate scanfDestinationUsesFixedCharArray(FunctionCall call, Expr arg) {
  exists(int argumentIndex |
    argumentIndex >= 2 and
    arg = call.getArgument(argumentIndex) and
    argumentUsesFixedCharArray(arg)
  )
}

from FunctionCall call, StringLiteral fmt, Expr arg
where
  inPatchScope(call) and
  fmt = call.getArgument(1) and
  formatHasAssigningUnboundedScanset(fmt) and
  scanfDestinationUsesFixedCharArray(call, arg)
select call, "Unbounded fscanf scanset conversion may overflow a fixed-size character buffer."