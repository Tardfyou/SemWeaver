from __future__ import annotations

import os
import re
import shlex
import shutil
import subprocess
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple


def _expand_path(value: str) -> str:
    return os.path.expandvars(os.path.expanduser(str(value or "")))


def _optional_path(value: Any) -> Path:
    token = _expand_path(str(value or "")).strip()
    return Path(token).resolve() if token else Path("")


def _path_is_configured(path: Path) -> bool:
    return str(path) not in {"", "."}


def _dedupe(values: List[str]) -> List[str]:
    result: List[str] = []
    seen = set()
    for value in values:
        if value and value not in seen:
            seen.add(value)
            result.append(value)
    return result


def _ensure_executable(path: Path) -> None:
    if not path.exists():
        return
    mode = path.stat().st_mode
    if mode & 0o111:
        return
    path.chmod(mode | 0o755)


@dataclass(frozen=True)
class KnighterE2Config:
    enabled: bool = False
    knighter_root: Path = Path("")
    llvm_dir: Path = Path("")
    linux_dir: Path = Path("")
    host_deps_dir: Path = Path("")
    arch: str = "x86"
    jobs: int = 8
    timeout: int = 1800
    scan_commit: str = "v6.13"
    checker_name: str = "SAGenTest"
    result_dir: Path = Path("")
    utility_header: Path = Path("")
    utility_source: Path = Path("")

    @classmethod
    def from_config(cls, raw: Dict[str, Any] | None) -> "KnighterE2Config":
        data = raw or {}
        knighter_root = _optional_path(data.get("knighter_root", ""))
        llvm_dir = _optional_path(data.get("llvm_dir", ""))
        linux_dir = _optional_path(data.get("linux_dir", ""))
        host_deps_dir = _optional_path(data.get("host_deps_dir", ""))
        result_dir = _optional_path(data.get("result_dir", ""))
        utility_header = _optional_path(data.get("utility_header", ""))
        utility_source = _optional_path(data.get("utility_source", ""))

        if not _path_is_configured(host_deps_dir) and _path_is_configured(knighter_root):
            host_deps_dir = (knighter_root.parent / "host_deps" / "jammy-amd64" / "root").resolve()
        if not _path_is_configured(utility_header) and _path_is_configured(knighter_root):
            utility_header = (knighter_root / "llvm_utils" / "utility.h").resolve()
        if not _path_is_configured(utility_source) and _path_is_configured(knighter_root):
            utility_source = (knighter_root / "llvm_utils" / "utility.cpp").resolve()

        return cls(
            enabled=bool(data.get("enabled", False)),
            knighter_root=knighter_root,
            llvm_dir=llvm_dir,
            linux_dir=linux_dir,
            host_deps_dir=host_deps_dir,
            arch=str(data.get("arch", "x86") or "x86"),
            jobs=int(data.get("jobs", 8) or 8),
            timeout=int(data.get("timeout", data.get("scan_timeout", 1800)) or 1800),
            scan_commit=str(data.get("scan_commit", "v6.13") or "v6.13"),
            checker_name=str(data.get("checker_name", "SAGenTest") or "SAGenTest"),
            result_dir=result_dir,
            utility_header=utility_header,
            utility_source=utility_source,
        )

    @property
    def llvm_build_dir(self) -> Path:
        return self.llvm_dir / "build"

    @property
    def plugin_root(self) -> Path:
        return self.llvm_dir / "clang" / "lib" / "Analysis" / "plugins"

    @property
    def plugin_dir(self) -> Path:
        return self.plugin_root / f"{self.checker_name}Handling"

    @property
    def checker_cpp(self) -> Path:
        return self.plugin_dir / f"{self.checker_name}Checker.cpp"

    @property
    def plugin_so(self) -> Path:
        return self.llvm_build_dir / "lib" / f"{self.checker_name}Plugin.so"

    @property
    def scan_build(self) -> Path:
        return self.llvm_build_dir / "bin" / "scan-build"

    @property
    def host_usr_bin_dir(self) -> Path:
        return self.host_deps_dir / "usr" / "bin"

    @property
    def host_usr_include_dir(self) -> Path:
        return self.host_deps_dir / "usr" / "include"

    @property
    def host_multiarch_lib_dir(self) -> Path:
        lib_root = self.host_deps_dir / "usr" / "lib"
        matches = sorted(lib_root.glob("*-linux-gnu")) if lib_root.exists() else []
        if matches:
            return matches[0]
        return lib_root / "x86_64-linux-gnu"

    @property
    def host_multiarch_include_dir(self) -> Path:
        candidate = self.host_usr_include_dir / self.host_multiarch_lib_dir.name
        return candidate

    @property
    def host_pkgconfig_dirs(self) -> List[Path]:
        return [
            self.host_multiarch_lib_dir / "pkgconfig",
            self.host_deps_dir / "usr" / "lib" / "pkgconfig",
            self.host_deps_dir / "usr" / "share" / "pkgconfig",
        ]


