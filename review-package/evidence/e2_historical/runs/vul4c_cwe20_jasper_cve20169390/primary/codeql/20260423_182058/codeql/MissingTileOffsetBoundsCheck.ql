/**
 * @name MissingTileOffsetBoundsCheck
 * @description 在 `src/libjasper/jpc/jpc_dec.c` 的 `jpc_siz_getparms` 中，定位把 `tilexoff` / `tileyoff` 直接从输入流读入 SIZ 几何状态的位置。该读取点正是补丁为其补加图像边界校验的语义锚点。
 * @kind problem
 * @problem.severity warning
 * @precision medium
 * @id cpp/custom/unknown
 * @tags security
 *       correctness
 */

import cpp

predicate isTargetFile(File f) {
  f.getRelativePath() = "src/libjasper/jpc/jpc_dec.c"
}

predicate inPatchScope(Function f) {
  isTargetFile(f.getFile()) and
  f.getName() = "jpc_siz_getparms"
}

predicate readsTileOffset(FunctionCall call, string fieldName) {
  call.getTarget().hasName("jpc_getuint32") and
  fieldName = ["tilexoff", "tileyoff"] and
  exists(Expr arg, FieldAccess fa |
    arg = call.getArgument(1) and
    (fa = arg or arg.getAChild*() = fa) and
    fa.getTarget().getName() = fieldName
  )
}

predicate isGeometrySizParser(Function f) {
  exists(FunctionCall widthRead, FunctionCall heightRead |
    widthRead.getEnclosingFunction() = f and
    heightRead.getEnclosingFunction() = f and
    widthRead.getTarget().hasName("jpc_getuint32") and
    heightRead.getTarget().hasName("jpc_getuint32") and
    exists(Expr widthArg, FieldAccess widthField |
      widthArg = widthRead.getArgument(1) and
      (widthField = widthArg or widthArg.getAChild*() = widthField) and
      widthField.getTarget().getName() = "width"
    ) and
    exists(Expr heightArg, FieldAccess heightField |
      heightArg = heightRead.getArgument(1) and
      (heightField = heightArg or heightArg.getAChild*() = heightField) and
      heightField.getTarget().getName() = "height"
    )
  )
}

from Function f, FunctionCall call, string fieldName
where
  inPatchScope(f) and
  call.getEnclosingFunction() = f and
  isGeometrySizParser(f) and
  readsTileOffset(call, fieldName)
select call,
  "SIZ parsing reads `" + fieldName + "` directly from the input stream; this is the patch-local geometry anchor that requires a dedicated image-bound validation."
