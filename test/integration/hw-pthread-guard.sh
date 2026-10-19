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
set -eu
# shellcheck source-path=SCRIPTDIR/..
# shellcheck source=lib/common.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../lib/common.sh"

variant="${JETSON_VARIANT:-unknown}"
echo "=== hw-pthread-guard on variant: ${variant} ==="

ITERATIONS=100

ensure_sample SAMPLE_H264_720P gen-sample-h264 3

# === 1. Rapid decoder open/close (H.264) ===
echo "== 1. rapid decoder open/close x${ITERATIONS} (h264_nvmpi) =="

for i in $(seq 1 $ITERATIONS); do
  # run_ff retries once on a signal-kill: a repeat crash is deterministic
  # (our pthread_join bug), a single one is a transient V4L2 driver race.
  run_ff_must "decoder iteration ${i}/${ITERATIONS}" -T 30 -c:v h264_nvmpi \
    -i "$SAMPLE_H264_720P" -frames:v 1 -f null -
  sleep 0.05
done
echo "  ${ITERATIONS} iterations clean."

# === 2. Rapid encoder open/close (H.264) ===
echo "== 2. rapid encoder open/close x${ITERATIONS} (h264_nvmpi) =="

for i in $(seq 1 $ITERATIONS); do
  run_ff_must "encoder iteration ${i}/${ITERATIONS}" -T 30 \
    -f lavfi -i "color=c=black:s=320x240:d=0.04:r=25" \
    -c:v h264_nvmpi -frames:v 1 -f null -
  sleep 0.05
done
echo "  ${ITERATIONS} iterations clean."

echo "OK: hw-pthread-guard passed on ${variant}."
