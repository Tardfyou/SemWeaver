from __future__ import annotations

import sys
import subprocess
from pathlib import Path
from types import SimpleNamespace

import pytest
import httpx
from openai import APIStatusError


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "experiments" / "knighter" / "baseline" / "src"))
sys.path.insert(0, str(ROOT / "experiments" / "knighter" / "experiment"))

from packaged_clang_backend import PackagedClangBackend  # noqa: E402
from report_binding import canonical_build_source_from_html  # noqa: E402
from run_matched_knighter import fixed_reports  # noqa: E402
from run_matched_knighter_batch import (  # noqa: E402
    rate_limit_observed, rate_limit_requires_interruption, transport_interruption_reason,
)
import model as knighter_model  # noqa: E402
import checker_refine as knighter_refine  # noqa: E402
from model import completion_token_parameter, normalize_custom_base_url, status_retry_delay  # noqa: E402


def fake_target(root: Path):
    return SimpleNamespace(repo=SimpleNamespace(working_dir=str(root)))


def test_report_basename_resolves_to_unique_kernel_object(tmp_path: Path):
    source = tmp_path / "drivers" / "vendor" / "target.c"
    source.parent.mkdir(parents=True)
    source.write_text("int target(void) { return 0; }\n", encoding="utf-8")

    objects = PackagedClangBackend.get_objects_from_report(
        "File:| target.c\n", fake_target(tmp_path)
    )

    assert objects == ["drivers/vendor/target.o"]


def test_report_basename_binding_fails_closed_on_ambiguity(tmp_path: Path):
    for directory in ("drivers/a", "drivers/b"):
        source = tmp_path / directory / "target.c"
        source.parent.mkdir(parents=True)
        source.write_text("int target(void) { return 0; }\n", encoding="utf-8")

    with pytest.raises(ValueError, match="Expected one source match"):
        PackagedClangBackend.get_objects_from_report(
            "File:| target.c\n", fake_target(tmp_path)
        )


def test_explicit_repository_path_does_not_depend_on_current_checkout(tmp_path: Path):
    objects = PackagedClangBackend.get_objects_from_report(
        "File:| drivers/removed/in/current/tree.c\n", fake_target(tmp_path)
    )

    assert objects == ["drivers/removed/in/current/tree.o"]


def test_amd_report_uses_kbuild_object_alias_with_display_include_flags(tmp_path: Path):
    objects = PackagedClangBackend.get_objects_from_report(
        "File:| drivers/gpu/drm/amd/display/amdgpu_dm/amdgpu_dm.c\n",
        fake_target(tmp_path),
    )
    assert objects == [
        "drivers/gpu/drm/amd/amdgpu/../display/amdgpu_dm/amdgpu_dm.o"
    ]


def test_report_binding_uses_translation_unit_for_header_diagnostic():
    html = """
    <title>./include/linux/instrumented.h</title>
    <!-- BUGFILE /old/checkout/linux/./include/linux/instrumented.h -->
    <div class="spoiler">clang -cc1 -main-file-name device.c -x c drivers/gpu/device.c\n</div>
    """

    assert canonical_build_source_from_html(html) == "drivers/gpu/device.c"


def test_baseline_keeps_distinct_reports_from_one_translation_unit(tmp_path: Path):
    header = (
        "<title>drivers/gpu/device.c</title>"
        "<div class=\"spoiler\">clang -cc1 -main-file-name device.c "
        "-x c drivers/gpu/device.c</div>"
    )
    (tmp_path / "report-001.html").write_text(header + "<p>Warning A</p>", encoding="utf-8")
    (tmp_path / "report-002.html").write_text(header + "<p>Warning B</p>", encoding="utf-8")

    reports = fixed_reports(tmp_path, limit=5)

    assert [report["id"] for report in reports] == ["report-001", "report-002"]
    assert all("BuildSource:| drivers/gpu/device.c" in report["content"] for report in reports)


