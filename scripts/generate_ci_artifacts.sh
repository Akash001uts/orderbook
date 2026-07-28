#!/usr/bin/env bash
#
# Regenerates the artifact set that CI can reproduce, from the one raw capture
# this repository commits.
#
# This exists so the results site provably cannot drift from the code. The site
# reads committed JSON; a job runs this script and diffs its output against those
# committed files; a change to a tool that alters a number therefore fails the
# build rather than silently changing the site. Same philosophy as the regression
# fixtures in test/.
#
# It covers QQQ on the committed slice only. Every other symbol on the site is
# derived from a 7.68 GB full day capture that lives on one machine and cannot be
# fetched by a runner, which is why those artifacts carry provenance instead of a
# guard. See site/data/SCHEMA.md.
#
# Usage:
#   scripts/generate_ci_artifacts.sh [output-directory]
#
# Environment:
#   OB_BIN    directory holding the built tools, default out/build/gcc-release
#   OB_SLICE  the committed capture, default data/qqq_slice.itch
#
# Paths passed to the tools are repository relative on purpose. They are recorded
# verbatim in each artifact's provenance block, and an absolute path would differ
# between a developer's machine and a runner, failing a byte comparison for a
# reason that has nothing to do with the code.

set -euo pipefail

cd "$(dirname "$0")/.."

OUT_DIR="${1:-site/data/ci}"
OB_BIN="${OB_BIN:-out/build/gcc-release}"
OB_SLICE="${OB_SLICE:-data/qqq_slice.itch}"
SYMBOL="QQQ"

# The tools are .exe on the MSYS2 host and extensionless on a runner. Resolving it
# here keeps the script identical in both places.
resolve() {
  local name="$1"
  if [[ -x "${OB_BIN}/${name}" ]]; then
    printf '%s/%s' "${OB_BIN}" "${name}"
  elif [[ -x "${OB_BIN}/${name}.exe" ]]; then
    printf '%s/%s.exe' "${OB_BIN}" "${name}"
  else
    echo "cannot find ${name} in ${OB_BIN}" >&2
    exit 1
  fi
}

REPLAY="$(resolve itch_replay)"
BACKTEST="$(resolve ob_strategy_backtest)"

if [[ ! -f "${OB_SLICE}" ]]; then
  echo "missing ${OB_SLICE}" >&2
  exit 1
fi

SHA="$(sha256sum "${OB_SLICE}" | cut -d' ' -f1)"

mkdir -p "${OUT_DIR}"

echo "regenerating ${SYMBOL} artifacts from ${OB_SLICE} into ${OUT_DIR}"

"${REPLAY}" \
  --symbol "${SYMBOL}" \
  --json "${OUT_DIR}/replay.json" \
  --json-depth "${OUT_DIR}/depth.json" \
  --sha256 "${SHA}" \
  "${OB_SLICE}" >/dev/null

"${BACKTEST}" \
  --file "${OB_SLICE}" \
  --symbol "${SYMBOL}" \
  --json "${OUT_DIR}/backtest.json" \
  --sha256 "${SHA}" >/dev/null

# Both grids in one document. Twenty five full replays of the slice, which is a
# few seconds at 5.5 MB and is the reason this guard uses the slice rather than a
# full trading day.
"${BACKTEST}" \
  --file "${OB_SLICE}" \
  --symbol "${SYMBOL}" \
  --sweep \
  --latency-sweep \
  --json "${OUT_DIR}/sweeps.json" \
  --sha256 "${SHA}" >/dev/null

echo "wrote replay.json depth.json backtest.json sweeps.json"
