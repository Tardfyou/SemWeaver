#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROFILE_PATH="${ROOT_DIR}/experiments/samples/knighter/profiles/smoke.csv"
SAMPLE_ROOT="${SEMWEEVER_KNIGHTER_SAMPLE_ROOT:-${ROOT_DIR}/artifacts/research/knighter/cases}"
SMOKE_ROOT="${ROOT_DIR}/artifacts/experiments/knighter/smoke"
MANIFEST_DIR="${SMOKE_ROOT}/manifests"
WORKTREE_ROOT="${SMOKE_ROOT}/worktrees"
CONFIG_DIR="${ROOT_DIR}/config"
RUNTIME_CONFIG="${CONFIG_DIR}/__tmp_knighter_smoke.yaml"
SAMPLE_ID="${SEMWEEVER_KNIGHTER_SMOKE_SAMPLE:-08_768f17fd25e4_Integer_Overflow}"

usage() {
  cat <<'EOF'
Usage: scripts/knighter_smoke.sh <setup|audit|run|run-full|clean>

Commands:
  setup     Stage the sample-only smoke workspace under artifacts/.
  audit     Stage, then run SemWeaver's sample audit.
  run       Stage, audit, then run the smoke generate step.
  run-full  Stage, audit, then run generate + refine for the smoke sample.
  clean     Remove the staged smoke workspace.

Environment overrides:
  SEMWEEVER_KNIGHTER_SMOKE_SAMPLE  Sample id to run from the smoke profile.
  SEMWEEVER_KNIGHTER_SAMPLE_ROOT    Root containing artifact case directories.
  KNIGHTER_ROOT                    Knighter baseline root.
  KNIGHTER_LLVM_DIR                LLVM source/build root used by Knighter.
  KNIGHTER_LINUX_DIR               Linux checkout used by Knighter validation.
  KNIGHTER_HOST_DEPS_DIR           Host dependency sysroot.
EOF
}

require_profile() {
  if [[ ! -f "${PROFILE_PATH}" ]]; then
    echo "Smoke profile not found: ${PROFILE_PATH}" >&2
    exit 1
  fi
}

stage_sources() {
  local case_dir="${SAMPLE_ROOT}/${SAMPLE_ID}"
  local src_vuln="${case_dir}/source/vulnerable"
  local src_fixed="${case_dir}/source/fixed"
  local dst_vuln="${WORKTREE_ROOT}/${SAMPLE_ID}/vulnerable"
  local dst_fixed="${WORKTREE_ROOT}/${SAMPLE_ID}/fixed"

  if [[ ! -d "${case_dir}" ]]; then
    echo "Smoke sample environment not found: ${case_dir}" >&2
    echo "Install or unpack the artifact sample package under artifacts/research/knighter/cases," >&2
    echo "or set SEMWEEVER_KNIGHTER_SAMPLE_ROOT to a directory containing the case." >&2
    exit 1
  fi

  if [[ ! -d "${src_vuln}" || ! -d "${src_fixed}" ]]; then
    echo "Smoke source stubs are incomplete under: ${case_dir}" >&2
    echo "Expected source/vulnerable and source/fixed from the external artifact package." >&2
    exit 1
  fi

  mkdir -p "${MANIFEST_DIR}" "${CONFIG_DIR}" "${SMOKE_ROOT}/runs" "${SMOKE_ROOT}/audits" "${SMOKE_ROOT}/tables"
  rm -rf "${dst_vuln}" "${dst_fixed}"
  mkdir -p "${dst_vuln}" "${dst_fixed}"
  cp -R "${src_vuln}/." "${dst_vuln}/"
  cp -R "${src_fixed}/." "${dst_fixed}/"
  cp "${PROFILE_PATH}" "${MANIFEST_DIR}/samples.csv"
}

