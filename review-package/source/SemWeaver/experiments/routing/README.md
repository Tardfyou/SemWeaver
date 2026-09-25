# Patch-Mechanism Routing Evaluation

Routing is evaluated independently of detector outcomes.

Two annotators label each frozen patch without seeing router predictions or
refinement results. Each annotation records one mechanism label and a semicolon-
separated set of required evidence roles. Disagreements are resolved before the
prediction file is opened. The adjudicated file is the only gold input to the
scorer.

Required gold columns:

- `case_id`
- `adjudicated_mechanism`
- `adjudicated_roles`

Required prediction columns:

- `case_id`
- `predicted_mechanism`
- `predicted_roles`
- `abstained`

Run:

```bash
python3 experiments/routing/score_router.py \
  --gold artifacts/routing/gold.csv \
  --predictions artifacts/routing/predictions.csv \
  --output artifacts/routing/router_metrics.json
```

The scorer reports mechanism accuracy, exact role-set match, micro role
precision/recall/F1, coverage, abstention, missing predictions, and per-role
counts. End-to-end experiments must separately compare predicted, oracle,
all-role, random-role, and deliberately wrong-route treatments.