def load_knighter_e2_config(config: Dict[str, Any] | None) -> KnighterE2Config:
    data = config or {}
    if "knighter_e2" in data:
        return KnighterE2Config.from_config(data.get("knighter_e2") or {})
    semantic = (data.get("validation", {}) or {}).get("semantic", {}) or {}
    return KnighterE2Config.from_config(semantic.get("knighter_e2") or {})


def validate_knighter_environment(env: KnighterE2Config, *, require_plugin_tree: bool = True) -> Tuple[bool, str]:
    if not env.enabled:
        return False, "Knighter E2 mode is disabled"
    _ensure_executable(env.scan_build)
    _ensure_executable(env.llvm_build_dir / "bin" / "clang")
    _ensure_executable(env.llvm_build_dir / "bin" / "ld.lld")
    _ensure_executable(env.llvm_build_dir / "libexec" / "ccc-analyzer")
    _ensure_executable(env.llvm_build_dir / "libexec" / "c++-analyzer")
    checks = [
        (_path_is_configured(env.knighter_root) and env.knighter_root.exists(), f"Knighter root not found: {env.knighter_root}"),
        (_path_is_configured(env.llvm_dir) and env.llvm_dir.exists(), f"Knighter LLVM_dir not found: {env.llvm_dir}"),
        (env.llvm_build_dir.exists(), f"Knighter LLVM build dir not found: {env.llvm_build_dir}"),
        (env.scan_build.exists(), f"Knighter scan-build not found: {env.scan_build}"),
        ((env.llvm_build_dir / "bin" / "clang").exists(), f"Knighter clang not found: {env.llvm_build_dir / 'bin' / 'clang'}"),
        (_path_is_configured(env.linux_dir) and env.linux_dir.exists(), f"Knighter linux_dir not found: {env.linux_dir}"),
        (_path_is_configured(env.utility_header) and env.utility_header.exists(), f"Knighter utility.h not found: {env.utility_header}"),
        (_path_is_configured(env.utility_source) and env.utility_source.exists(), f"Knighter utility.cpp not found: {env.utility_source}"),
    ]
    if _path_is_configured(env.host_deps_dir):
        checks.extend(
            [
                (env.host_deps_dir.exists(), f"Knighter host_deps_dir not found: {env.host_deps_dir}"),
                (env.host_usr_include_dir.joinpath("libelf.h").exists(), f"Knighter host libelf.h not found: {env.host_usr_include_dir / 'libelf.h'}"),
                (env.host_usr_include_dir.joinpath("openssl", "opensslv.h").exists(), f"Knighter host openssl headers not found: {env.host_usr_include_dir / 'openssl' / 'opensslv.h'}"),
                (env.host_multiarch_lib_dir.joinpath("libelf.so").exists(), f"Knighter host libelf.so not found: {env.host_multiarch_lib_dir / 'libelf.so'}"),
                (env.host_multiarch_lib_dir.joinpath("libcrypto.so").exists(), f"Knighter host libcrypto.so not found: {env.host_multiarch_lib_dir / 'libcrypto.so'}"),
                (env.host_usr_bin_dir.joinpath("pkg-config").exists(), f"Knighter host pkg-config not found: {env.host_usr_bin_dir / 'pkg-config'}"),
            ]
        )
    if require_plugin_tree:
        checks.append((env.plugin_root.exists(), f"Knighter plugin root not found: {env.plugin_root}"))
    for ok, message in checks:
        if not ok:
            return False, message
    return True, ""


