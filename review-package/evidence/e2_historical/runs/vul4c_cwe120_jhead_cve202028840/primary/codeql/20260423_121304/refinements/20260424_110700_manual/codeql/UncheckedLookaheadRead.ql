/**
 * @name UncheckedLookaheadRead
 * @description Detects parser logic that performs a CRLF-style lookahead read
 *              (`Data[a+1]`) while iterating with only `a < length`, unless the
 *              same condition also proves `a < length-1` before the next-byte
 *              access. The query intentionally models the surrounding parsing
 *              contract rather than matching a bare patch string.
 * @kind problem
 * @problem.severity warning
 * @precision medium
 * @id cpp/custom/buffer_overflow
 * @tags security
 *       correctness
 */

import cpp

/**
 * Patch-local semantic anchor:
 * `process_COM` parses JPEG comment payload bytes into a bounded comment buffer.
 * We keep the function anchor, but the rest of the query focuses on the parser
 * mechanics that make the lookahead read risky.
 */
class ProcessComFunction extends Function {
  ProcessComFunction() {
    this.getName() = "process_COM" and
    this.getFile().getBaseName() = "jpgfile.c"
  }
}

/**
 * The parser first caps `length` to the fixed comment-buffer size. This proves
 * the code is not a generic arbitrary array walk: it is a bounded parser loop
 * whose remaining risk is a mismatched relation between the loop bound and the
 * one-byte lookahead access.
 */
class CommentLengthNormalization extends IfStmt {
  CommentLengthNormalization() {
    this.getEnclosingFunction() instanceof ProcessComFunction and
    exists(string condText, string ifText |
      condText = this.getCondition().toString() and
      ifText = this.toString() and
      condText.regexpMatch(".*length\\s*>\\s*MAX_COMMENT_SIZE.*") and
      ifText.regexpMatch("(?s).*length\\s*=\\s*MAX_COMMENT_SIZE.*")
    )
  }
}

/**
 * The main byte scan loop advances `a` from the first payload bytes through the
 * remaining comment body while the loop condition only proves `a < length`.
 */
class CommentScanLoop extends ForStmt {
  CommentScanLoop() {
    this.getEnclosingFunction() instanceof ProcessComFunction and
    exists(string loopText |
      loopText = this.toString() and
      loopText.regexpMatch(
        "(?s).*for\\s*\\(\\s*a\\s*=\\s*2\\s*;\\s*a\\s*<\\s*length\\s*;\\s*a\\+\\+\\s*\\).*"
      )
    )
  }
}

predicate loopBoundOnlyProtectsCurrentIndex(CommentScanLoop loop) {
  exists(string condText |
    condText = loop.getCondition().toString() and
    condText.regexpMatch(".*a\\s*<\\s*length.*") and
    not condText.regexpMatch(".*a\\s*<\\s*length\\s*-\\s*1.*")
  )
}

/**
 * `ch = Data[a]` records the current byte before the CRLF-elision branch
 * performs a one-byte lookahead on the same logical index variable.
 */
predicate loadsCurrentByteFromCurrentIndex(ProcessComFunction f) {
  exists(Stmt s, string stmtText |
    s.getEnclosingFunction() = f and
    stmtText = s.toString() and
    stmtText.regexpMatch(".*ch\\s*=\\s*Data\\s*\\[\\s*a\\s*\\].*")
  )
}

/**
 * The function copies printable bytes into a bounded local comment buffer.
 * This helps distinguish a real parser-state loop from an arbitrary isolated
 * condition containing `Data[a+1]`.
 */
predicate appendsIntoCommentBuffer(ProcessComFunction f) {
  exists(Stmt s, string stmtText |
    s.getEnclosingFunction() = f and
    stmtText = s.toString() and
    stmtText.regexpMatch(".*Comment\\s*\\[\\s*nch\\s*\\+\\+\\s*\\]\\s*=.*")
  )
}

predicate exportsAccumulatedComment(ProcessComFunction f) {
  exists(Stmt s1, string stmtText1 |
    s1.getEnclosingFunction() = f and
    stmtText1 = s1.toString() and
    stmtText1.regexpMatch(".*printf\\s*\\(\\s*\"COM marker comment: %s\\\\n\"\\s*,\\s*Comment\\s*\\).*")
  ) and
  exists(Stmt s2, string stmtText2 |
    s2.getEnclosingFunction() = f and
    stmtText2 = s2.toString() and
    stmtText2.regexpMatch(".*strcpy\\s*\\(\\s*ImageInfo\\.Comments\\s*,\\s*Comment\\s*\\).*")
  )
}

