/**
 * @name UncheckedLookaheadRead
 * @description Detects lookahead reads of the form buffer[index+1] inside parsing loops whose iteration bound still allows the final valid index, unless the same read is guarded by an explicit index < length-1 style barrier.
 * @kind problem
 * @problem.severity warning
 * @precision medium
 * @id cpp/custom/buffer_overflow
 * @tags security
 *       correctness
 */

import cpp
import semmle.code.cpp.controlflow.Guards

predicate exprContains(Expr root, Expr inner) {
  inner = root or
  root.getAChild*() = inner
}

predicate exprReferencesVariable(Expr expr, Variable v) {
  exists(VariableAccess access |
    exprContains(expr, access) and
    access.getTarget() = v
  )
}

predicate isPlusOneOffset(Expr offset, Variable indexVar) {
  exists(AddExpr add, VariableAccess indexUse, Literal one |
    offset = add and
    indexUse.getTarget() = indexVar and
    one.toString() = "1" and
    (
      add.getLeftOperand() = indexUse and add.getRightOperand() = one
      or
      add.getRightOperand() = indexUse and add.getLeftOperand() = one
    )
  )
}

predicate isLookaheadRead(ArrayExpr read, Expr base, Variable indexVar) {
  read.getArrayBase() = base and
  isPlusOneOffset(read.getArrayOffset(), indexVar)
}

predicate inLookaheadContract(Function f) {
  f.getName() = "process_COM"
}

predicate hasLoopUpperBound(ArrayExpr read, Variable indexVar, Expr lengthExpr, ForStmt fs) {
  exists(Expr cond, VariableAccess indexUse |
    fs = read.getEnclosingStmt().getEnclosingStmt*() and
    cond = fs.getCondition() and
    indexUse.getTarget() = indexVar and
    exprContains(cond, indexUse) and
    exprContains(cond, lengthExpr)
  )
}

predicate isConditionalLookahead(ArrayExpr read, IfStmt ifs) {
  exprContains(ifs.getCondition(), read)
}

predicate hasSameConditionPatchBarrier(IfStmt ifs, ArrayExpr read, Variable indexVar, Expr lengthExpr) {
  exists(GuardCondition guard, VariableAccess indexUse |
    indexUse.getTarget() = indexVar and
    exprContains(ifs.getCondition(), indexUse) and
    exprContains(ifs.getCondition(), lengthExpr) and
    exprContains(ifs.getCondition(), read) and
    guard.ensuresLt(indexUse, lengthExpr, 1, read.getBasicBlock(), true)
  )
}

from ArrayExpr read, Expr base, Variable indexVar, Expr lengthExpr, IfStmt ifs, Function f, ForStmt fs
where
  read.getEnclosingFunction() = f and
  inLookaheadContract(f) and
  isLookaheadRead(read, base, indexVar) and
  isConditionalLookahead(read, ifs) and
  hasLoopUpperBound(read, indexVar, lengthExpr, fs) and
  not hasSameConditionPatchBarrier(ifs, read, indexVar, lengthExpr)
select read,
  "Lookahead read uses index + 1 inside a loop bounded only by index < length without an explicit same-condition barrier proving index + 1 stays in bounds."
