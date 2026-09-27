# Automatic CodeQL development repeats (one ImageMagick patch)

This is a **separate E2 development diagnostic**, not part of the 39 Linux
CSA/KNighter cases. All three runs start from the same archived,
model-generated CodeQL query for ImageMagick CVE-2017-12877. Its independent
CodeQL 2.26.4 no-build paired replay yields 0 vulnerable / 0 fixed rows.
No manually completed or historically refined query is used as an input.

An automatic GPT-6-Luna-high agent edits the query and independently replays
each changed version on frozen vulnerable and fixed databases. The three
input manifests are byte-identical; each run has its own output directory,
model exchanges, query versions, BQRS results, and hash-bound paired ledger.

| Repeat | Calls | Best vulnerable / fixed rows | Strict PDS? |
| --- | ---: | ---: | ---: |
| 1 | 1 | 8 / 0 | yes |
| 2 | 1 | 8 / 0 | yes |
| 3 | 8 | 22 / 22 | no |

Thus the same-input strict success frequency is **2/3**, not 3/3. Repeat 3
does recover a vulnerable-side hit, but 22 fixed-side rows are a substantial
noise burden and are not described as a usable checker or PDS. An
execution-invalid intermediate query in that run is retained as such, not
converted into a miss or success. The machine-readable recomputation is in
`SUMMARY.json`.

The successful edit corrects a root-relative file-path predicate. The
resulting query still names the target file, enclosing function, and local
variables. This establishes an automatic *patch-local* CodeQL improvement
and the operability of the cross-backend editing/replay interface, not
cross-function transfer, vulnerability-family coverage, or an effect caused
by analyzer-internal evidence. The prompt and quality warning were refined
on this development case before these repeats; this is not held-out
confirmation. A separate historical ImageMagick generated query was already
2/0 at the starting replay and is excluded from refinement-gain counting.