def _merge_flag_value(prefix_values: List[str], current: str) -> str:
    parts = [value for value in prefix_values if value]
    current = str(current or "").strip()
    if current:
        parts.append(current)
    return " ".join(parts).strip()


def _merge_path_value(prefix_values: List[str], current: str) -> str:
    parts = [value for value in prefix_values if value]
    current = str(current or "").strip()
    if current:
        parts.append(current)
    return ":".join(parts).strip(":")


def build_knighter_process_env(env: KnighterE2Config) -> Dict[str, str]:
    process_env = dict(os.environ)
    path_prefixes = [str(env.llvm_build_dir / "bin")]

    if _path_is_configured(env.host_deps_dir) and env.host_deps_dir.exists():
        if env.host_usr_bin_dir.exists():
            path_prefixes.append(str(env.host_usr_bin_dir))

        include_prefixes = [f"-I{env.host_usr_include_dir}"]
        if env.host_multiarch_include_dir.exists():
            include_prefixes.append(f"-I{env.host_multiarch_include_dir}")
        process_env["HOSTCFLAGS"] = _merge_flag_value(include_prefixes, process_env.get("HOSTCFLAGS", ""))

        lib_prefixes: List[str] = []
        if env.host_multiarch_lib_dir.exists():
            lib_dir = str(env.host_multiarch_lib_dir)
            lib_prefixes.extend([f"-L{lib_dir}", f"-Wl,-rpath,{lib_dir}"])
        process_env["HOSTLDFLAGS"] = _merge_flag_value(lib_prefixes, process_env.get("HOSTLDFLAGS", ""))

        pkgconfig_dirs = [str(path) for path in env.host_pkgconfig_dirs if path.exists()]
        if pkgconfig_dirs:
            process_env["PKG_CONFIG_LIBDIR"] = _merge_path_value(pkgconfig_dirs, process_env.get("PKG_CONFIG_LIBDIR", ""))
        process_env["PKG_CONFIG_SYSROOT_DIR"] = str(env.host_deps_dir)
        local_pkg_config = env.host_usr_bin_dir / "pkg-config"
        if local_pkg_config.exists():
            process_env["HOSTPKG_CONFIG"] = str(local_pkg_config)

    process_env["PATH"] = _merge_path_value(path_prefixes, process_env.get("PATH", ""))
    return process_env


_CCC_ANALYZER_MLLVM_OLD = """  if ($Arg =~ /^-m.*/) {
    push @CompileOpts,$Arg;
    next;
  }
"""

_CCC_ANALYZER_MLLVM_NEW = """  if ($Arg eq '-mllvm' || $Arg eq '--mllvm') {
    push @CompileOpts, $Arg;
    if ($i + 1 < scalar(@ARGV)) {
      ++$i;
      push @CompileOpts, $ARGV[$i];
    }
    next;
  }

  if ($Arg =~ /^--mllvm=.*/) {
    push @CompileOpts, $Arg;
    next;
  }

  if ($Arg =~ /^-m.*/) {
    push @CompileOpts,$Arg;
    next;
  }
"""


def _patch_ccc_analyzer_mllvm(script_path: Path) -> None:
    text = script_path.read_text(encoding="utf-8", errors="ignore")
    if _CCC_ANALYZER_MLLVM_NEW in text:
        _ensure_executable(script_path)
        return
    if _CCC_ANALYZER_MLLVM_OLD not in text:
        raise RuntimeError(f"Unable to patch {script_path}: expected -m* forwarding block not found")
    text = text.replace(_CCC_ANALYZER_MLLVM_OLD, _CCC_ANALYZER_MLLVM_NEW, 1)
    script_path.write_text(text, encoding="utf-8")
    _ensure_executable(script_path)