write_runtime_config() {
  python3 - "${ROOT_DIR}" "${RUNTIME_CONFIG}" <<'PY'
import os
import sys
from pathlib import Path

import yaml

root = Path(sys.argv[1]).resolve()
out = Path(sys.argv[2]).resolve()
base = root / "config" / "config.yaml"
data = yaml.safe_load(base.read_text(encoding="utf-8"))

semantic = data.setdefault("validation", {}).setdefault("semantic", {})
knighter = semantic.setdefault("knighter", {})
knighter["enabled"] = True
knighter["knighter_root"] = os.environ.get(
    "KNIGHTER_ROOT",
    str(root / "experiments" / "knighter" / "baseline"),
)
knighter["llvm_dir"] = os.environ.get(
    "KNIGHTER_LLVM_DIR",
    str(root / "artifacts" / "external" / "llvm"),
)
knighter["linux_dir"] = os.environ.get(
    "KNIGHTER_LINUX_DIR",
    str(root / "artifacts" / "external" / "linux"),
)
knighter["host_deps_dir"] = os.environ.get(
    "KNIGHTER_HOST_DEPS_DIR",
    str(root / "artifacts" / "external" / "host_deps" / "jammy-amd64" / "root"),
)
knighter["result_dir"] = str(root / "artifacts" / "experiments" / "knighter" / "smoke" / "runs" / "knighter-validation")
knighter["checker_name"] = "SAGenTest"

out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(yaml.safe_dump(data, sort_keys=False, allow_unicode=False), encoding="utf-8")
PY
}

check_external_env() {
  local missing=0
  for path in \
    "${KNIGHTER_ROOT:-${ROOT_DIR}/experiments/knighter/baseline}" \
    "${KNIGHTER_LLVM_DIR:-${ROOT_DIR}/artifacts/external/llvm}" \
    "${KNIGHTER_LINUX_DIR:-${ROOT_DIR}/artifacts/external/linux}"; do
    if [[ ! -e "${path}" ]]; then
      echo "Missing external runtime path: ${path}" >&2
      missing=1
    fi
  done
  return "${missing}"
}

setup_smoke() {
  require_profile
  stage_sources
  write_runtime_config
  echo "Smoke workspace staged:"
  echo "  root: ${SMOKE_ROOT}"
  echo "  manifest: ${MANIFEST_DIR}/samples.csv"
  echo "  config: ${RUNTIME_CONFIG}"
}

audit_smoke() {
  setup_smoke
  (cd "${ROOT_DIR}" && python3 -m src.main --config "${RUNTIME_CONFIG}" experiment audit \
    --root "${SMOKE_ROOT}" \
    --manifest "${MANIFEST_DIR}/samples.csv" \
    --sample-id "${SAMPLE_ID}")
}

run_smoke() {
  local generate_only="$1"
  audit_smoke
  if ! check_external_env; then
    echo "External Knighter runtime is incomplete; setup/audit succeeded, run skipped." >&2
    exit 2
  fi
  local args=()
  if [[ "${generate_only}" == "true" ]]; then
    args+=(--generate-only)
  fi
  (cd "${ROOT_DIR}" && python3 -m src.main --config "${RUNTIME_CONFIG}" experiment run \
    --root "${SMOKE_ROOT}" \
    --manifest "${MANIFEST_DIR}/samples.csv" \
    --sample-id "${SAMPLE_ID}" \
    "${args[@]}")
}

case "${1:-}" in
  setup)
    setup_smoke
    ;;
  audit)
    audit_smoke
    ;;
  run)
    run_smoke true
    ;;
  run-full)
    run_smoke false
    ;;
  clean)
    rm -rf "${SMOKE_ROOT}" "${RUNTIME_CONFIG}"
    echo "Removed ${SMOKE_ROOT}"
    echo "Removed ${RUNTIME_CONFIG}"
    ;;
  -h|--help|help|"")
    usage
    ;;
  *)
    usage >&2
    exit 1
    ;;
esac
