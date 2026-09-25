/**
 * @name UncheckedLookaheadRead
 * @description Detects unchecked one-byte lookahead reads such as buffer[i+1] in parsing loops when iteration can reach the last valid index and no preceding guard constrains the index below length-1 before the access.
 * @kind problem
 * @problem.severity warning
 * @precision medium
 * @id cpp/custom/buffer_overflow
 * @tags security
 *       correctness
 */

import cpp
import semmle.code.cpp.controlflow.Guards
import semmle.code.cpp.controlflow.Dominance

/**
 * Detect unchecked one-byte lookahead reads such as buf[i+1] in loops whose
 * bound still allows i == len-1, unless a dominating patch-style guard rejects
 * the last index before the access.
 */

predicate inPatchScope(Element e) {
  e.getFile().getBaseName() = "jpgfile.c"
}

predicate exprContainsVariableAccess(Expr root, VariableAccess access) {
  access = root or root.getAChild*() = access
}

predicate exprContainsExpr(Expr root, Expr inner) {
  inner = root or root.getAChild*() = inner
}

predicate sameValueExpr(Expr left, Expr right) {
  left = right
  or
  exists(VariableAccess l, VariableAccess r |
    exprContainsVariableAccess(left, l) and
    exprContainsVariableAccess(right, r) and
    l.getTarget() = r.getTarget()
  )
}

predicate isZeroOrOne(IntLiteral lit) {
  lit.getValue() = 0 or lit.getValue() = 1
}

predicate isPlusOneOffset(Expr offset, Variable v) {
  exists(AddExpr add, VariableAccess va, IntLiteral one |
    offset = add and
    va.getTarget() = v and
    one.getValue() = 1 and
    (
      add.getLeftOperand() = va and add.getRightOperand() = one
      or
      add.getRightOperand() = va and add.getLeftOperand() = one
    )
  )
}

predicate isLookaheadRead(ArrayExpr arr, Expr base, Variable indexVar) {
  arr.getArrayBase() = base and
  isPlusOneOffset(arr.getArrayOffset(), indexVar)
}

predicate isLoopIndexBoundCandidate(LoopStmt loop, Variable indexVar, Expr lengthExpr) {
  exists(VariableAccess idxUse |
    idxUse.getTarget() = indexVar and
    exprContainsExpr(loop.getControllingExpr(), idxUse) and
    exprContainsExpr(loop.getControllingExpr(), lengthExpr)
  )
}

predicate hasPatchGuard(ArrayExpr arr, Variable indexVar, Expr lengthExpr) {
  exists(GuardCondition guard, VariableAccess idxUse |
    idxUse.getTarget() = indexVar and
    exprContainsExpr(lengthExpr, lengthExpr) and
    guard.ensuresLt(idxUse, lengthExpr, 1, arr.getBasicBlock(), true)
  )
}

predicate acceptedWithoutGuard(ArrayExpr arr, Expr base, Variable indexVar, Expr lengthExpr) {
  exists(LoopStmt loop |
    loop = arr.getEnclosingStmt().getEnclosingStmt*() and
    isLoopIndexBoundCandidate(loop, indexVar, lengthExpr)
  ) and
  not hasPatchGuard(arr, indexVar, lengthExpr)
}

predicate isTrigger(ArrayExpr arr, Expr base, Variable indexVar) {
  isLookaheadRead(arr, base, indexVar)
}

from ArrayExpr arr, Expr base, Variable indexVar, Expr lengthExpr
where
  inPatchScope(arr) and
  isTrigger(arr, base, indexVar) and
  acceptedWithoutGuard(arr, base, indexVar, lengthExpr)
select arr, "Unchecked one-byte lookahead read may access past the end of the buffer when the loop still allows the last valid index and no patch-style upper-bound guard excludes index+1."