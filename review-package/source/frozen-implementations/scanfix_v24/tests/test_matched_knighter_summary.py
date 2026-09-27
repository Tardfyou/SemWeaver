from __future__ import annotations

import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "experiments" / "knighter" / "experiment"))

from summarize_matched_knighter_batch import clopper_pearson  # noqa: E402


def test_clopper_pearson_for_one_of_twelve():
    lower, upper = clopper_pearson(1, 12)

    assert abs(lower - 0.002107593) < 1e-8
    assert abs(upper - 0.384796165) < 1e-8


def test_clopper_pearson_boundary_counts():
    zero_lower, zero_upper = clopper_pearson(0, 12)
    all_lower, all_upper = clopper_pearson(12, 12)

    assert zero_lower == 0.0
    assert 0.0 < zero_upper < 1.0
    assert 0.0 < all_lower < 1.0
    assert all_upper == 1.0
