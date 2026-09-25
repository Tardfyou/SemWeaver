/**
 * @name IntegerOverflowInTileCountComputation
 * @description 检测在图像解码路径中，由两个独立的几何/维度计数相乘得到总数时，若未经过安全乘法辅助函数或失败即返回的屏障校验，就将结果赋给状态字段并用于后续分配/长度语义的位置。
 * @kind problem
 * @problem.severity warning
 * @precision medium
 * @id cpp/custom/unknown
 * @tags security
 *       correctness
 */

import cpp

predicate inPatchScope(Function f) {
  f.getName() = "jpc_dec_cp_create"
  and
  f.getFile().getRelativePath().matches("%src/libjasper/jpc/jpc_dec.c")
}

predicate isCountLikeLvalue(Expr e) {
  exists(FieldAccess fa |
    e = fa and
    fa.getTarget().getName().regexpMatch(".*(tiles|count|num|size|len).*")
  )
  or
  exists(VariableAccess va |
    e = va and
    va.getTarget().getName().regexpMatch(".*(tiles|count|num|size|len).*")
  )
}

predicate assignedFromProduct(AssignExpr assign, MulExpr mul) {
  assign.getRValue() = mul and
  isCountLikeLvalue(assign.getLValue()) and
  isCountLikeLvalue(mul.getLeftOperand()) and
  isCountLikeLvalue(mul.getRightOperand())
}

predicate flowsToAllocationCount(AssignExpr assign, FunctionCall allocCall) {
  exists(Expr lhs, Expr countArg |
    lhs = assign.getLValue() and
    countArg = allocCall.getArgument(0) and
    (
      countArg = lhs
      or
      exists(FieldAccess lhsFa, FieldAccess argFa |
        lhs = lhsFa and
        countArg = argFa and
        lhsFa.getTarget() = argFa.getTarget()
      )
      or
      exists(VariableAccess lhsVa, VariableAccess argVa |
        lhs = lhsVa and
        countArg = argVa and
        lhsVa.getTarget() = argVa.getTarget()
      )
    )
  ) and
  allocCall.getTarget().getName().regexpMatch("jas_alloc2|jas_malloc|malloc|calloc|realloc")
}

predicate hasCheckedMulGuardFor(MulExpr mul, Expr assignedValue) {
  exists(FunctionCall safeCall |
    safeCall.getTarget().getName() = "jas_safe_size_mul" and
    exists(Expr arg0, Expr arg1, Expr arg2 |
      arg0 = safeCall.getArgument(0) and
      arg1 = safeCall.getArgument(1) and
      arg2 = safeCall.getArgument(2) and
      (
        arg0 = mul.getLeftOperand()
        or
        arg0 = mul.getRightOperand()
      ) and
      (
        arg1 = mul.getLeftOperand()
        or
        arg1 = mul.getRightOperand()
      ) and
      exists(AssignExpr safeAssign |
        safeAssign.getRValue() = assignedValue and
        safeAssign.getEnclosingFunction() = mul.getEnclosingFunction() and
        safeCall.getEnclosingFunction() = mul.getEnclosingFunction()
      )
    )
  )
}

from Function f, AssignExpr assign, MulExpr mul, FunctionCall allocCall
where
  inPatchScope(f) and
  assign.getEnclosingFunction() = f and
  mul.getEnclosingFunction() = f and
  allocCall.getEnclosingFunction() = f and
  assignedFromProduct(assign, mul) and
  flowsToAllocationCount(assign, allocCall) and
  not hasCheckedMulGuardFor(mul, assign.getLValue())
select assign, "Unchecked multiplication computes an aggregate count that flows to allocation sizing; use an overflow-checked multiplication guard before assigning the tile count."