# Evidence origins and availability

Analyzer-internal evidence means observations obtained from an analyzer's
intermediate analysis representations or checker execution state, rather than
inferred solely from source text or from the presence/absence of a warning.
Records carrying these observations may attach source context; that context is
not itself analyzer-internal evidence.

## Initial CSA bundles

The corrected 39-subject input catalog contains 193 records:

| Recorded origin | Records | Meaning |
|---|---:|---|
| Analyzer-internal | 81 | Raw-backed intermediate-analysis observations |
| Analyzer output | 51 | Analysis/validation outputs, distinct from internal state |
| Source-derived | 61 | Facts derived from the patch/source, not internal observations |

The 81 native-backed records comprise 45 path-guard, 27 state-transition and
9 allocation-lifecycle records. The interface is
`clang-18:debug.DumpCFG+debug.DumpCallGraph`, with parsed schema
`semweaver.csa_cfg_snapshot.v1`. Each native-backed record binds its raw output
digest and patch-related file/function scope. CFG assignments and branches are
not full symbolic ProgramState transitions or proofs of the target bug.

All 61 source-derived records have the provenance artifact prefix `patch-diff`.
They must not be relabeled collectively as source-window fallbacks. Native-backed
records can also include source-window context; record-origin counts are not
counts of exclusively native fields. `INITIAL_EVIDENCE_CATALOG.json` records
the actual bundles, types and raw bindings, including the corrected ARM64 case.

## Dynamic observations during refinement

Disposable checker instrumentation uses `semweaver.checker_trace.v2` JSONL,
summarized as `semweaver.trace_feedback.v1`. Supported observations include
callback/helper entry and return sites, outer/peeled AST kinds, symbolic values
and constraints, existing ProgramState map reads/writes, and supported name
predicates. State operations are evaluated once; instrumentation does not add
scored checker transitions. State/frame identifiers establish observed
associations, not an exhaustive runtime path or reaching-definition proof.

Four original/instrumented paired scans verify diagnostic parity. Only complete,
uncapped, hash-bound observations can become dynamic feedback. Compilation
errors, unsupported context sites, zero scoped callbacks and capped traces are
recorded as unavailable, not empty evidence or program safety. Static evidence
may remain available in those cases. Dynamic availability is counted per
refinement attempt in `NATIVE_EVIDENCE_AVAILABILITY_STATUS.json`; it has a
different denominator from the initial record catalog.
