from types import SimpleNamespace
import subprocess
from src.research.csa_plugin_load import probe_checker_plugin


def test_probe_forces_symbol_resolution(monkeypatch, tmp_path):
    def run(command, **kwargs):
        assert kwargs["env"]["LD_BIND_NOW"] == "1"
        assert "-analyze" in command and "-analyzer-checker-help" not in command
        assert "semweaver_load_probe" in kwargs["input"]
        return SimpleNamespace(returncode=0, stdout="Available checkers", stderr="")
    monkeypatch.setattr(subprocess, "run", run)
    assert probe_checker_plugin("clang", str(tmp_path / "checker.so"))["loadable"]


def test_missing_symbol_is_a_build_gate_failure(monkeypatch):
    monkeypatch.setattr(subprocess, "run", lambda *a, **kw: SimpleNamespace(returncode=127, stdout="", stderr="undefined symbol: checkEndFunction"))
    result = probe_checker_plugin("clang", "checker.so")
    assert not result["loadable"] and "undefined symbol" in result["diagnostic"]


def test_probe_timeout_is_invalid(monkeypatch):
    def run(*args, **kwargs):
        raise subprocess.TimeoutExpired(args[0], 30)
    monkeypatch.setattr(subprocess, "run", run)
    assert probe_checker_plugin("clang", "checker.so")["failure_kind"] == "plugin_load_timeout"