def prepare_knighter_e2_scan_build(env: KnighterE2Config, work_root: Path) -> Path:
    """Create an E2-only patched scan-build toolchain without mutating Knighter's LLVM tree."""
    work_root = Path(work_root).expanduser().resolve()
    tool_root = work_root / ".knighter_e2_scan_build"
    scan_build_root = tool_root / "tools" / "scan-build"
    bin_dir = scan_build_root / "bin"
    libexec_dir = scan_build_root / "libexec"
    bin_dir.mkdir(parents=True, exist_ok=True)
    libexec_dir.mkdir(parents=True, exist_ok=True)

    scan_build_src = env.scan_build
    ccc_src = env.llvm_build_dir / "libexec" / "ccc-analyzer"
    cxx_src = env.llvm_build_dir / "libexec" / "c++-analyzer"

    scan_build_dst = bin_dir / "scan-build"
    ccc_dst = libexec_dir / "ccc-analyzer"
    cxx_dst = libexec_dir / "c++-analyzer"

    shutil.copy2(scan_build_src, scan_build_dst)
    shutil.copy2(ccc_src, ccc_dst)
    shutil.copy2(cxx_src, cxx_dst)

    _patch_ccc_analyzer_mllvm(ccc_dst)
    _ensure_executable(cxx_dst)
    _ensure_executable(scan_build_dst)
    return scan_build_dst


def normalize_checker_name(checker_name: str, default: str = "SAGenTest") -> str:
    name = str(checker_name or default).strip()
    name = re.sub(r"Checker$", "", name)
    name = re.sub(r"Plugin$", "", name)
    name = re.sub(r"[^A-Za-z0-9_]", "_", name)
    if not name:
        name = default
    if not name[0].isalpha():
        name = f"{default}{name}"
    return name


def rewrite_checker_identity(source_code: str, plugin_name: str) -> str:
    plugin_name = normalize_checker_name(plugin_name)
    class_name = f"{plugin_name}Checker"
    text = str(source_code or "")
    text = re.sub(r"\bSAGenTestChecker\b", class_name, text)
    text = re.sub(r'"custom\.SAGenTestChecker"', f'"custom.{class_name}"', text)
    text = re.sub(r'"custom\.[A-Za-z_]\w*Checker"', f'"custom.{class_name}"', text)
    return text


def ensure_knighter_plugin(env: KnighterE2Config) -> Tuple[bool, str]:
    ok, message = validate_knighter_environment(env, require_plugin_tree=True)
    if not ok:
        return False, message

    env.plugin_dir.mkdir(parents=True, exist_ok=True)

    def plugin_files_ready() -> bool:
        main_cmake = env.plugin_root / "CMakeLists.txt"
        required = [
            env.plugin_dir / "CMakeLists.txt",
            env.plugin_dir / f"{env.checker_name}Checker.cpp",
            env.plugin_dir / f"{env.checker_name}Checker.exports",
        ]
        if not all(path.exists() for path in required) or not main_cmake.exists():
            return False
        text = main_cmake.read_text(encoding="utf-8", errors="ignore")
        return f"add_subdirectory({env.checker_name}Handling)" in text

    create_plugin = env.plugin_root / "create_plugin.py"
    if not create_plugin.exists() and env.knighter_root:
        source = env.knighter_root / "llvm_utils" / "create_plugin.py"
        if source.exists():
            shutil.copy2(source, create_plugin)

    if create_plugin.exists() and not plugin_files_ready():
        proc = subprocess.run(
            ["python3", str(create_plugin.name), env.checker_name],
            cwd=str(env.plugin_root),
            capture_output=True,
            text=True,
            timeout=60,
        )
        if proc.returncode != 0 and not plugin_files_ready():
            return False, (proc.stderr or proc.stdout or "create_plugin.py failed").strip()

    if not plugin_files_ready():
        return False, f"Plugin dir was not created: {env.plugin_dir}"
    return True, ""


