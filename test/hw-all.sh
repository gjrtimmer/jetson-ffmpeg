#!/usr/bin/env bash
# Run every hw-* test suite on the ffmpeg found in PATH (real Jetson only).
#
# Suites are auto-discovered (test/**/hw-*.sh, this runner excluded) so adding a
# new suite file requires no changes here. hw-smoke always runs first, the rest
# in sorted order (stable job logs). All shared input samples are generated once
# up front (gen-all); suites do not regenerate them.
#
# Usage:
#   JETSON_VARIANT=orin-nx test/hw-all.sh           # all suites
#   HW_SUITES="decoder-chunk encoder-gop" test/hw-all.sh   # subset
#   HW_SUITES_EXCLUDE="soak-decode encoder-modes" test/hw-all.sh  # skip some
#     (CI fast tier: only soak-decode is excluded, new suites run by default)
#
# Mock mode (CI test:stub, no Jetson): NVMPI_MOCK=1 runs only the mockable
# suites (MOCK_SUITES below) unless HW_SUITES is given. See test/README.md
# "Mock (stub) runs". The mock is ADDITIVE: every suite below also runs on the
# Orin, and a green mock run never substitutes for a hardware run.
#   NVMPI_MOCK=1 test/hw-all.sh
#
# CI log contract (what you see when something breaks):
#   - PASSING suites are wrapped in collapsed GitLab sections (noise hidden).
#   - A FAILING suite's full output is printed UNCOLLAPSED, followed by a
#     copy-paste rerun hint. Suites are responsible for printing actionable
#     evidence on FAIL (observed values, captured ffmpeg output, pointers to
#     the code under test) — see test/README.md "Failure style".
#   - The run ends with a per-suite result block (verdict + seconds), one suite
#     per line, and lists any process stuck in D-state (driver hang).
#   - A suite exceeding HW_SUITE_TIMEOUT is hard-killed (timeout -k) and the
#     D-state processes are reported right then, so a driver-stuck hang is
#     visible instead of silently eating the job timeout.
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
KILL_GRACE="${HW_SUITE_KILL_GRACE:-10}"

# Suites now live under component subdirectories (test/decoder/, test/encoder/,
# test/filter/, test/perf/, test/format/, test/integration/) instead of flat in
# test/ — recurse to find them all. `find` excludes hw-all.sh itself by name.
discover() {
  local s f
  echo smoke
  while IFS= read -r f; do
    s="$(basename "$f" .sh)"; s="${s#hw-}"
    case "$s" in all|smoke) continue ;; esac
    echo "$s"
  done < <(find "${SCRIPT_DIR}" -name 'hw-*.sh' ! -name 'hw-all.sh' | sort)
}
# Suites that give meaningful results on the mock (opt-in: a new suite is NOT
# mockable until listed here). Excluded, with the reason:
#   decoder-mjpeg, decoder-mjpeg-drmprime,
#   encoder-jpeg                  NvJPEG is not mocked (link-time stub only)
#   encoder-level                 the mock encoder emits a canned SPS (level 4.0)
#   soak-decode, perf-bench,
#   perf-mode                     timing/throughput numbers need the engines
MOCK_SUITES="smoke decoder-blocking-wait decoder-chunk decoder-codecs
  decoder-downscale decoder-flush decoder-lifecycle decoder-pool encoder-gop
  encoder-header encoder-modes encoder-pools encoder-thread-safety
  filter-scale-vic format-pixfmt lifecycle-stress rtp-sdp"
if [ "${NVMPI_MOCK:-0}" = "1" ]; then
  : "${HW_SUITES:=${MOCK_SUITES}}"
  SUITE_TIMEOUT="${HW_SUITE_TIMEOUT:-120}"
  echo "[i] NVMPI_MOCK=1: running against the nvmpi mock (no Jetson hardware)"
fi
SUITES="${HW_SUITES:-$(discover | tr '\n' ' ')}"

# Suite name -> script path. Suites can live in any subdirectory now, so
# resolve by filename search instead of assuming a fixed parent directory.
resolve_script() {
  find "${SCRIPT_DIR}" -name "hw-${1}.sh" -print -quit
}

