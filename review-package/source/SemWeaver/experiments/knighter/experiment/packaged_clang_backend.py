"""Execution adapter for running the Knighter loop with packaged Clang 18.

This changes only plugin compilation and tool locations. It inherits Knighter's
original scanning, report extraction, triage, refinement, and validation logic.
"""

from __future__ import annotations

import shlex
import shutil
import subprocess
import re
import posixpath
from pathlib import Path

from backends.csa import ClangBackend
from targets.linux import Linux


class PackagedClangBackend(ClangBackend):
    def __init__(
        self,
        backend_path: str,
        *,
        cmake_project: str,
        utility_source: str,
        utility_header: str,
        clang_root: str = "/usr/lib/llvm-18",
    ):
        super().__init__(backend_path)
        self.backend_path.mkdir(parents=True, exist_ok=True)
        self.cmake_project = Path(cmake_project).resolve()
        self.utility_source = Path(utility_source).resolve()
        self.utility_header = Path(utility_header).resolve()
        self.clang_root = Path(clang_root)
        self.package_source_dir = self.backend_path / "source"
        self.package_build_dir = self.backend_path / "cmake-build"
        self.compatibility_lib_dir = self.backend_path / "build" / "lib"

    @property
    def plugin_path(self) -> Path:
        return self.compatibility_lib_dir / "SAGenTestPlugin.so"

    def build_checker(
        self,
        checker_code: str,
        log_dir: Path,
        checker_name: str = "SAGenTest",
        attempt: int = 1,
        jobs: int = 8,
        timeout: int = 300,
    ):
        if checker_name != "SAGenTest":
            return -1, "Packaged adapter currently supports only SAGenTest."

        self.package_source_dir.mkdir(parents=True, exist_ok=True)
        self.compatibility_lib_dir.mkdir(parents=True, exist_ok=True)
        log_dir = Path(log_dir)
        log_dir.mkdir(parents=True, exist_ok=True)
        checker_source = self.package_source_dir / "SAGenTestChecker.cpp"
        checker_source.write_text(checker_code, encoding="utf-8")

        configure = [
            "cmake",
            "-S",
            str(self.cmake_project),
            "-B",
            str(self.package_build_dir),
            "-G",
            "Ninja",
            f"-DCMAKE_CXX_COMPILER={self.clang_root / 'bin' / 'clang++'}",
            f"-DCHECKER_SOURCE={checker_source}",
            f"-DUTILITY_SOURCE={self.utility_source}",
            f"-DUTILITY_HEADER={self.utility_header}",
            f"-DLLVM_DIR={self.clang_root / 'lib' / 'cmake' / 'llvm'}",
            f"-DClang_DIR={self.clang_root / 'lib' / 'cmake' / 'clang'}",
        ]
        try:
            configured = subprocess.run(
                configure,
                capture_output=True,
                text=True,
                timeout=timeout,
                check=False,
            )
            built = subprocess.run(
                ["cmake", "--build", str(self.package_build_dir), "-j", str(jobs)],
                capture_output=True,
                text=True,
                timeout=timeout,
                check=False,
            ) if configured.returncode == 0 else None
        except Exception as exc:
            (log_dir / f"build_error_{attempt}.log").write_text(str(exc), encoding="utf-8")
            return -1, str(exc)

        stdout = (configured.stdout or "") + ((built.stdout or "") if built else "")
        stderr = (configured.stderr or "") + ((built.stderr or "") if built else "")
        (log_dir / f"build_stdout_{attempt}.log").write_text(stdout, encoding="utf-8")
        (log_dir / f"build_stderr_{attempt}.log").write_text(stderr, encoding="utf-8")
        if configured.returncode != 0 or built is None or built.returncode != 0:
            # Ninja writes compiler errors to stdout. Knighter's syntax-repair
            # loop consumes this return value as its diagnostic text and would
            # otherwise stop after one failed build with "stderr was empty".
            # Keep the raw streams in their separate logs, but forward both to
            # the upstream repair prompt when compilation fails.
            diagnostics = "\n".join(part for part in (stdout, stderr) if part)
            return (built.returncode if built is not None else configured.returncode), diagnostics

        built_plugin = self.package_build_dir / "SAGenTestPlugin.so"
        if not built_plugin.exists():
            return -1, f"Built plugin missing: {built_plugin}"
        shutil.copy2(built_plugin, self.plugin_path)
        return 0, stderr

    def _generate_command(self, no_output: bool = False, plugin_names=None):
        if plugin_names:
            raise NotImplementedError("Matched baseline adapter runs one frozen checker at a time.")

        clang = self.clang_root / "bin" / "clang"
        scan_build = Path("/usr/bin/scan-build-18")
        command = (
            f"PATH={shlex.quote(str(self.clang_root / 'bin'))}:/usr/bin:$PATH "
            f"{shlex.quote(str(scan_build))} "
            f"--use-analyzer={shlex.quote(str(clang))} --use-cc=clang "
            f"-load-plugin {shlex.quote(str(self.plugin_path))} "
            "-enable-checker custom.SAGenTestChecker "
        )
        for arg_name, arg_value in self._default_args:
            if no_output and arg_name == "-o":
                continue
            command += f"{arg_name} {shlex.quote(str(arg_value))} "
        return command

    @staticmethod
    def get_objects_from_report(report: str, target):
        """Resolve report source names to repository-relative kernel objects.

        Knighter's upstream implementation makes a basename absolute relative
        to the runner cwd before stripping the Linux checkout prefix. Packaged
        scan-build reports expose only the basename in their rendered table,
        so that logic silently produces an object outside the Linux tree. This
        adapter resolves a basename only when it is unique in the frozen tree.
        Ambiguous or missing bindings fail closed.
        """

        source_names = re.findall(r"BuildSource:\|\s*(.+?\.c)(?:\s|$)", report)
        if not source_names:
            source_names = re.findall(r"File:\|\s*(.+?\.c)(?:\s|$)", report)
        if not source_names:
            raise ValueError("No C source path found in analyzer report")

        root = Path(target.repo.working_dir).resolve()
        objects = []
        for raw_name in source_names:
            normalized = raw_name.strip().replace("\\", "/")
            marker = "/artifacts/external/linux/"
            if marker in normalized:
                normalized = normalized.split(marker, 1)[1]
            candidate = Path(normalized)

            if candidate.is_absolute():
                try:
                    relative_source = candidate.resolve().relative_to(root)
                except ValueError:
                    relative_source = None
            elif len(candidate.parts) > 1 and ".." not in candidate.parts:
                relative_source = candidate
            else:
                relative_source = None

            if relative_source is None:
                matches = sorted(root.rglob(candidate.name))
                matches = [path for path in matches if path.is_file()]
                if len(matches) != 1:
                    raise ValueError(
                        f"Expected one source match for {candidate.name}, found {len(matches)}"
                    )
                relative_source = matches[0].relative_to(root)

            direct_object = relative_source.with_suffix(".o").as_posix()
            build_object = Linux.get_object_name(relative_source.as_posix())
            objects.append(
                build_object
                if posixpath.normpath(build_object) == posixpath.normpath(direct_object)
                else direct_object
            )

        return sorted(set(objects))
