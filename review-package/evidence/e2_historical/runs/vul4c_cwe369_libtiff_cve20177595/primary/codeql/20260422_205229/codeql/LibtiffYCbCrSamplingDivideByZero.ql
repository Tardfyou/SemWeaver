/**
 * @name LibtiffYCbCrSamplingDivideByZero
 * @description 检测在 libtiff 的 YCbCr/JPEG 设置路径中，来自 `td->td_ycbcrsubsampling` 的采样值赋给状态字段后，未经支配性的非零校验即流入后续除法或取模等分母位置的缺陷模式；若存在补丁同类的零值检查并提前返回，则应静默。
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

predicate inPatchScope(Element e) {
  e.getFile().getRelativePath().matches("%tif_jpeg.c")
}

predicate isSamplingFieldRead(Expr e) {
  exists(ArrayExpr arr, FieldAccess fa, Literal idx |
    e = arr and
    arr.getArrayBase() = fa and
    fa.getTarget().hasName("td_ycbcrsubsampling") and
    idx = arr.getArrayOffset() and
    (idx.toString() = "0" or idx.toString() = "1")
  )
}

predicate sameValueExpr(Expr left, Expr right) {
  left = right
  or
  exists(VariableAccess l, VariableAccess r |
    l = left and
    r = right and
    l.getTarget() = r.getTarget()
  )
  or
  exists(FieldAccess l, FieldAccess r |
    l = left and
    r = right and
    l.getTarget() = r.getTarget() and
    sameValueExpr(l.getQualifier(), r.getQualifier())
  )
}

predicate assignedFromSampling(Variable v) {
  exists(AssignExpr assign |
    inPatchScope(assign) and
    assign.getLValue() instanceof VariableAccess and
    assign.getLValue().(VariableAccess).getTarget() = v and
    isSamplingFieldRead(assign.getRValue())
  )
}

predicate assignedFromSamplingField(Field f) {
  exists(AssignExpr assign |
    inPatchScope(assign) and
    assign.getLValue() instanceof FieldAccess and
    assign.getLValue().(FieldAccess).getTarget() = f and
    isSamplingFieldRead(assign.getRValue())
  )
}

predicate isSamplingDerivedExpr(Expr e) {
  exists(VariableAccess va |
    va = e and
    assignedFromSampling(va.getTarget())
  )
  or
  exists(FieldAccess fa |
    fa = e and
    assignedFromSamplingField(fa.getTarget())
  )
}

predicate isZeroLiteral(Expr e) {
  exists(Literal i |
    i = e and i.toString() = "0"
  )
}

predicate zeroCheckExpr(Expr checked, Expr cond) {
  exists(EqualityOperation eq |
    eq = cond and
    (
      sameValueExpr(eq.getLeftOperand(), checked) and isZeroLiteral(eq.getRightOperand())
      or
      sameValueExpr(eq.getRightOperand(), checked) and isZeroLiteral(eq.getLeftOperand())
    )
  )
}

predicate hasPatchGuard(Expr checked, Expr sink) {
  exists(IfStmt ifs, ReturnStmt ret, BasicBlock guardBb, BasicBlock sinkBb |
    inPatchScope(ifs) and
    ret = ifs.getThen().getAChild*() and
    zeroCheckExpr(checked, ifs.getCondition()) and
    guardBb = ifs.getBasicBlock() and
    sinkBb = sink.getBasicBlock() and
    dominates(guardBb, sinkBb)
  )
}

predicate isDivOrModSink(Expr checked, Expr sink) {
  exists(DivExpr div |
    sink = div and
    sameValueExpr(div.getRightOperand(), checked)
  )
  or
  exists(RemExpr rem |
    sink = rem and
    sameValueExpr(rem.getRightOperand(), checked)
  )
}

from Expr source, Expr sink
where inPatchScope(sink)
  and isSamplingDerivedExpr(source)
  and isDivOrModSink(source, sink)
  and not hasPatchGuard(source, sink)
select sink, "YCbCr subsampling value derived from file metadata reaches a division or modulo operation without a dominating non-zero guard."