def build_knighter_checker(
    env: KnighterE2Config,
    source_code: str,
    *,
    output_dir: str = "",
) -> Tuple[bool, str, Dict[str, Any]]:
    env = KnighterE2Config(
        **{
            **env.__dict__,
            "checker_name": normalize_checker_name(env.checker_name),
        }
    )
    ok, message = ensure_knighter_plugin(env)
    if not ok:
        return False, "", {"error": message}

    rewritten = rewrite_checker_identity(source_code, env.checker_name)
    env.checker_cpp.write_text(rewritten, encoding="utf-8")

    log_dir = Path(output_dir or env.result_dir or ".").expanduser().resolve()
    log_dir.mkdir(parents=True, exist_ok=True)
    build_log = log_dir / f"{env.checker_name}_knighter_build.log"

    proc = subprocess.run(
        ["make", f"-j{max(1, env.jobs)}", f"{env.checker_name}Plugin"],
        cwd=str(env.llvm_build_dir),
        capture_output=True,
        text=True,
        timeout=max(60, env.timeout),
    )
    build_output = (proc.stdout or "") + ("\n" + proc.stderr if proc.stderr else "")
    build_log.write_text(build_output, encoding="utf-8", errors="ignore")

    if proc.returncode != 0:
        return False, build_output, {
            "error": f"Knighter plugin build failed with return code {proc.returncode}",
            "build_log": str(build_log),
            "source_file": str(env.checker_cpp),
        }

    output_file = str(env.plugin_so.resolve())
    if not Path(output_file).exists():
        return False, build_output, {
            "error": f"Knighter plugin build did not produce {output_file}",
            "build_log": str(build_log),
            "source_file": str(env.checker_cpp),
        }

    return True, build_output, {
        "source_file": str(env.checker_cpp),
        "output_file": output_file,
        "build_log": str(build_log),
        "checker_name": f"{env.checker_name}Checker",
        "plugin_name": env.checker_name,
        "compile_mode": "knighter_llvm_plugin",
    }


def extract_commit_id_from_patch(patch_text: str) -> str:
    patterns = [
        r"^commit\s+([0-9a-fA-F]{12,40})\b",
        r"\bcommit\s+([0-9a-fA-F]{12,40})\b",
    ]
    for pattern in patterns:
        match = re.search(pattern, patch_text or "", flags=re.MULTILINE)
        if match:
            return match.group(1)
    return ""


def changed_c_files_from_patch(patch_text: str) -> List[str]:
    files: List[str] = []
    for match in re.finditer(r"^---\s+a/(.+)$", patch_text or "", flags=re.MULTILINE):
        path = match.group(1).strip()
        if path.endswith(".c"):
            files.append(path)
    return _dedupe(files)


def _path_similarity(candidate: str, source_file: str) -> int:
    candidate_parts = Path(candidate).with_suffix(".c").parts
    source_parts = Path(source_file).parts
    score = 0
    for left, right in zip(reversed(candidate_parts), reversed(source_parts)):
        if left != right:
            break
        score += 1
    return score


def object_for_source(knighter_root: Path, source_file: str) -> str:
    build_commands_path = knighter_root / "src" / "targets" / "linux-build-commands.txt"
    file_path = Path(source_file)
    default = str(file_path.with_suffix(".o"))
    if not build_commands_path.exists():
        return default

    try:
        build_commands = build_commands_path.read_text(encoding="utf-8", errors="ignore")
    except OSError:
        return default

    stem = re.escape(file_path.stem)
    pattern = rf"-o\s+(\S*?{stem}\.o)\b"
    matches = re.findall(pattern, build_commands)
    if not matches:
        return default
    matches.sort(key=lambda item: _path_similarity(item, source_file), reverse=True)
    return matches[0]


def objects_from_patch(env: KnighterE2Config, patch_text: str) -> List[str]:
    return [object_for_source(env.knighter_root, path) for path in changed_c_files_from_patch(patch_text)]


def knighter_scan_prefix(
    env: KnighterE2Config,
    output_dir: Path,
    *,
    no_output: bool = False,
    scan_build_path: Optional[Path] = None,
) -> str:
    scan_build = Path(scan_build_path or env.scan_build).expanduser().resolve()
    parts = [
        f"PATH={shlex.quote(str(env.llvm_build_dir / 'bin'))}:$PATH",
        shlex.quote(str(scan_build)),
        f"--use-analyzer={shlex.quote(str(env.llvm_build_dir / 'bin' / 'clang'))}",
        "--use-cc=clang",
        "-load-plugin",
        shlex.quote(str(env.plugin_so)),
        "-enable-checker",
        f"custom.{env.checker_name}Checker",
    ]
    for name, value in [
        ("-disable-checker", "core"),
        ("-disable-checker", "cplusplus"),
        ("-disable-checker", "deadcode"),
        ("-disable-checker", "unix"),
        ("-disable-checker", "nullability"),
        ("-disable-checker", "security"),
        ("-maxloop", "4"),
    ]:
        parts.extend([name, value])
    if not no_output:
        parts.extend(["-o", shlex.quote(str(output_dir))])
    return " ".join(parts) + " "