/**
 * Vulnerable mechanism anchor:
 * the parser recognizes CRLF by reading the current byte (`ch`) and peeking at
 * the next byte (`Data[a+1]`), then skipping the pair with `continue`.
 */
class CrLfLookaheadSkip extends IfStmt {
  CrLfLookaheadSkip() {
    this.getEnclosingFunction() instanceof ProcessComFunction and
    exists(string condText |
      condText = this.getCondition().toString() and
      condText.regexpMatch(".*ch\\s*==.*") and
      condText.regexpMatch(".*Data\\s*\\[\\s*a\\s*\\+\\s*1\\s*\\].*")
    ) and
    exists(string ifText |
      ifText = this.toString() and
      ifText.regexpMatch("(?s).*continue\\s*;.*")
    )
  }
}

predicate checksCarriageReturn(CrLfLookaheadSkip ifs) {
  exists(string condText |
    condText = ifs.getCondition().toString() and
    condText.regexpMatch(".*ch\\s*==.*")
  )
}

predicate readsNextByteAsLineFeed(CrLfLookaheadSkip ifs) {
  exists(string condText |
    condText = ifs.getCondition().toString() and
    condText.regexpMatch(".*Data\\s*\\[\\s*a\\s*\\+\\s*1\\s*\\].*")
  )
}

predicate failsClosedByContinue(CrLfLookaheadSkip ifs) {
  exists(string ifText |
    ifText = ifs.toString() and
    ifText.regexpMatch("(?s).*continue\\s*;.*")
  )
}

/**
 * The patch-introduced barrier is semantically specific: the same conjunction
 * that performs the lookahead also proves that the current index is strictly
 * below `length-1`.
 */
predicate hasExplicitSameConditionBarrier(CrLfLookaheadSkip ifs) {
  exists(string condText |
    condText = ifs.getCondition().toString() and
    condText.regexpMatch(".*a\\s*<\\s*length\\s*-\\s*1.*")
  )
}

predicate barrierMentionsTheSameIndexRole(CrLfLookaheadSkip ifs) {
  exists(string condText |
    condText = ifs.getCondition().toString() and
    condText.regexpMatch(".*a\\s*<\\s*length\\s*-\\s*1.*") and
    condText.regexpMatch(".*Data\\s*\\[\\s*a\\s*\\+\\s*1\\s*\\].*")
  )
}

predicate enclosedByCommentScanLoop(CrLfLookaheadSkip ifs, CommentScanLoop loop) {
  loop = ifs.getEnclosingStmt().getEnclosingStmt*()
}

/**
 * Full parser contract:
 * 1. the function caps the input length to the comment-buffer capacity,
 * 2. iterates with `a < length`,
 * 3. loads the current byte from `Data[a]`,
 * 4. performs CRLF-specific lookahead on `Data[a+1]`,
 * 5. and stores the sanitized bytes into a bounded comment buffer.
 *
 * This is still patch-anchored, but it is no longer patch-only: the alert is
 * driven by the mismatch between the loop proof and the lookahead proof.
 */
predicate modelsParserStateAndByteRoles(CrLfLookaheadSkip ifs) {
  exists(ProcessComFunction f, CommentLengthNormalization norm, CommentScanLoop loop |
    f = ifs.getEnclosingFunction() and
    norm.getEnclosingFunction() = f and
    enclosedByCommentScanLoop(ifs, loop) and
    loopBoundOnlyProtectsCurrentIndex(loop) and
    loadsCurrentByteFromCurrentIndex(f) and
    appendsIntoCommentBuffer(f)
  )
}

/**
 * A report is emitted when the parser performs the CRLF lookahead inside the
 * comment loop but lacks the patch-style same-condition barrier that proves the
 * next byte exists on the active path.
 */
predicate missingLookaheadAvailabilityProof(CrLfLookaheadSkip ifs) {
  checksCarriageReturn(ifs) and
  readsNextByteAsLineFeed(ifs) and
  failsClosedByContinue(ifs) and
  modelsParserStateAndByteRoles(ifs) and
  not hasExplicitSameConditionBarrier(ifs)
}

from CrLfLookaheadSkip ifs
where
  missingLookaheadAvailabilityProof(ifs) and
  not barrierMentionsTheSameIndexRole(ifs)
select ifs.getCondition(),
  "Parser performs CRLF lookahead via Data[a+1] inside a comment-scanning loop that only proves a < length; no same-condition barrier proves a < length-1 before the next-byte read."
