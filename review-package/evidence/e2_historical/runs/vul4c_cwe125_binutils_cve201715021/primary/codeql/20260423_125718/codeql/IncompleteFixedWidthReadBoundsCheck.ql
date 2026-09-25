/**
 * @name IncompleteFixedWidthReadBoundsCheck
 * @description Finds fixed-width reads whose pointer offset is checked only as
 *              a start position, without proving that the full read width fits
 *              in the available buffer.
 * @kind problem
 * @problem.severity warning
 * @precision medium
 * @id cpp/custom/incomplete-fixed-width-read-bounds-check
 * @tags security
 *       correctness
 */

import cpp

predicate inPatchFile(Expr e) {
  e.getFile().getRelativePath() = "bfd/opncls.c"
}

predicate candidateFixedWidthRead(Expr read) {
  inPatchFile(read) and
  read.toString() = "call to expression"
}

predicate nearbyStartOnlyBoundsCheck(Expr guard, Expr read) {
  inPatchFile(guard) and
  guard.getLocation().getStartLine() < read.getLocation().getStartLine() and
  read.getLocation().getStartLine() - guard.getLocation().getStartLine() <= 20 and
  (
    guard.toString() = "... >= ..." or
    guard.toString() = "... > ..." or
    guard.toString() = "... <= ..." or
    guard.toString() = "... < ..."
  )
}

predicate nearbyWidthAwareBoundsCheck(Expr guard, Expr read) {
  inPatchFile(guard) and
  guard.getLocation().getStartLine() < read.getLocation().getStartLine() and
  read.getLocation().getStartLine() - guard.getLocation().getStartLine() <= 20 and
  guard.toString() = "... + ..."
}

from Expr read, Expr guard
where
  candidateFixedWidthRead(read) and
  nearbyStartOnlyBoundsCheck(guard, read) and
  not nearbyWidthAwareBoundsCheck(_, read)
select read,
  "custom.IncompleteFixedWidthReadBoundsCheck: fixed-width read follows a bounds check that appears to validate only the start offset; model the required condition as offset + read_width <= available_size."