def git_checkout_and_configure(
    env: KnighterE2Config,
    commit_id: str,
    *,
    before: bool,
    output_dir: Path,
) -> Tuple[bool, str]:
    target = f"{commit_id}^" if before else commit_id
    process_env = build_knighter_process_env(env)
    commands = [
        ["make", "clean"],
        ["git", "checkout", target],
        ["make", "LLVM=1", f"ARCH={env.arch}", "allyesconfig"],
    ]
    logs: List[str] = []
    for cmd in commands:
        proc = subprocess.run(
            cmd,
            cwd=str(env.linux_dir),
            capture_output=True,
            text=True,
            timeout=max(300, env.timeout),
            env=process_env,
        )
        text = f"$ {' '.join(cmd)}\n{proc.stdout or ''}{proc.stderr or ''}"
        logs.append(text)
        if proc.returncode != 0:
            return False, "\n".join(logs)

    scripts_config = env.linux_dir / "scripts" / "config"
    if scripts_config.exists():
        # E2 validation runs Clang Static Analyzer over kernel objects. Some
        # allyesconfig instrumentation options add cc1-only or plugin flags that
        # scan-build replays as invalid analyzer arguments (for example a bare
        # "-mllvm -mllvm" from AUTOFDO), causing false 0/0 validation results.
        disable_options = [
            "AUTOFDO_CLANG",
            "GCOV_KERNEL",
            "KCOV",
            "KCOV_INSTRUMENT_ALL",
            "KCOV_ENABLE_COMPARISONS",
            "KASAN",
            "KMSAN",
            "KCSAN",
        ]
        for option in disable_options:
            proc = subprocess.run(
                [str(scripts_config), "--disable", option],
                cwd=str(env.linux_dir),
                capture_output=True,
                text=True,
                timeout=max(300, env.timeout),
                env=process_env,
            )
            logs.append(f"$ {scripts_config} --disable {option}\n{proc.stdout or ''}{proc.stderr or ''}")
            if proc.returncode != 0:
                return False, "\n".join(logs)

    olddefcmd = knighter_scan_prefix(env, output_dir) + f"make LLVM=1 ARCH={env.arch} olddefconfig"
    proc = subprocess.run(
        olddefcmd,
        cwd=str(env.linux_dir),
        shell=True,
        capture_output=True,
        text=True,
        timeout=max(300, env.timeout),
        env=process_env,
    )
    logs.append(f"$ {olddefcmd}\n{proc.stdout or ''}{proc.stderr or ''}")
    if proc.returncode != 0:
        return False, "\n".join(logs)
    return True, "\n".join(logs)


def parse_scan_bug_count(output: str) -> int:
    matches = re.findall(r"scan-build:\s*([0-9]+)\s+bugs?\s+found", output or "", flags=re.IGNORECASE)
    if matches:
        return int(matches[-1])
    if "No bugs found" in (output or ""):
        return 0
    custom_warning_count = len(
        re.findall(
            r"warning: .+?\[custom\.[A-Za-z_]\w*Checker\]",
            output or "",
            flags=re.IGNORECASE,
        )
    )
    if custom_warning_count:
        return custom_warning_count
    return 0


def scan_build_analysis_completed(output: str) -> bool:
    text = output or ""
    return "scan-build: Analysis run complete." in text or "No bugs found" in text or parse_scan_bug_count(text) > 0


@dataclass
class KnighterValidationSummary:
    success: bool
    diagnostics: List[Dict[str, Any]] = field(default_factory=list)
    error_message: str = ""
    metadata: Dict[str, Any] = field(default_factory=dict)


