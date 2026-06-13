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
# Upstream: Keylost/jetson-ffmpeg#21
# Fix:      vendor/mmapi/NvV4l2ElementPlane.cpp (pthread_join guard)
# shellcheck source=test/gen-samples.sh
set -eu

. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/gen-samples.sh"

variant="${JETSON_VARIANT:-unknown}"
echo "=== hw-pthread-guard on variant: ${variant} ==="

# === 1. Rapid decoder open/close (H.264) ===
# Decode a minimal number of frames and exit immediately. Repeat 100 times.
# If pthread_join hits an uninitialized/zero dq_thread, ffmpeg dies with
# signal 11 (SIGSEGV). A clean exit (rc=0) means the guard works.
echo "== 1. rapid decoder open/close x100 (h264_nvmpi) =="

ITERATIONS=100
for i in $(seq 1 $ITERATIONS); do
  rc=0
  out=$(ffmpeg -y -hide_banner -c:v h264_nvmpi -i "$SAMPLE_H264_720P" \
    -frames:v 1 -f null /dev/null 2>&1) || rc=$?

  if [ "$rc" -ge 128 ]; then
    sig=$((rc - 128))
    echo "FAIL: iteration ${i}/${ITERATIONS} killed by signal ${sig} (likely SIGSEGV from pthread_join on uninitialized dq_thread)."
    echo "--- ffmpeg output (last 15 lines) ---"
    echo "$out" | tail -15
    exit 1
  fi

  if [ "$rc" -ne 0 ]; then
    echo "FAIL: iteration ${i}/${ITERATIONS} exited with rc=${rc}."
    echo "--- ffmpeg output (last 15 lines) ---"
    echo "$out" | tail -15
    exit 1
  fi
done
echo "  ${ITERATIONS} iterations clean."

# === 2. Rapid encoder open/close (H.264) ===
# Encode a single frame from lavfi and exit. The encoder path uses
# capture_plane.stopDQThread()/waitForDQThread() which also hits the bug.
echo "== 2. rapid encoder open/close x100 (h264_nvmpi) =="

for i in $(seq 1 $ITERATIONS); do
  rc=0
  out=$(ffmpeg -y -hide_banner \
    -f lavfi -i "color=c=black:s=320x240:d=0.04:r=25" \
    -c:v h264_nvmpi -frames:v 1 -f null /dev/null 2>&1) || rc=$?

  if [ "$rc" -ge 128 ]; then
    sig=$((rc - 128))
    echo "FAIL: encoder iteration ${i}/${ITERATIONS} killed by signal ${sig}."
    echo "--- ffmpeg output (last 15 lines) ---"
    echo "$out" | tail -15
    exit 1
  fi

  if [ "$rc" -ne 0 ]; then
    echo "FAIL: encoder iteration ${i}/${ITERATIONS} exited with rc=${rc}."
    echo "--- ffmpeg output (last 15 lines) ---"
    echo "$out" | tail -15
    exit 1
  fi
done
echo "  ${ITERATIONS} iterations clean."

echo "OK: hw-pthread-guard passed on ${variant}."
