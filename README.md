# Checker-refinement review snapshot

This repository contains the final manuscript, readable study inputs and English
prompt views, and the complete recorded-evidence package in a lossless transport.

## Browse

- [Paper PDF](paper/main.pdf) and [LaTeX](paper/main.tex).
- [Original editable author-slide subset (before Figures 1 and 2 were corrected)](paper/figures/manual/semweaver-figures.pptx)
  and [current author-exported source PDF](paper/figures/manual/source-slides.pdf).
- [Current exact paper archive](paper-current.tar.gz),
  [layout change note](paper/checks/LAYOUT_REFRESH.md) and
  [current figure sources and export instructions](paper/figures/manual/README.md).
- [All39 starting checkers and patches](inputs/).
- [English prompt pages](prompts/README.md).
- [Illustrative versus main example checkers](example/).
- [Main results and tables](study-view/main39-tables/).
- [Canonical main summary](study-view/FINAL39_SUMMARY.json) and
  [auxiliary summary](study-view/AUXILIARY_SUMMARY.json).
- [Evidence interfaces](study-view/EVIDENCE_ORIGINS.md) and
  [independent reconstruction audit](audits/INDEPENDENT_SEALED_ARTIFACT_REVIEW.md).

## Exact reproduction from the downloaded repository

Every published file is at most7MiB, to fit the anonymous service's documented
single-file limit. The neutral binary parts contain the complete already-vetted
publication tree, including large indexes and original byte bindings. They are
not text-anonymized again. Browsable text may receive additional service masking;
use the protected tree when validating experimental bytes. The transport is an
immutable experiment-closure snapshot and contains the paper as it stood at
closure. The current layout-only paper revision is in `paper/` and the separately
hash-bound `paper-current.tar.gz`; experimental payload parts are unchanged.

To rebuild the latest paper without text-masking changes, download the small
archive, check its SHA-256 against `RELEASE_ID.txt`, and extract into a new folder:

```sh
mkdir ../paper-current
tar -xzf paper-current.tar.gz -C ../paper-current
cd ../paper-current/paper
latexmk -pdf main.tex
```

The archive contains only the current PDF, required sources and English layout /
redraw notes. This presentation update adds no experiments and changes no scores.

If the service does not offer a repository ZIP download, use its public file API
(replace `REVIEW_ID` with the identifier from your anonymous link):

```sh
python3 download_transport.py --api-base https://anonymous.4open.science/api/repo/REVIEW_ID/file --output ../review-download
python3 ../review-download/restore_review.py restore ../review-download --output ../review-exact
```

Downloads use ordinary public GET requests, bounded retries, and byte hashes.
An access-denied response is reported, never bypassed or treated as empty data.

```sh
python3 restore_review.py verify .
python3 restore_review.py restore . --output ../review-exact
python3 ../review-exact/recorded-study/unpack_artifact.py verify ../review-exact/recorded-study
python3 ../review-exact/recorded-study/unpack_artifact.py unpack ../review-exact/recorded-study --output ../study-expanded
python3 ../study-expanded/project/final-native-20260928/verify_final_artifact.py ../study-expanded
```

All destinations must be new. Allow roughly1.3GiB for the exact publication tree,
10.23GiB for expanded evidence, and temporary space. Restoration, unpacking and
reanalysis use Python3.10+ and run offline after download, without keys, models
or analyzer executions. The optional downloader uses ordinary public network GET.
R0/R1 reanalysis does not
claim fresh execution or bit-identical future model replies.

The actual same-model39-subject comparison includes KNighter's real no-report
branches, repeated12-subject comparisons and single-decode12-subject model
configuration sensitivity. Native warning coverage has a noise cost; its F1
nearly ties no-internal. The15-case report reduction is concentrated in one
subject, without target-preservation adjudication. The14-response automatic
illustration and its fixture feedback remain separate from the main checker.
All unfavorable/failed/superseded histories are retained in the exact package.

No project-wide license is granted; provided for review. Upstream notices are
retained in the protected tree and under `notices/`. All new entry documentation
is English. Original raw tool/model language is preserved, not claimed translated
experimental input. Credentials and private original backups are not included.