def run_knighter_validation(
    env: KnighterE2Config,
    *,
    checker_so_path: str,
    target_path: str,
    patch_path: str = "",
    scan_build_path: Optional[Path] = None,
) -> KnighterValidationSummary:
    ok, message = validate_knighter_environment(env, require_plugin_tree=True)
    if not ok:
        return KnighterValidationSummary(False, error_message=message, metadata={"environment_blocked": True})
    if not env.plugin_so.exists():
        return KnighterValidationSummary(False, error_message=f"Knighter plugin .so not found: {env.plugin_so}", metadata={"environment_blocked": True})

    patch_file = Path(patch_path).expanduser().resolve() if patch_path else None
    patch_text = patch_file.read_text(encoding="utf-8", errors="ignore") if patch_file and patch_file.is_file() else ""
    commit_id = extract_commit_id_from_patch(patch_text) or env.scan_commit
    objects = objects_from_patch(env, patch_text)
    if not objects and target_path:
        target = Path(target_path)
        if target.is_file() and target.suffix == ".c":
            try:
                rel = str(target.resolve().relative_to(env.linux_dir.resolve()))
            except ValueError:
                rel = str(target)
            objects = [object_for_source(env.knighter_root, rel)]
    objects = _dedupe(objects)
    if not objects:
        return KnighterValidationSummary(False, error_message="No Linux object targets found from patch", metadata={"commit_id": commit_id})

    run_root = Path(env.result_dir or Path(checker_so_path).parent / "knighter_scan").expanduser().resolve()
    run_root.mkdir(parents=True, exist_ok=True)
    run_id = time.strftime("%Y%m%d_%H%M%S")
    output_root = run_root / f"validation_{run_id}"
    buggy_dir = output_root / "buggy"
    fixed_dir = output_root / "fixed"
    buggy_dir.mkdir(parents=True, exist_ok=True)
    fixed_dir.mkdir(parents=True, exist_ok=True)

    logs: List[str] = []
    buggy_counts: Dict[str, int] = {}
    fixed_counts: Dict[str, int] = {}
    diagnostics: List[Dict[str, Any]] = []

    ok, text = git_checkout_and_configure(env, commit_id, before=True, output_dir=buggy_dir)
    logs.append(text)
    if not ok:
        log_path = output_root / "knighter_validation.log"
        log_path.write_text("\n\n".join(logs), encoding="utf-8", errors="ignore")
        return KnighterValidationSummary(False, error_message="Knighter buggy checkout/configure failed", metadata={"log_path": str(log_path), "commit_id": commit_id})

    prefix = knighter_scan_prefix(env, buggy_dir, scan_build_path=scan_build_path)
    process_env = build_knighter_process_env(env)
    for obj in objects:
        cmd = prefix + f"make LLVM=1 ARCH={env.arch} {shlex.quote(obj)} -j{max(1, env.jobs)}"
        proc = subprocess.run(
            cmd,
            cwd=str(env.linux_dir),
            shell=True,
            capture_output=True,
            text=True,
            timeout=max(300, env.timeout),
            env=process_env,
        )
        output = (proc.stdout or "") + ("\n" + proc.stderr if proc.stderr else "")
        logs.append(f"$ {cmd}\n{output}")
        if proc.returncode != 0 and not scan_build_analysis_completed(output):
            log_path = output_root / "knighter_validation.log"
            log_path.write_text("\n\n".join(logs), encoding="utf-8", errors="ignore")
            return KnighterValidationSummary(False, error_message=f"Knighter buggy object build failed: {obj}", metadata={"log_path": str(log_path), "return_code": proc.returncode, "object": obj, "commit_id": commit_id})
        buggy_counts[obj] = parse_scan_bug_count(output)

    ok, text = git_checkout_and_configure(env, commit_id, before=False, output_dir=fixed_dir)
    logs.append(text)
    if not ok:
        log_path = output_root / "knighter_validation.log"
        log_path.write_text("\n\n".join(logs), encoding="utf-8", errors="ignore")
        return KnighterValidationSummary(False, error_message="Knighter fixed checkout/configure failed", metadata={"log_path": str(log_path), "commit_id": commit_id})

    prefix = knighter_scan_prefix(env, fixed_dir, scan_build_path=scan_build_path)
    for obj in objects:
        cmd = prefix + f"make LLVM=1 ARCH={env.arch} {shlex.quote(obj)} -j{max(1, env.jobs)}"
        proc = subprocess.run(
            cmd,
            cwd=str(env.linux_dir),
            shell=True,
            capture_output=True,
            text=True,
            timeout=max(300, env.timeout),
            env=process_env,
        )
        output = (proc.stdout or "") + ("\n" + proc.stderr if proc.stderr else "")
        logs.append(f"$ {cmd}\n{output}")
        if proc.returncode != 0 and not scan_build_analysis_completed(output):
            log_path = output_root / "knighter_validation.log"
            log_path.write_text("\n\n".join(logs), encoding="utf-8", errors="ignore")
            return KnighterValidationSummary(False, error_message=f"Knighter fixed object build failed: {obj}", metadata={"log_path": str(log_path), "return_code": proc.returncode, "object": obj, "commit_id": commit_id})
        fixed_counts[obj] = parse_scan_bug_count(output)

    for obj, count in buggy_counts.items():
        if count > 0:
            diagnostics.append({
                "file_path": obj,
                "line": 0,
                "column": 0,
                "severity": "warning",
                "message": f"Knighter scan-build reported {count} bug(s) on buggy object",
                "source": "csa",
            })

    success = any(count > 0 for count in buggy_counts.values()) and all(
        fixed_counts.get(obj, 0) == 0 or fixed_counts.get(obj, 0) < buggy_counts.get(obj, 0)
        for obj in objects
    )
    log_path = output_root / "knighter_validation.log"
    log_path.write_text("\n\n".join(logs), encoding="utf-8", errors="ignore")
    return KnighterValidationSummary(
        success=success,
        diagnostics=diagnostics,
        error_message="" if success else "Knighter TP/TN validation failed",
        metadata={
            "compile_mode": "knighter_scan_build_make",
            "commit_id": commit_id,
            "objects": objects,
            "buggy_counts": buggy_counts,
            "fixed_counts": fixed_counts,
            "log_path": str(log_path),
            "output_root": str(output_root),
            "checker_so_path": str(checker_so_path),
            "scan_prefix": knighter_scan_prefix(env, output_root, scan_build_path=scan_build_path),
            "environment_blocked": False,
        },
    )


