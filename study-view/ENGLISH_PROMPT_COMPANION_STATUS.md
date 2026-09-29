# English prompt companions

The canonical main-only reading snapshot is `english-main-prompts-v2/`.
It translates runtime-feedback fragments with an explicit assistant-prepared
glossary and preserves original prompt/exchange digests. It makes no new
experiment-model call and does not change any prompt used in the experiments.

The earlier `english-main-prompts/` snapshot incorrectly called the glossary
human-authored. It is superseded, excluded from final export, and retained
locally as generation history rather than silently rewritten. The language
translations themselves are unchanged; only their provenance is corrected.
`build_study_prompt_companions.py` now prepares main, repeated/model, baseline,
CodeQL and illustrative prompt reading views. Its completed-records preflight
currently checks732 requests without untranslated Chinese fragments. It prefers
actual Anthropic request bodies over local deterministic-replay serializations,
and binds the two natural-stop baseline prompts to their original audited bytes.
Final output refuses incomplete auxiliary coverage; the all-study package is
not yet written or certified. Five translation/provenance tests pass, including
unknown-fragment refusal and preservation of structured text blocks.
