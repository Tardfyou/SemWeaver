/**
 * @name DivideByZeroUncheckedParsedDenominator
 * @description 检测在补丁相关作用域内，来自解析/状态字段的数值被用作除法或取模分母，但在到达危险运算前缺少零值检查及失败屏障（如 goto fail/return error）的模式。
 * @kind problem
 * @problem.severity warning
 * @precision medium
 * @id cpp/custom/divide_by_zero
 * @tags security
 *       correctness
 */

import cpp
import semmle.code.cpp.controlflow.Guards
import semmle.code.cpp.controlflow.Dominance

predicate inPatchScope(Element e) {
  e.getFile().getRelativePath().matches("%bfd/dwarf2.c") and
  exists(Function f | f = e.getEnclosingFunction() | any())
}

predicate isZeroLiteral(Expr e) {
  exists(Literal lit | lit = e and lit.getValueText() = "0")
}

predicate exprContainsFieldAccess(Expr root, FieldAccess access) {
  access = root or root.getAChild*() = access
}

predicate sameFieldValue(Expr left, Expr right) {
  left = right
  or
  exists(FieldAccess l, FieldAccess r |
    exprContainsFieldAccess(left, l) and
    exprContainsFieldAccess(right, r) and
    l.getTarget() = r.getTarget() and
    (
      not exists(l.getQualifier()) and not exists(r.getQualifier())
      or
      exists(Expr ql, Expr qr | ql = l.getQualifier() and qr = r.getQualifier() and ql = qr)
    )
  )
}

predicate isParsedStateDenominator(Expr e) {
  exists(FieldAccess fa |
    exprContainsFieldAccess(e, fa) and
    fa.getTarget().getName().matches("%line_range%")
  )
}

predicate isDangerousArithmetic(Expr arith, Expr denom) {
  (
    exists(DivExpr d |
      d = arith and
      denom = d.getRightOperand()
    )
    or
    exists(RemExpr r |
      r = arith and
      denom = r.getRightOperand()
    )
  ) and
  isParsedStateDenominator(denom)
}

predicate hasPatchGuard(Expr checked, Element anchor) {
  exists(IfStmt ifs, Expr cond, BasicBlock guardBb, BasicBlock anchorBb |
    ifs.getEnclosingFunction() = anchor.getEnclosingFunction() and
    cond = ifs.getCondition() and
    exprContainsFieldAccess(cond, any(FieldAccess fa | sameFieldValue(checked, fa))) and
    exists(EqualityOperation eq |
      eq = cond and
      (
        sameFieldValue(eq.getLeftOperand(), checked) and isZeroLiteral(eq.getRightOperand())
        or
        sameFieldValue(eq.getRightOperand(), checked) and isZeroLiteral(eq.getLeftOperand())
      )
    ) and
    guardBb = ifs.getThen().getBasicBlock() and
    anchorBb = anchor.getBasicBlock() and
    bbDominates(guardBb, anchorBb)
  )
}

predicate acceptedWithoutGuard(Expr source, Element anchor) {
  exists(Expr arith |
    anchor = arith and
    isDangerousArithmetic(arith, source)
  )
}

from Expr source, Expr anchor
where
  inPatchScope(anchor) and
  isParsedStateDenominator(source) and
  acceptedWithoutGuard(source, anchor) and
  not hasPatchGuard(source, anchor)
select anchor, "Parsed numeric state is used as a divisor or modulo denominator without a dominating zero-value failure guard."