from __future__ import annotations

import sys
import subprocess
from pathlib import Path
from types import SimpleNamespace

import pytest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "experiments" / "knighter" / "baseline" / "src"))
sys.path.insert(0, str(ROOT / "experiments" / "knighter" / "experiment"))

from packaged_clang_backend import PackagedClangBackend  # noqa: E402
from report_binding import canonical_build_source_from_html  # noqa: E402
from run_matched_knighter_batch import rate_limit_observed  # noqa: E402


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


def test_baseline_batch_rate_limit_guard():
    assert rate_limit_observed("Error code: 429 - gateway_concurrency_limit")
    assert rate_limit_observed("HTTP 429 rate_limit_error")
    assert not rate_limit_observed("Refinement completed with 1 bug found")


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
