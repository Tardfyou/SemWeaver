# Reference-integrity checkpoint

Current original-based bibliography:63 entries,50 citation keys used in LaTeX.
No duplicate entry keys or missing actual citation keys were found. This is
a syntax/linkage check, not verification of all63 publications or every claim.

Five relevant works have primary-source existence/metadata support:

- KNighter: authors/title supported by https://arxiv.org/abs/2503.09002 ;
  ACM venue,2025 publication and655--669 pages match Crossref DOI metadata:
  https://api.crossref.org/works/10.1145/3731569.3764827
- Interleaving Static Analysis and LLM Prompting: title, SOAP2024 venue and9--17
  pages match https://api.crossref.org/works/10.1145/3652588.3663317
- IRIS: authors/title/ICLR2025 match the official proceedings entry:
  https://proceedings.iclr.cc/paper_files/paper/2025/hash/582d4e27fa24168f3af1f4582655034b-Abstract-Conference.html
- LLM4PFA: authors/title and2025 preprint match https://arxiv.org/abs/2506.10322
- ZeroFalse: authors/title and2025 preprint match https://arxiv.org/abs/2510.02534

ACM landing pages returned403; this is a fetch limitation, not evidence that
the papers do not exist. Crossref is used for the publisher-deposited metadata,
not as a substitute for reading full technical claims. Remaining citation
authenticity/claim correspondence and final rendered references still need
checking before release. No bibliography or manuscript file changed here.

## Used DOI-entry metadata audit, September29

`REFERENCE_DOI_METADATA_CHECK.json` audits the37 DOI-bearing entries among
the50 used keys against publisher-deposited Crossref metadata.27 initially
match title/year/pages; eight require review and two return retrieval errors.
The original response/checkpoint is retained. Some publishers store subtitles
separately; this is not evidence that the bibliography's complete title is wrong.

`REFERENCE_DOI_METADATA_RECHECK.json` includes separate publisher subtitles
and resolves six further entries (Engler, Coverity, FlowDroid, LineVul,
VulPecker and Angelix), bringing exact title/year/page matches to33. The
SLAM retry has a transient retrieval error; its original title/page metadata
is preserved. Four remaining DOI-entry checks have primary-source support:

- QL: title/authors/ECOOP2016 and2:1--2:25 match the publisher entry:
  https://drops.dagstuhl.de/entities/document/10.4230/LIPIcs.ECOOP.2016.2
- Codex: the arXiv primary entry confirms the paper identity/title/year:
  https://arxiv.org/abs/2107.03374 ; Crossref retrieval failure is not absence.
- SLAM: the originating project's publication list confirms the complete
  title, Ball/Rajamani, POPL2002 and1--3 pages:
  https://www.microsoft.com/en-us/research/project/slam/
- Pixy: Crossref's page string is malformed (`6 pp.-263`); the authors'
  institutional record gives258--263, matching the bibliography:
  https://informatics.tuwien.ac.at/people/engin-kirda
  The author-hosted paper confirms identity/title/authors:
  https://sites.cs.ucsb.edu/~chris/research/doc/oakland06_pixy.pdf

This establishes metadata support for the37 used DOI-bearing entries, not
verification of every author/venue field by the automatic comparator or the
technical claim attached to every citation.13 used non-DOI entries and claim
correspondence still need their own checks. No bibliography was silently
shortened or edited to force matches; the manuscript remains unchanged.
