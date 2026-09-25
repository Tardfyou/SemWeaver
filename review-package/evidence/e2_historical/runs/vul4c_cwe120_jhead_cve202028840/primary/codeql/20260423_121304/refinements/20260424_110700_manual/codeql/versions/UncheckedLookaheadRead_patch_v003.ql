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
predicate inLookaheadContract(IfStmt ifs) {
  ifs.getEnclosingFunction().getName() = "process_COM"
}

predicate isLookaheadCondition(IfStmt ifs) {
  exists(string condText |
    condText = ifs.getCondition().toString() and
    condText.regexpMatch(".*\\[[^\\]]*\\+\\s*1\\].*")
  )
}

predicate hasLoopUpperBound(IfStmt ifs) {
  exists(ForStmt fs, string loopText |
    fs = ifs.getEnclosingStmt().getEnclosingStmt*() and
    loopText = fs.getCondition().toString() and
    loopText.regexpMatch(".*<\\s*(length|len|size).*")
  )
}

predicate hasSameConditionPatchBarrier(IfStmt ifs) {
  exists(string condText |
    condText = ifs.getCondition().toString() and
    condText.regexpMatch(".*<\\s*(length|len|size)\\s*-\\s*1.*")
  )
}

from IfStmt ifs
where
  inLookaheadContract(ifs) and
  isLookaheadCondition(ifs) and
  hasLoopUpperBound(ifs) and
  not hasSameConditionPatchBarrier(ifs)
select ifs.getCondition(),
  "Lookahead read uses index + 1 inside a loop bounded only by index < length without an explicit same-condition barrier proving index + 1 stays in bounds."
