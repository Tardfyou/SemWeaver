/**
 * @name MissingTileOffsetBoundsCheck
 * @description 在 `src/libjasper/jpc/jpc_dec.c` 的 SIZ 解析/校验逻辑中，检测对 `siz->tilexoff`, `siz->tileyoff`, `siz->width`, `siz->height` 这组图像几何字段缺少关键关系约束的情形，重点命中补丁新增 guard 所在函数附近的漏洞前状态。
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
  f.getName() = "jpc_dec_process_siz"
}

predicate exprContainsFieldNamed(Expr root, string name) {
  exists(FieldAccess fa |
    (fa = root or root.getAChild*() = fa) and
    fa.getTarget().getName() = name
  )
}

predicate comparesTileOffsetAndImageBound(Expr e) {
  exists(RelationalOperation ro |
    ro = e and
    (
      exprContainsFieldNamed(ro.getAnOperand(), "tilexoff") and
      exprContainsFieldNamed(ro.getAnOperand(), "width")
    )
    or
    (
      exprContainsFieldNamed(ro.getAnOperand(), "tileyoff") and
      exprContainsFieldNamed(ro.getAnOperand(), "height")
    )
  )
}

predicate hasTileOffsetBoundsGuard(Function f) {
  exists(IfStmt ifs |
    ifs.getEnclosingFunction() = f and
    comparesTileOffsetAndImageBound(ifs.getCondition())
  )
}

from Function f
where inPatchScope(f) and not hasTileOffsetBoundsGuard(f)
select f, "Missing validation that SIZ tile offsets stay within image bounds (for example, tilexoff < width and tileyoff < height)."