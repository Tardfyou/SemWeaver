/**
 * @name PngReadJNGImageErrorPathUseAfterFree
 * @description 在 `coders/png.c` 的 `ReadJNGImage` 中查找补丁附近被删除的 `ThrowReaderException` 调用，重点命中其第三个实参为 `image`、`color_image` 或 `alpha_image` 的错误处理路径；优先覆盖这些调用前同一分支/同一函数内紧邻出现的 `DestroyJNGInfo(...)`、`DestroyImage(...)`、分配失败、`OpenBlob` 失败或不完整读取等 patch-local 事实，作为潜在悬垂对象被异常处理消费的触发器。
 * @kind problem
 * @problem.severity warning
 * @precision medium
 * @id cpp/custom/use_after_free
 * @tags security
 *       correctness
 */

import cpp

predicate isPngFile(File f) {
  f.getRelativePath() = "coders/png.c"
}

predicate inPatchScope(FunctionCall call) {
  exists(Function fn, File f |
    fn = call.getEnclosingFunction() and
    fn.getName() = "ReadJNGImage" and
    f = fn.getFile() and
    isPngFile(f)
  )
}

predicate isTrackedImageLikeArg(Expr e) {
  exists(VariableAccess va |
    va = e and
    (
      va.getTarget().getName() = "image" or
      va.getTarget().getName() = "color_image" or
      va.getTarget().getName() = "alpha_image"
    )
  )
}

predicate hasNearbyTeardown(FunctionCall throwCall) {
  exists(FunctionCall cleanup |
    cleanup.getEnclosingFunction() = throwCall.getEnclosingFunction() and
    cleanup.getLocation().getStartLine() <= throwCall.getLocation().getStartLine() and
    (
      cleanup.getTarget().getName() = "DestroyJNGInfo" or
      cleanup.getTarget().getName() = "DestroyImage" or
      cleanup.getTarget().getName() = "DestroyImageInfo"
    )
  )
}

predicate isPatchLocalTrigger(FunctionCall call) {
  inPatchScope(call) and
  call.getTarget().getName() = "ThrowReaderException" and
  call.getNumArgument() >= 3 and
  isTrackedImageLikeArg(call.getArgument(call.getNumArgument() - 1)) and
  hasNearbyTeardown(call)
}

from FunctionCall call
where isPatchLocalTrigger(call)
select call, "Patch-local JNG error-path exception uses an image-like pointer after nearby teardown/partial cleanup in ReadJNGImage."