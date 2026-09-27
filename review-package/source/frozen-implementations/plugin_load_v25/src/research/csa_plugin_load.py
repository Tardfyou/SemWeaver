"""Check dynamic symbol resolution before declaring a checker compilable."""
from __future__ import annotations
import os
import subprocess
from pathlib import Path


def probe_checker_plugin(clang: str, plugin: str, timeout: int = 30) -> dict:
    environment = os.environ.copy()
    environment["LD_BIND_NOW"] = "1"
    # Help exits before loading plugins in Clang 18. An actual analysis of an
    # owned inert translation unit forces the dynamic loader to run.
    command = [str(clang), "-cc1", "-load", str(Path(plugin).resolve()), "-analyze", "-x", "c", "-", "-o", os.devnull]
    try:
        result = subprocess.run(command, input="int semweaver_load_probe(void) { return 0; }\n", capture_output=True, text=True, timeout=timeout, env=environment)
    except subprocess.TimeoutExpired:
        return {"loadable": False, "failure_kind": "plugin_load_timeout", "diagnostic": "Plugin load probe timed out", "command": command}
    return {"loadable": result.returncode == 0, "return_code": result.returncode, "failure_kind": "" if result.returncode == 0 else "plugin_load_failure", "diagnostic": (result.stdout + result.stderr)[-6000:] if result.returncode else "", "command": command}
