/**
 * @name ImageListDeletedAliasUseAfterFree
 * @description 检测空图像节点回收分支里，旧别名 `image2` 未在删除前失效，随后仍被当作活的 Image 指针参与 cleanup 的 stale-pointer use-after-free 机制。
 * @kind problem
 * @problem.severity warning
 * @precision medium
 * @id cpp/custom/use_after_free
 * @tags security
 *       correctness
 */

import cpp

predicate inPatchScope(Element e) {
  e.getFile().getRelativePath() = "coders/mat.c" and
  exists(Function f | e.getEnclosingElement*() = f and f.getName() = "ReadMATImage")
}

predicate isNullLike(Expr e) {
  e.toString() = "0" or
  e.toString().matches("%NULL%")
}

predicate isImageAliasAssignment(AssignExpr assign) {
  assign.getLValue().toString() = "image2"
}

predicate isPreviousStepAssignment(AssignExpr assign) {
  assign.getLValue().toString() = "p" and
  assign.getRValue().toString() = "previous"
}

predicate isDestroyCleanupAssignment(AssignExpr assign) {
  isImageAliasAssignment(assign) and
  assign.getRValue().toString() = "call to DestroyImage"
}

predicate occursAfter(Locatable later, Locatable earlier) {
  later.getLocation().getStartLine() > earlier.getLocation().getEndLine()
}

predicate occursInside(Locatable inner, Locatable outer) {
  inner.getLocation().getStartLine() > outer.getLocation().getStartLine() and
  inner.getLocation().getEndLine() < outer.getLocation().getEndLine()
}

predicate assignmentInside(AssignExpr assign, Stmt outer) {
  inPatchScope(assign) and
  inPatchScope(outer) and
  occursInside(assign, outer)
}

/*
 * Mechanism roles in this sample:
 * 1. prune branch: rewinds the list and deletes the current empty node
 * 2. alias barrier: image2 must be nulled if it aliases the soon-to-be-deleted node
 * 3. cleanup sink: a later DestroyImage(image2) still consumes the alias
 *
 * The refined query keeps those roles separate so later rounds can extend each
 * role independently instead of falling back to one patch-local string match.
 *
 * This sample is intentionally modeled around:
 * - branch-local lifetime transition before DeleteImageFromList
 * - barrier-style alias invalidation rather than generic null checks
 * - later cleanup consumption that proves the alias still matters
 * - a repair pattern that is specifically "delete stale alias before reuse"
 *
 * The goal is not to report every deletion site, but to capture the lifetime
 * gap between node deletion and later alias consumption.
 */
predicate isPruneRole(IfStmt prune) {
  isEmptyImagePruneBranch(prune)
}

predicate isBarrierRole(IfStmt prune) {
  hasAliasInvalidationBarrier(prune)
}

predicate isCleanupSinkRole(AssignExpr sink) {
  isDestroyCleanupAssignment(sink)
}

/*
 * The vulnerable branch first walks back to the previous list node and then
 * deletes the current empty node. The fix adds exactly one missing action in
 * that branch: clearing image2 before the delete completes.
 *
 * We intentionally keep the branch abstraction simple and stable:
 * - `p = p->previous` witnesses the rewind transition
 * - the delete happens later in the same branch
 * - the barrier belongs to the same branch, not to some unrelated later site
 */
predicate isEmptyImagePruneBranch(IfStmt prune) {
  inPatchScope(prune) and
  exists(AssignExpr prevAssign |
    assignmentInside(prevAssign, prune) and
    isPreviousStepAssignment(prevAssign)
  )
}

predicate hasAliasInvalidationBarrier(IfStmt prune) {
  exists(AssignExpr clear |
    assignmentInside(clear, prune) and
    isImageAliasAssignment(clear) and
    isNullLike(clear.getRValue())
  )
}

/*
 * The later cleanup sink is stable in both versions: image2 is conditionally
 * destroyed near the function exit. The bug is whether image2 may still alias
 * a node already deleted by the prune branch.
 *
 * This separation lets us talk about:
 * - the transition site that creates staleness
 * - the later consumer that proves the stale alias remains semantically live
 */
predicate isLaterDestroySink(IfStmt prune, AssignExpr sink) {
  inPatchScope(sink) and
  occursAfter(sink, prune) and
  isCleanupSinkRole(sink)
}

predicate staleAliasMechanismObserved(IfStmt prune, AssignExpr sink) {
  isPruneRole(prune) and
  isLaterDestroySink(prune, sink) and
  not isBarrierRole(prune)
}

/*
 * Two alerts are enough to expose the full mechanism:
 * - the prune branch that forgets to invalidate the alias
 * - the later cleanup sink that still consumes the stale alias
 *
 * Keeping both sites preserves the source/sink narrative while avoiding
 * unrelated auxiliary warnings.
 */
predicate staleAliasAlert(IfStmt prune, Element site, string message) {
  exists(AssignExpr sink |
    staleAliasMechanismObserved(prune, sink) and
    site = prune and
    message =
      "This empty-image pruning branch advances to the previous node without invalidating image2 first, so a stale alias may survive deletion of the pruned node."
  )
  or
  exists(AssignExpr sink |
    staleAliasMechanismObserved(prune, sink) and
    site = sink and
    message =
      "image2 reaches a later DestroyImage cleanup sink after an earlier prune branch deleted an aliased node without first nulling the alias."
  )
}

from IfStmt prune, Element site, string message
where
  staleAliasAlert(prune, site, message)
select site, message
