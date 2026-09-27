# Prompt Layout

SemWeaver uses a manifest-driven prompt repository. Runtime access is implemented
in `src/prompts/repository.py`; every production prompt must be registered in
`manifest.yaml` and loaded by prompt id.

Directories:

- `manifest.yaml`
- `generate/`
- `refine/`
- `orchestrator/`
- `analysis/`

Primary prompt ids:

- `generate.agent.system`
- `generate.agent.task`
- `generate.agent.plan`
- `generate.agent.rag_check`
- `generate.agent.draft`
- `generate.agent.repair`
- `generate.agent.analyzer.csa`
- `generate.agent.analyzer.codeql`
- `generate.agent.reference.csa`
- `generate.agent.reference.codeql`
- `refine.agent.system`
- `refine.agent.task`
- `refine.agent.decide`
- `refine.agent.repair`
- `orchestrator.analyzer_selection`
- `analysis.patch`

Rules:

- Generate and refine are independent systems. Refine consumes persisted
  generation contracts and never silently reuses generation prompts.
- Generate follows `analyze_patch -> search_knowledge -> draft/materialize ->
  validate -> apply_patch repair`.
- CSA prompts target Clang 18 plugin constraints, including correct registry and
  API-version exports.
- Add prompts by defining their hierarchy and registering them in the manifest;
  do not embed production prompts in Python.
- Delete unused prompts to avoid documentation/runtime drift.
- Production prompts are English. Historical non-English prompts must remain
  available only through immutable run artifacts or tagged repository revisions,
  with an English translation included in the replication package.
