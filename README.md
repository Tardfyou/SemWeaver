# SemWeaver: FSE 2027 anonymous review artifact

The current replication package is in [review-package/](review-package/README.md).
It contains the pinned SemWeaver source, the FSE-format manuscript and PDF,
frozen checker/patch inputs, raw and derived evidence, three matched repeats
against KNighter's actual refinement loop, a ten-candidate extension screen,
historical mixed-provenance CSA/CodeQL records, and English instructions for
offline verification.

```bash
cd review-package
python3 scripts/verify_v7_review_package.py .
```

The verifier requires no model API, Linux build, or network connection. The
package manifest binds every file by SHA-256, and `REDACTIONS.json` records
path-only anonymization without changing outcome labels. The full method,
denominators, and limitations are described in the package README and paper.

This repository's former source-only snapshot was removed from the current
tree to avoid confusing it with the reviewed version; it remains recoverable
from earlier Git commits. The included KNighter baseline retains its upstream
Apache-2.0 license and attribution. No project-level SemWeaver reuse license
is granted by this review snapshot.
