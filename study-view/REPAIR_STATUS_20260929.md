# Interrupted execution recovery

All original positive/negative model outputs and failure records remain intact.
No failed execution is a zero-effect measurement.

## G23 observer failure before model invocation

The original checker compiles. Its disposable observer copy failed because a
local `SymbolRef S` shadowed a `const Stmt *S` callback parameter, so an injected
pre-return emitter received the wrong C++ type. This is an observer defect, not
a generated-checker outcome.

`fixed_trace_probe.py` captures the callback statement in a unique entry-scope
variable. Only diagnostic copies change; no scored checker is manually edited.
`repaired_treatment_entry.py` routes probe subprocesses through this isolated
fix without modifying frozen v43 files. `run_observer_repaired_cell.py` retains
the selected runner and its existing verified contextless-coverage bridge.
`launch_observer_repair.py` preserves prefixes and response budgets and binds
all added wrapper hashes in the repair plan.

`repairs/22-native/continuation-01/checker_execution_probe/RESULT.json` completed
with diagnostic parity. The four original/traced vulnerable/fixed scans passed.
Its budget ledger then recorded the first real model dispatch. This confirms
the former pre-model exit was overcome, not that the whole cell has finished.
The first actual Luna response was subsequently preserved in
`continuation-01/llm_exchanges.jsonl`; its treatment manifest records one model
response, success, and candidate SHA256
`8e0e4c1fd47e210e12ad2d6d1d25a6adb48f60b4358e4cd4455da667ac56e098`.
Independent paired validation is underway; treatment success is not yet a
claimed vulnerable/fixed improvement.
Eight regression/stopping-policy tests passed (`test_observer_repair.py` plus
`test_profile_cell.py`).

## Parent-launcher termination

No SemWeaver containers or host queue processes were live at recovery start.
Five actual completed cells lacked the outer receipt: main 21-native,
21-no_internal, 23-native; E3 repeat-2 native G08; E4 Flash repeat-1 G02.
The strict existing result/input/output/call audit recovered these receipts.
The missing launcher exit code is `null`, never invented as zero.
KNighter G17's completed twenty-response ledger and summary were independently
verified and its missing parent receipt recovered; paired replay is still
required. Upstream per-call triage requests .01 while refinement requests 0;
the raw entry settings are preserved rather than rewritten.

`resume_verified_queues.py` resumed only absent directories using the original
frozen launcher loops. Existing results, replies and partial failures are
skipped, not overwritten. Main, E3, E4, baseline and three watchers run in
detached host processes with separate `queue-restart-20260929-*.log` logs.

## Still requires follow-up

- G23: inspect actual returned response, paired validation, subsequent attempts,
  and final outer receipt before claiming a completed result.
- G11 main native: fifth response generated a checker whose
  `dyn_cast<FunctionDecl>(Call.getDecl())` asserts on null calls. The crash stack
  binds `isPublishCall` in the exact candidate plugin. Four earlier responses
  were checkpointed; the fifth response must not be forgotten or resampled.
  This needs a model-produced crash repair and normal paired validation, not a
  manually edited scored checker and not an inferred zero warning count.
- Watch remaining queues for any new pre-model failures. Main G04's wrong-scope
  records remain superseded by the completed ARM64-bound replay.
- Original contextless failures already have completed designated repairs.
- Do not publish partial aggregate improvements, overwrite frozen scripts, or
  push manuscript/artifact while experiments remain unfinished.
