from pathlib import Path
from unittest.mock import patch

from src.tools.compile import CompileCheckerTool
from src.core.csa_build_contract import CSA_PLUGIN_STD_FLAG


def test_compile_checker_stages_knighter_utility_for_both_include_casings(tmp_path: Path):
    utility_header = tmp_path / "source" / "utility.h"
    utility_source = tmp_path / "source" / "utility.cpp"
    utility_header.parent.mkdir()
    utility_header.write_text("#pragma once\n", encoding="utf-8")
    utility_source.write_text("int utility_symbol;\n", encoding="utf-8")
    output_dir = tmp_path / "out"
    tool = CompileCheckerTool(
        {
            "llvm_dir": "/usr/lib/llvm-18",
            "clang_path": "/usr/lib/llvm-18/bin/clang++",
            "utility_header": str(utility_header),
            "utility_source": str(utility_source),
        }
    )

    with patch("src.tools.compile.subprocess.run") as run:
        run.return_value.returncode = 0
        run.return_value.stdout = ""
        run.return_value.stderr = ""
        result = tool.execute(
            checker_name="DemoChecker",
            source_code='#include "clang/StaticAnalyzer/Checkers/Utility.h"\n',
            output_dir=str(output_dir),
        )

    assert result.success
    command = run.call_args.args[0]
    assert CSA_PLUGIN_STD_FLAG in command
    assert CSA_PLUGIN_STD_FLAG == "-std=c++17"
    cmake = (
        Path(__file__).resolve().parents[1]
        / "experiments/robustness/case07_negative_control/CMakeLists.txt"
    ).read_text(encoding="utf-8")
    assert "target_compile_features(SAGenTestPlugin PRIVATE cxx_std_17)" in cmake
    assert str(utility_source) in command
    assert f"-I{output_dir / 'generated-include'}" in command
    assert (output_dir / "generated-include/clang/StaticAnalyzer/Checkers/utility.h").is_file()
    assert (output_dir / "generated-include/clang/StaticAnalyzer/Checkers/Utility.h").is_file()
