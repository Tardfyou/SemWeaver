"""Repair a pre-model, fully verified zero-scope probe without changing facts."""
import importlib.util
from pathlib import Path

ROOT = Path(__file__).resolve().parent


if __name__ == '__main__':
    original = ROOT / 'launch_any_profile_cap_repair.py'
    source = original.read_text()
    old = "assert relative.parts[0] in ('e3-selected-repeats','e4-common64')"
    new = "assert relative.parts[0]=='e4-focused12'"
    assert source.count(old) == 1
    source = source.replace(old, new)
    old = "assert any(x.get('trace_capped') for x in d['scans'])"
    new = "assert all(x.get('trace_events')==0 and not x.get('trace_capped') for x in d['scans'])\n        assert verified_zero(probe)"
    assert source.count(old) == 1
    source = source.replace(old, new)
    source = source.replace("str(ROOT/'run_any_profile_cap_repair.py')", "str(ROOT/'run_glm_zero_coverage_cell.py')")
    old = "paths=[Path(__file__),ROOT/'run_any_profile_cap_repair.py'"
    new = "paths=[Path(__file__),original,ROOT/'adaptive_trace_probe_zero_coverage.py',ROOT/'glm_mapped_high_zero_coverage_entry.py',ROOT/'run_glm_zero_coverage_cell.py',ROOT/'run_any_profile_cap_repair.py'"
    assert source.count(old) == 1
    source = source.replace(old, new)
    source = source.replace("'operational_repair':'Same response ceiling/profile/method; complete uncapped diagnostic observations, all consumed reply prefixes retained.'",
                            "'operational_repair':'Verified zero-callback availability: fresh four-scan parity, no fabricated dynamics, same mapped high/profile/cap and static evidence; pre-model original failure retained.'")
    helper = ROOT.parent/'LLM-Native/SemWeaver-v43/src/research/trace_health.py'
    spec = importlib.util.spec_from_file_location('original_zero_scope_verifier', helper)
    health = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(health)
    exec(compile(source, str(original)+'[verified-zero-scope-only]', 'exec'),
         {'__name__': '__main__', '__file__': str(Path(__file__).resolve()),
          'original': original, 'verified_zero': health.complete_zero_scope_coverage})
