# Case 19 development robustness probes

These fixtures were constructed after observing the first automatic
GLM-5.3-Flash candidate. They preserve the concrete
`devm_add_action_or_reset(dev, action, data)` API contract from the patch and
vary local and callback names, unrelated statements, nesting, branch
presentation, and negative callback/object relationships. They are development diagnostics,
**not held-out evidence**, and must not be included in an unbiased robustness
rate for this candidate. The earlier generic `register_or_reset(action,data)`
suite changes the API contract and is reported separately rather than
discarded.