# Identify the binary under test — proves WHICH ffmpeg build a CI log
# exercised (PATH-injected per-version trees all ship their own binary).
echo "ffmpeg under test: $(command -v ffmpeg)"
ffmpeg -version 2>/dev/null | head -1
echo "variant: ${variant} | suites: ${SUITES}"

# d_state_report: processes in uninterruptible sleep (D) are stuck in the
# kernel driver and cannot be killed; show them so a hang is attributable.
d_state_report() {
  local stuck
  stuck="$(ps -eo pid,stat,etime,comm 2>/dev/null | awk '$2 ~ /^D/' || true)"
  if [ -n "$stuck" ]; then
    echo "[W] processes in D-state (uninterruptible, driver-stuck):"
    printf '%s\n' "$stuck"
  fi
}

# Generate all shared samples once per job (suites must not regenerate them).
# shellcheck source-path=SCRIPTDIR
# shellcheck source=gen-samples.sh
. "${SCRIPT_DIR}/gen-samples.sh"
gen-all

section_start() { [ -n "${GITLAB_CI:-}" ] && printf '\e[0Ksection_start:%s:%s[collapsed=true]\r\e[0K' "$2" "$1" || true; }
section_end()   { [ -n "${GITLAB_CI:-}" ] && printf '\e[0Ksection_end:%s:%s\r\e[0K' "$(date +%s)" "$1" || true; }

results="" fail=0 t_all="$(date +%s)"
for s in $SUITES; do
  case " ${HW_SUITES_EXCLUDE:-} " in *" $s "*) echo "[i] skipping suite ${s} (HW_SUITES_EXCLUDE)"; continue ;; esac
  script="$(resolve_script "$s")"
  if [ -z "$script" ] || [ ! -f "$script" ]; then
    echo "[E] unknown suite: ${s} (no ${script})" >&2
    results="${results}${s}:MISSING:0\n"
    fail=1
    continue
  fi
  ts_before="$(date +%s)"
  rc=0
  secs=0
  out="$(timeout --kill-after="${KILL_GRACE}" "${SUITE_TIMEOUT}" bash "$script" 2>&1)" || rc=$?
  secs=$(( $(date +%s) - ts_before ))
  if [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; then
    echo "######## suite: hw-${s} — TIMEOUT (${SUITE_TIMEOUT}s) ########"
    printf '%s\n' "$out"
    echo "######## suite hung — killed after ${SUITE_TIMEOUT}s ########"
    echo "[hint] rerun with longer timeout: HW_SUITE_TIMEOUT=900 HW_SUITES=\"${s}\" test/hw-all.sh"
    d_state_report
    results="${results}${s}:TIMEOUT:${secs}\n"
    fail=1
    continue
  fi
  if [ "$rc" -eq 0 ]; then
    # Passing suite: full log inside a collapsed section.
    section_start "hw_${s//-/_}" "$ts_before"
    echo "######## suite: hw-${s} — PASS ########"
    printf '%s\n' "$out"
    section_end "hw_${s//-/_}"
    results="${results}${s}:PASS:${secs}\n"
  else
    # Failing suite: full output stays visible — this is what you debug from.
    echo "######## suite: hw-${s} — FAIL (rc=${rc}) ########"
    printf '%s\n' "$out"
    echo "######## end of failing suite hw-${s} ########"
    echo "[hint] rerun just this suite: HW_SUITES=\"${s}\" test/hw-all.sh"
    d_state_report
    results="${results}${s}:FAIL:${secs}\n"
    fail=1
  fi
done

echo ""
echo "suite results (${variant}):"
printf '%b' "$results" | while IFS=: read -r name verdict secs; do
  printf '  %-26s %-8s %5ss\n' "$name" "$verdict" "$secs"
done
echo "total: $(( $(date +%s) - t_all ))s"
d_state_report
if [ "$fail" -ne 0 ]; then
  echo "FAIL: one or more hw suites failed on ${variant}."
  exit 1
fi
echo "OK: all hw suites passed on ${variant}."
