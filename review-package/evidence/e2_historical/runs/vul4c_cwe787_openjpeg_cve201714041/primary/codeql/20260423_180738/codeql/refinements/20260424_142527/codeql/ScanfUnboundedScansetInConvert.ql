/**
 * @name ScanfUnboundedScansetInConvert
 * @description 在 `src/bin/jp2/convert.c` 的补丁函数内定位对 `fscanf` 的调用，检查其格式字符串是否包含未带宽度的 scanset 转换 `%[...]`，并将该调用作为潜在的越界写触发点；修复版通过 `%31[...]` 之类宽度约束自然不再命中。
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

predicate formatHasUnboundedScanset(StringLiteral fmt) {
  exists(string value, int i |
    value = fmt.getValue() and
    i >= 0 and i < value.length() - 1 and
    value.charAt(i) = "%" and
    value.charAt(i + 1) = "["
  )
  or
  exists(string value, int i, int j |
    value = fmt.getValue() and
    i >= 0 and i < value.length() - 2 and
    value.charAt(i) = "%" and
    value.charAt(i + 1) = "*" and
    j = i + 2 and
    j < value.length() and
    value.charAt(j) = "["
  )
}

from FunctionCall call, StringLiteral fmt, Expr arg
where
  inPatchScope(call) and
  fmt = call.getArgument(1) and
  formatHasUnboundedScanset(fmt) and
  arg = call.getArgument(2) and
  argumentUsesFixedCharArray(arg)
select call, "Unbounded fscanf scanset conversion may overflow a fixed-size character buffer."