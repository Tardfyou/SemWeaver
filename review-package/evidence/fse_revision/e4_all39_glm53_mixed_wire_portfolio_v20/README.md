# GLM-5.3 full 39-case descriptive portfolio

`RESULT.json` recomputes the same frozen 39 patch pairs from the independent
starting screen and hash-bound paired candidate replays. Only candidates with
an execution-valid vulnerable-side hit and zero fixed-side alerts are adopted;
other cases retain their starting checker. The resulting patch-version F1 is
0.580 (20 TP, 10 FP, 19 FN) versus 0.418 at the common starting screen;
10/39 cases reach the strict paired discriminator.

This is **not a homogeneous wire-protocol model comparison**. The 14
fixed-noisy cases used the original GLM-5.3 Chat Completions condition with
65,536 output tokens per call. The 25 target-miss cases used the same model
on the Anthropic-compatible endpoint with a 131,072-token limit after the
standard endpoint's resource package was unavailable. Recovery is itself
split into a completed five-case prefix and an exact 20-case suffix after a
provider-format interruption. Both prefixes and suffixes, implementation
revisions, wire APIs, caps, file hashes, and per-case outcomes are recorded
in `RESULT.json`. No interrupted response or negative analyzer sentinel is
scored as a detection result.
