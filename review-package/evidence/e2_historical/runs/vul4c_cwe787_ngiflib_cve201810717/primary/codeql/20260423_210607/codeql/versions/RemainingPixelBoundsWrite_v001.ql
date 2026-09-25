/**
 * @name RemainingPixelBoundsWrite
 * @description 在 `ngiflib.c` 的补丁所在函数内，检测与补丁局部一致的像素输出越界模式：对 `WritePixel(i, &context, casspecial)` 的调用在漏洞版中未受 `npix > 0` 约束且其后紧邻/随后存在 `npix--`；或对 `WritePixels(i, &context, stackp, stack_top - stackp)` 的调用使用动态长度 `stack_top - stackp`，同时后续才执行 `npix -= (stack_top - stackp)`。查询首稿只锚定补丁文件、补丁附近 API、长度表达式和 `npix` 状态更新，不强制建模修复版 barrier。
 * @kind problem
 * @problem.severity warning
 * @precision medium
 * @id cpp/custom/buffer_overflow
 * @tags security
 *       correctness
 */

import cpp

predicate inPatchFile(File f) {
  f.getBaseName() = "ngiflib.c"
}

predicate exprContainsVariableAccess(Expr root, VariableAccess access) {
  access = root or root.getAChild*() = access
}

predicate exprContainsName(Expr root, string name) {
  exists(VariableAccess va |
    exprContainsVariableAccess(root, va) and
    va.getTarget().getName() = name
  )
}

predicate isNpixDecrementExpr(Expr e) {
  exists(UnaryOperation u, VariableAccess va |
    u = e and
    (u.getOperator() = "--" or u.getOperator() = "post--") and
    exprContainsVariableAccess(u.getOperand(), va) and
    va.getTarget().getName() = "npix"
  )
  or
  exists(AssignExpr a, VariableAccess lhs |
    a = e and
    a.getOperator() = "-=" and
    exprContainsVariableAccess(a.getLValue(), lhs) and
    lhs.getTarget().getName() = "npix"
  )
}

predicate hasLaterNpixDecrement(FunctionCall call) {
  exists(ExprStmt stmt, Expr dec |
    stmt.getExpr() = dec and
    isNpixDecrementExpr(dec) and
    stmt.getLocation().getStartLine() > call.getLocation().getStartLine() and
    stmt.getEnclosingFunction() = call.getEnclosingFunction()
  )
}

predicate isWritePixelTrigger(FunctionCall call) {
  call.getTarget().getName() = "WritePixel" and
  inPatchFile(call.getFile()) and
  hasLaterNpixDecrement(call)
}

predicate isStackSpanExpr(Expr e) {
  exists(AdditiveOperation sub |
    sub = e and
    sub.getOperator() = "-" and
    exprContainsName(sub.getLeftOperand(), "stack_top") and
    exprContainsName(sub.getRightOperand(), "stackp")
  )
}

predicate isWritePixelsTrigger(FunctionCall call) {
  call.getTarget().getName() = "WritePixels" and
  inPatchFile(call.getFile()) and
  isStackSpanExpr(call.getArgument(3)) and
  hasLaterNpixDecrement(call)
}

from FunctionCall call
where isWritePixelTrigger(call) or isWritePixelsTrigger(call)
select call, "Patch-local pixel write uses remaining-pixel state without sink-side bounds enforcement; later npix decrement indicates possible out-of-bounds write."