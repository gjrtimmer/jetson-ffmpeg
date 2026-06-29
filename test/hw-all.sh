#!/usr/bin/env bash
# Run every hw-* test suite on the ffmpeg found in PATH (real Jetson only).
#
# Suites are auto-discovered (test/hw-*.sh, this runner excluded) so adding a
# new suite file requires no changes here. hw-smoke always runs first — when
# basic transcode is broken, nothing else is worth reading.
#
# Usage:
#   JETSON_VARIANT=orin-nx test/hw-all.sh           # all suites
#   HW_SUITES="decoder-chunk encoder-gop" test/hw-all.sh   # subset
#
# CI log contract (what you see when something breaks):
#   - PASSING suites are wrapped in collapsed GitLab sections (noise hidden).
#   - A FAILING suite's full output is printed UNCOLLAPSED, followed by a
#     copy-paste rerun hint. Suites are responsible for printing actionable
#     evidence on FAIL (observed values, captured ffmpeg output, pointers to
#     the code under test) — see test/README.md "Failure style".
#   - The run ends with a per-suite result block, one suite per line.
set -eu

# Serialize hardware access: only one hw-all instance at a time.
# The Tegra V4L2 driver segfaults on device access collision when
# multiple processes fight over NVDEC/NVENC — flock prevents that.
# Uses /tmp so it works across CI jobs and local runs on the same host.
LOCK_FILE="/tmp/nvmpi-hw-test.lock"
exec 9>"${LOCK_FILE}"
if ! flock -n 9; then
  echo "[W] Another hw test run holds ${LOCK_FILE} — waiting..."
  flock 9
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
variant="${JETSON_VARIANT:-unknown}"
# Per-suite wall-clock timeout (seconds). Prevents a single hanging suite from
# eating the entire CI job timeout. Default 600s (10 min) — no individual suite
# should take longer; the longest (perf-bench) runs ~3 min on Orin Nano.
SUITE_TIMEOUT="${HW_SUITE_TIMEOUT:-600}"

discover() {
  local s
  echo smoke
  for s in "${SCRIPT_DIR}"/hw-*.sh; do
    s="$(basename "$s" .sh)"; s="${s#hw-}"
    case "$s" in all|smoke) continue ;; esac
    echo "$s"
  done
}
SUITES="${HW_SUITES:-$(discover | tr '\n' ' ')}"

# Identify the binary under test — proves WHICH ffmpeg build a CI log
# exercised (PATH-injected per-version trees all ship their own binary).
echo "ffmpeg under test: $(command -v ffmpeg)"
ffmpeg -version 2>/dev/null | head -1
echo "variant: ${variant} | suites: ${SUITES}"

section_start() { [ -n "${GITLAB_CI:-}" ] && printf '\e[0Ksection_start:%s:%s[collapsed=true]\r\e[0K' "$2" "$1" || true; }
section_end()   { [ -n "${GITLAB_CI:-}" ] && printf '\e[0Ksection_end:%s:%s\r\e[0K' "$(date +%s)" "$1" || true; }

results="" fail=0
for s in $SUITES; do
  script="${SCRIPT_DIR}/hw-${s}.sh"
  if [ ! -f "$script" ]; then
    echo "[E] unknown suite: ${s} (no ${script})" >&2
    results="${results}${s}:MISSING\n"
    fail=1
    continue
  fi
  ts_before="$(date +%s)"
  rc=0
  out="$(timeout --kill-after=10 "${SUITE_TIMEOUT}" bash "$script" 2>&1)" || rc=$?
  if [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; then
    echo "######## suite: hw-${s} — TIMEOUT (${SUITE_TIMEOUT}s) ########"
    printf '%s\n' "$out"
    echo "######## suite hung — killed after ${SUITE_TIMEOUT}s ########"
    echo "[hint] rerun with longer timeout: HW_SUITE_TIMEOUT=900 HW_SUITES=\"${s}\" test/hw-all.sh"
    results="${results}${s}:TIMEOUT\n"
    fail=1
    continue
  fi
  if [ "$rc" -eq 0 ]; then
    # Passing suite: full log inside a collapsed section.
    section_start "hw_${s//-/_}" "$ts_before"
    echo "######## suite: hw-${s} — PASS ########"
    printf '%s\n' "$out"
    section_end "hw_${s//-/_}"
    results="${results}${s}:PASS\n"
  else
    # Failing suite: full output stays visible — this is what you debug from.
    echo "######## suite: hw-${s} — FAIL (rc=${rc}) ########"
    printf '%s\n' "$out"
    echo "######## end of failing suite hw-${s} ########"
    echo "[hint] rerun just this suite: HW_SUITES=\"${s}\" test/hw-all.sh"
    results="${results}${s}:FAIL\n"
    fail=1
  fi
done

echo ""
echo "suite results (${variant}):"
printf '%b' "$results" | while IFS=: read -r name verdict; do
  printf '  %-18s %s\n' "$name" "$verdict"
done
if [ "$fail" -ne 0 ]; then
  echo "FAIL: one or more hw suites failed on ${variant}."
  exit 1
fi
echo "OK: all hw suites passed on ${variant}."
