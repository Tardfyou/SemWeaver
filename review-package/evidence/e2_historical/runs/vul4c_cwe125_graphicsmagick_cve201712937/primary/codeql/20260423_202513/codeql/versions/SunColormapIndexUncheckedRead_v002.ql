/**
 * @name SunColormapIndexUncheckedRead
 * @description 检测 `coders/sun.c` 中 SUN 解码路径里把局部变量 `index` 直接用作 `image->colormap[index]` 下标读取、且该读取点附近未出现新增的权威校验调用 `VerifyColormapIndex(image,index)` 的模式，作为补丁前越界读风险的 patch-local 触发。
 * @kind problem
 * @problem.severity warning
 * @precision medium
 * @id cpp/custom/buffer_overflow
 * @tags security
 *       correctness
 */

import cpp

predicate inPatchFile(Element e) {
  e.getFile().getRelativePath() = "coders/sun.c"
}

predicate exprContainsVariableAccess(Expr root, VariableAccess access) {
  access = root or root.getAChild*() = access
}

predicate exprContainsFieldAccess(Expr root, FieldAccess access) {
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
  or
  exists(FieldAccess l, FieldAccess r |
    exprContainsFieldAccess(left, l) and
    exprContainsFieldAccess(right, r) and
    l.getTarget() = r.getTarget() and
    sameValueExpr(l.getQualifier(), r.getQualifier())
  )
}

predicate isImageColormapField(Expr e, Expr imageBase) {
  exists(FieldAccess fa |
    fa = e and
    fa.getTarget().getName() = "colormap" and
    imageBase = fa.getQualifier()
  )
}

predicate isPatchLocalColormapRead(ArrayExpr read, Expr imageExpr, Expr indexExpr) {
  inPatchFile(read) and
  exists(Expr base |
    base = read.getArrayBase() and
    indexExpr = read.getArrayOffset() and
    isImageColormapField(base, imageExpr)
  )
}

predicate hasVerifyColormapIndex(Function f, Expr imageExpr, Expr indexExpr) {
  exists(FunctionCall verify |
    verify.getEnclosingFunction() = f and
    inPatchFile(verify) and
    verify.getTarget().getName() = "VerifyColormapIndex" and
    verify.getNumberOfArguments() >= 2 and
    sameValueExpr(verify.getArgument(0), imageExpr) and
    sameValueExpr(verify.getArgument(1), indexExpr)
  )
}

from ArrayExpr read, Function f, Expr imageExpr, Expr indexExpr
where
  read.getEnclosingFunction() = f and
  isPatchLocalColormapRead(read, imageExpr, indexExpr) and
  not hasVerifyColormapIndex(f, imageExpr, indexExpr)
select read, "Unchecked read from image colormap using index without VerifyColormapIndex(image, index)."