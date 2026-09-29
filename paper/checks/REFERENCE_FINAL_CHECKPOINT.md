# Reference integrity: current manuscript, 2026-09-29

The initial integrity pass covered 64 bibliography entries and 51 cited keys.
After the requested citation additions, the current bibliography contains 69
entries and renders 59 cited references. The additions are detailed below;
the original audit counts in the following sections remain historical counts.
No missing citation key was found. The unused entries are not rendered by the
current BibTeX bibliography and are not claimed individually verified here.

## Follow-up: six more cited references and original AI heading

The preceding layout pass added LLift and RepoAudit (51 to 53 cited entries).
This follow-up adds six relevant citations (53 to 59); three were already in
the BibTeX database but had not been cited, and three are new entries.

| Key | Placement / purpose | Primary verification |
|---|---|---|
| `johnson2013why` | Background: report burden and adoption | [Google Research](https://research.google/pubs/why-dont-software-developers-use-static-analysis-tools-to-find-bugs/); publisher-deposited DOI metadata 10.1109/ICSE.2013.6606613, ICSE 2013, 672-681 |
| `heckman2011alerts` | Background: actionable-alert research | [Authors' institutional manuscript](https://repository.lib.ncsu.edu/bitstreams/fa707e19-8ce5-4ca6-b459-b6bdbf72db61/download); DOI metadata confirms IST 53(4), 363-387, 2011 |
| `kremenek2002zranking` | Background: ranking emitted warnings | [Authors' paper](https://web.stanford.edu/~engler/sas-camera-ready.pdf); DOI 10.1007/3-540-44898-5_16 confirms SAS **2003**, 295-315; historical key spelling is retained |
| `ruthruff2008actionable` | Background: predicting accuracy/actionability | [Institutional author record](https://digitalcommons.unl.edu/cseconfwork/128/); DOI 10.1145/1368088.1368135 confirms authors, ICSE 2008 and 341-350 |
| `wang2024llmdfa` | Related Work: compilation-free dataflow analysis and synthesized tools | [NeurIPS proceedings and linked BibTeX](https://proceedings.neurips.cc/paper_files/paper/2024/hash/ed9dcde1eb9c597f68c1d375bbecf3fc-Abstract-Conference.html), volume 37, 131545-131574, 2024 |
| `serebryany2012asan` | Motivating example: tool already used for fixture checks | [USENIX paper entry and BibTeX](https://www.usenix.org/conference/atc12/technical-sessions/presentation/serebryany), ATC 2012, 309-318 |

The new sentences distinguish emitted-warning prioritization and code analysis
from modifying an existing checker. No cross-paper performance claim or new
experimental result was introduced. The overview sentence and duplicated patch
explanation in Background were compressed with the same semantic boundary.
The original `Acknowledgments` heading and AI-use sentence were both checked
verbatim against the preserved first manuscript. No identity or funding text
was inserted. References start on page 19, occupy four pages, and retain ACM
typography; bibliography entries are kept together to avoid isolated DOI lines.

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
