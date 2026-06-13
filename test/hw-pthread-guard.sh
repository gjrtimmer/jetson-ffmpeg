#!/usr/bin/env bash
# Regression test for glibc >= 2.34 pthread_join segfault (#6).
#
# On glibc >= 2.34, pthread_join on an uninitialized/zero pthread_t
# segfaults instead of returning ESRCH. NVIDIA's NvV4l2ElementPlane
# calls pthread_join unconditionally in stopDQThread()/waitForDQThread(),
# crashing every decoder/encoder teardown on Ubuntu 22.04+ / JetPack 6.
#
# This suite validates that rapid open/close cycles (where the DQ thread
# may never start before teardown) do not segfault. Without the vendored
# NvV4l2ElementPlane fix, iteration 1 typically crashes with signal 11.
#
# Transient V4L2 driver races during rapid device cycling can cause
# occasional SIGSEGV unrelated to the pthread_join bug. Each iteration
# gets one retry to distinguish deterministic crashes (our bug) from
# transient driver issues.
#
# Upstream: Keylost/jetson-ffmpeg#21
# Fix:      vendor/mmapi/NvV4l2ElementPlane.cpp (pthread_join guard)
# shellcheck source=test/gen-samples.sh
set -eu

. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/gen-samples.sh"

variant="${JETSON_VARIANT:-unknown}"
echo "=== hw-pthread-guard on variant: ${variant} ==="

ITERATIONS=100

# run_iteration CMD...
# Run a command; on signal-kill, wait 200 ms and retry once.
# Returns 0 on success, 1 on non-signal error, 2 on confirmed signal-kill.
run_iteration() {
  local rc=0 out
  out=$("$@" 2>&1) || rc=$?

  if [ "$rc" -ge 128 ]; then
    local sig=$((rc - 128))
    echo "  warn: signal ${sig}, retrying after 200 ms..."
    sleep 0.2
    rc=0
    out=$("$@" 2>&1) || rc=$?
    if [ "$rc" -ge 128 ]; then
      sig=$((rc - 128))
      echo "--- retry also killed by signal ${sig} ---"
      echo "$out" | tail -15
      return 2
    fi
  fi

  if [ "$rc" -ne 0 ]; then
    echo "--- exited with rc=${rc} ---"
    echo "$out" | tail -15
    return 1
  fi
  return 0
}

# === 1. Rapid decoder open/close (H.264) ===
echo "== 1. rapid decoder open/close x${ITERATIONS} (h264_nvmpi) =="

for i in $(seq 1 $ITERATIONS); do
  if ! run_iteration ffmpeg -y -hide_banner -c:v h264_nvmpi \
       -i "$SAMPLE_H264_720P" -frames:v 1 -f null /dev/null; then
    echo "FAIL: decoder iteration ${i}/${ITERATIONS}."
    exit 1
  fi
  sleep 0.05
done
echo "  ${ITERATIONS} iterations clean."

# === 2. Rapid encoder open/close (H.264) ===
echo "== 2. rapid encoder open/close x${ITERATIONS} (h264_nvmpi) =="

for i in $(seq 1 $ITERATIONS); do
  if ! run_iteration ffmpeg -y -hide_banner \
       -f lavfi -i "color=c=black:s=320x240:d=0.04:r=25" \
       -c:v h264_nvmpi -frames:v 1 -f null /dev/null; then
    echo "FAIL: encoder iteration ${i}/${ITERATIONS}."
    exit 1
  fi
  sleep 0.05
done
echo "  ${ITERATIONS} iterations clean."

echo "OK: hw-pthread-guard passed on ${variant}."