def test_knighter_triage_receives_patch_not_duplicate_pattern(monkeypatch):
    backend = SimpleNamespace(get_objects_from_report=lambda _report, _target: ["target.o"])
    monkeypatch.setattr(knighter_refine.global_config, "_config", {
        "backend": backend, "target": object(),
    })
    captured = []
    monkeypatch.setattr(knighter_refine, "check_report", lambda *args, **kwargs: (
        captured.append(kwargs) or "NotABug"
    ))
    checker = SimpleNamespace(checker_id="checker", pattern="PATTERN", patch="ACTUAL PATCH")
    report = SimpleNamespace(report_id="r1", report_content="report", report_objects=[])
    outcome = SimpleNamespace(num_FP=0, num_TP=0, error_objects=set())

    assert knighter_refine._triage_reports([report], checker, 0, outcome)
    assert captured[0]["pattern"] == "PATTERN"
    assert captured[0]["patch"] == "ACTUAL PATCH"
    assert outcome.num_FP == 1


def test_baseline_batch_rate_limit_guard():
    assert rate_limit_observed("Error code: 429 - gateway_concurrency_limit")
    assert rate_limit_observed("HTTP 429 rate_limit_error")
    assert not rate_limit_observed("Refinement completed with 1 bug found")
    assert not rate_limit_requires_interruption(
        0, "HTTP 429 followed by a successful retry", {"execution_valid": True}
    )
    assert rate_limit_requires_interruption(2, "HTTP 429 after retries", {})
    assert transport_interruption_reason(1, "Model request timed out", {}) == "model_timeout"
    assert transport_interruption_reason(0, "Model request timed out but recovered", {
        "execution_valid": True
    }) == ""


def test_bigmodel_openai_compatible_base_url_and_token_parameter():
    assert normalize_custom_base_url("https://open.bigmodel.cn/api/coding/paas/v4/") == (
        "https://open.bigmodel.cn/api/coding/paas/v4"
    )
    assert normalize_custom_base_url("https://provider.example/v1/") == "https://provider.example/v1"
    assert normalize_custom_base_url("https://provider.example") == "https://provider.example/v1"
    assert completion_token_parameter("glm-5.3-flash") == "max_tokens"
    assert completion_token_parameter("gpt-5.6-terra") == "max_completion_tokens"


def test_rate_limit_delay_is_exponential_and_honors_retry_after():
    request = httpx.Request("POST", "https://provider.example/v1/chat/completions")
    response = httpx.Response(429, request=request, headers={"Retry-After": "90"})
    error = APIStatusError("rate limited", response=response, body=None)

    assert status_retry_delay(error, 0) == 90.0
    assert status_retry_delay(error, 3) == 120.0


def test_baseline_model_call_cap_is_enforced_before_another_request(monkeypatch):
    monkeypatch.setenv("CUSTOM_OPENAI_MAX_MODEL_CALLS", "1")
    monkeypatch.setattr(knighter_model, "usage_log", [{"model": "glm-5.3-flash"}])

    with pytest.raises(knighter_model.ModelCallBudgetExhausted, match="1/1"):
        knighter_model.invoke_llm("No second semantic call is allowed")


def test_packaged_compile_forwards_ninja_stdout_to_syntax_repair(tmp_path: Path, monkeypatch):
    backend = PackagedClangBackend(
        str(tmp_path / "backend"),
        cmake_project=str(tmp_path / "cmake"),
        utility_source=str(tmp_path / "utility.cpp"),
        utility_header=str(tmp_path / "utility.h"),
    )
    calls = iter([
        subprocess.CompletedProcess([], 0, stdout="-- Configuring done\n", stderr=""),
        subprocess.CompletedProcess([], 1, stdout="error: no matching member function\n", stderr=""),
    ])
    monkeypatch.setattr(
        "packaged_clang_backend.subprocess.run",
        lambda *args, **kwargs: next(calls),
    )

    code, diagnostics = backend.build_checker(
        "// invalid checker\n", tmp_path / "logs", attempt=1
    )

    assert code == 1
    assert "error: no matching member function" in diagnostics
    assert (tmp_path / "logs" / "build_stdout_1.log").read_text().endswith(
        "error: no matching member function\n"
    )
    assert (tmp_path / "logs" / "build_stderr_1.log").read_text() == ""
