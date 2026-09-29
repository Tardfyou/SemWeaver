# Recent related-work addition pending manuscript integration

VulGenie, USENIX Security2026:
https://www.usenix.org/conference/usenixsecurity26/presentation/chen-bofei
Published PDF:
https://www.usenix.org/system/files/usenixsecurity26-chen-bofei.pdf
PDF SHA-256: `0636d4b2b0cafb421fe125829cc429cfee51e58b7f32c72fafa7107f63269130`.

Read coverage: complete published text, including design (§3), evaluation (§4),
limitations (§5), related work (§6) and appendices. Initial default-agent403
and browser-cache fetch failures were retrieval failures, not absence evidence;
the publisher PDF was subsequently read with the existing PDF tool.

VulGenie combines patch-dependency graphs, inter-library attack/defense value
flows and LLM-assisted API-rule synthesis, then applies adaptive analysis to
Java applications. It is important adjacent work: analysis-guided patch-derived
security specifications are not unique to SemWeaver. SemWeaver studies edits
to an existing generated checker and its analyzer interaction under paired
warning-count validation. That artifact/operation distinction is not by itself
a novelty proof. Cite this work without comparing its target-vulnerability
metrics to our version-warning proxies. Its limitations concern dynamic Java
features and implicit exploit conditions; they do not establish our effectiveness.
Its references also point to APHP and Seal. This is a scoped update, not a
saturated literature search or authorization for a “first” claim. No new
experiment is needed merely to add this related-work distinction.
