Reference skeleton for a first CodeQL draft. It is provided only on the first
generation turn; adapt it to the actual mechanism.

```ql
/**
 * @name PatchGuidedChecker
 * @description Patch-local detector query generated from a security fix.
 * @kind problem
 * @problem.severity warning
 * @precision medium
 * @id cpp/custom/patch-guided-checker
 */

import cpp
import semmle.code.cpp.controlflow.Guards
import semmle.code.cpp.controlflow.Dominance

/**
 * Patch-guided query starter.
 * Stable APIs: SizeofExprOperator, getTarget(), getQualifier(),
 * bbDominates(...), GuardCondition.ensuresLt(...)
 * Do not replace them with invented APIs such as sizeof(...) or
 * ComparisonOperation.
 */

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

predicate hasPatchStyleGuard(FunctionCall call, Expr measured, Expr capacity) {
  exists(GuardCondition guard |
    guard.ensuresLt(measured, capacity, 0, call.getBasicBlock(), true)
    or
    guard.ensuresLt(measured, capacity, 1, call.getBasicBlock(), true)
  )
}

predicate patchGuidedCandidate(FunctionCall call) {
  // Declare dest, measured, and capacity in this same exists().
  exists(Expr dest, Expr measured, Expr capacity |
    dest = call.getArgument(0) and
    // Add the actual patch-evidenced relation here.
    not hasPatchStyleGuard(call, measured, capacity)
  ) and
  false // Replace with a real condition.
}

from FunctionCall call
where patchGuidedCandidate(call)
select call, "Patch-guided candidate: describe the issue."
```