def knighter_helper_context(config: Dict[str, Any] | None) -> str:
    env = load_knighter_e2_config(config)
    if not env.enabled:
        return ""
    lines = [
        "Knighter E2 environment:",
        f"- LLVM_dir: {env.llvm_dir}",
        f"- linux_dir: {env.linux_dir}",
        f"- host_deps_dir: {env.host_deps_dir}",
        f"- validation command shape: scan-build --use-cc=clang -load-plugin <plugin>.so -enable-checker custom.<Plugin>Checker make LLVM=1 ARCH={env.arch} <object>",
        f"- checker plugin build shape: write clang/lib/Analysis/plugins/{env.checker_name}Handling/{env.checker_name}Checker.cpp, then make {env.checker_name}Plugin in LLVM build dir",
    ]
    if env.utility_header.exists():
        lines.append(f"- Knighter utility header: {env.utility_header}")
    if env.utility_source.exists():
        lines.append(f"- Knighter utility implementation: {env.utility_source}")
    lines.extend(
        [
            "Available Knighter CSA helper APIs from clang/StaticAnalyzer/Checkers/utility.h:",
            "- bool EvaluateExprToInt(llvm::APSInt &EvalRes, const Expr *expr, CheckerContext &C)",
            "- const llvm::APSInt *inferSymbolMaxVal(SymbolRef Sym, CheckerContext &C)",
            "- bool getArraySizeFromExpr(llvm::APInt &ArraySize, const Expr *E)",
            "- bool getStringSize(llvm::APInt &StringSize, const Expr *E)",
            "- const MemRegion *getMemRegionFromExpr(const Expr *E, CheckerContext &C)",
            "- template <typename T> const T *findSpecificTypeInParents(const Stmt *S, CheckerContext &C)",
            "- template <typename T> const T *findSpecificTypeInChildren(const Stmt *S)",
            "- bool functionKnownToDeref(const CallEvent &Call, llvm::SmallVectorImpl<unsigned> &DerefParams)",
            "- bool ExprHasName(const Expr *E, StringRef Name, CheckerContext &C)",
            "Use these helpers directly when they simplify AST/source-text matching or region extraction; they are part of the Knighter build, not upstream LLVM.",
        ]
    )
    return "\n".join(lines)
