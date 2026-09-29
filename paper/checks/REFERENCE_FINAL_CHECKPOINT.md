# Reference integrity: current manuscript, 2026-09-29

The bibliography contains 64 entries; 51 keys are cited in the section sources.
No missing citation key was found. The unused entries are not rendered by the
current BibTeX bibliography and are not claimed individually verified here.

## Publisher-deposited DOI metadata

The retained `REFERENCE_DOI_METADATA_CHECK.json` and
`REFERENCE_DOI_METADATA_RECHECK.json` in the experiment phase cover 37 used
DOI-bearing entries. Thirty-three match title, year and applicable pages after
including publisher subtitles. Primary sources resolve the four exceptions:

- QL: the [Dagstuhl publisher entry](https://drops.dagstuhl.de/entities/document/10.4230/LIPIcs.ECOOP.2016.2)
  confirms authors, title, ECOOP2016 and pages2:1--2:25, including the publisher's
  own `Jones, Michael Peyton` name serialization.
- Codex: the [arXiv entry](https://arxiv.org/abs/2107.03374) confirms title,2021
  and the listed authors; the bibliography explicitly abbreviates the remainder
  with `others`. Failure of Crossref retrieval is not absence of the publication.
- SLAM: the originating [Microsoft project bibliography](https://www.microsoft.com/en-us/research/project/slam/)
  supplies the complete title and POPL2002 pages1--3 alongside the retained
  publisher identity metadata.
- Pixy: the author's [institutional publication record](https://informatics.tuwien.ac.at/people/engin-kirda)
  supports258--263; the Crossref string `6 pp.-263` is not used as a page range.

The current bibliography's author family names and order were compared with
all35 available publisher author lists. This exposed an omitted René Rydhof
Hansen in the Coccinelle/EuroSys2008 entry; that author has been restored.
All35 family/order checks now match. This check is not a byte-exact comparison
of initials, accents or every metadata field.

## Fourteen used entries without a DOI field

Primary identity/author/year support was checked at these sources:

| Key | Primary source |
|---|---|
| `chen2026vulgenie` | https://www.usenix.org/conference/usenixsecurity26/presentation/chen-bofei |
| `li2025iris` | https://proceedings.iclr.cc/paper_files/paper/2025/hash/582d4e27fa24168f3af1f4582655034b-Abstract-Conference.html |
| `du2025llm4pfa` | https://arxiv.org/abs/2506.10322 |
| `iranmanesh2025zerofalse` | https://arxiv.org/abs/2510.02534 |
| `madaan2023selfrefine` | https://proceedings.neurips.cc/paper_files/paper/2023/hash/91edff07232fb1b55a505a9e9f6c0ff3-Abstract-Conference.html |
| `shinn2023reflexion` | https://proceedings.neurips.cc/paper_files/paper/2023/hash/1b44b878bb782e6954cd888628510e90-Abstract-Conference.html |
| `schick2023toolformer` | https://proceedings.neurips.cc/paper_files/paper/2023/hash/d842425e4bf79ba039352da0f658a906-Abstract-Conference.html |
| `yao2023tree` | https://proceedings.neurips.cc/paper_files/paper/2023/hash/271db9922b8d1f4dd7aaef84ed5ac703-Abstract-Conference.html |
| `yao2023react` | https://react-lm.github.io/ and its official linked paper/repository |
| `cadar2008klee` | https://www.usenix.org/conference/osdi-08/klee-unassisted-and-automatic-generation-high-coverage-tests-complex-systems |
| `livshits2005finding` | https://www.usenix.org/conference/14th-usenix-security-symposium/finding-security-vulnerabilities-java-applications-static ; pages271--286 at https://www.usenix.org/legacy/event/sec05/tech/livshits.html |
| `clangsaDocs` | https://clang-analyzer.llvm.org/ |
| `codeqlDocs` | https://codeql.github.com/docs/ |
| `linuxCommit97cba232` | Locally inspected upstream Git commit97cba232549b9fe7e491fb60a69cf93075015f29; Srinivasan Shanmugam,2024-01-29, matching patch subject |

The VulGenie publisher entry directly supplies1847--1866 and Baltimore,MD;
the latter field was added. The four NeurIPS paper PDFs were checked during
the preceding reference pass; the current pass rechecked their proceedings
identity/author lists. Their abstract entries alone are not represented as
proof of page ranges. OpenReview's browser challenge prevented direct ReAct
entry access; the author's project/linked official repository provided the
alternative identity source. Transient failures of two USENIX PDF opens were
resolved for identity using the official presentation/proceedings pages, not
treated as missing publications.

## Claim correspondence and remaining release checks

The related-work descriptions distinguish analyzer context, taint-specification
inference, report adjudication, attack/defense rule synthesis, and editing an
existing checker. This is a bounded editorial correspondence check, not proof
of novel mechanisms or exhaustive literature saturation. No extra experiment
or reference was introduced by this integrity pass. Final post-polish BibTeX
compilation, citation linkage, anonymous PDF/URL checks and rendered reference
inspection remain release checks.
