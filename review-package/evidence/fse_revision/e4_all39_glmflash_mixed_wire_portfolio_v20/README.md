# GLM-5.3-Flash full 39-case descriptive portfolio

`RESULT.json` recomputes the frozen 39 patch pairs from the independent
starting screen and hash-bound paired candidate replays. Only candidates
with an execution-valid vulnerable-side hit and zero fixed-side alerts are
adopted; all other cases retain their starting checker. The resulting
patch-version F1 is 0.563 (20 TP, 12 FP, 19 FN), versus 0.418 at the common
starting screen. Eight of 39 cases reach the strict paired discriminator.

The model identity is constant, but this is **not a homogeneous wire-protocol
model comparison**. The 14 fixed-noisy cases form exact 9+2+3 segments: the
first nine used Chat Completions with a 65,536-token output cap, and the
remaining five used the Anthropic-compatible endpoint with a 131,072-token
cap after a provider output-limit interruption. The 25 target-miss cases
form exact 6+19 Anthropic-compatible segments after a provider-format
interruption. The JSON records each segment's implementation revision,
wire API, output cap, file hashes, and per-case outcome. No interrupted
response or negative analyzer sentinel is scored as a detection result.

Valid but unadopted partial effects are separate from strict success. For
example, G31 retained a vulnerable-side hit while reducing fixed-side
alerts from two to one; it remains a fixed-noisy case under the strict
adoption rule.
