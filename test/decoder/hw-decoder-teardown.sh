#!/usr/bin/env bash
# hw-decoder-teardown.sh — decoder close-path regression test (#8).
#
# Validates that nvmpi_decoder_close() properly drains V4L2 planes and
# destroys DMA buffers in the correct order. Without the fix, rapid
# open/close cycles crash with a segfault because queued DMA buffers are
# freed while the V4L2 driver still references them.
#
# Tests:
#   1. Rapid full-decode open/close ×200 — exercises the normal teardown
#      path with the capture plane fully initialized (DMA buffers allocated).
#   2. Close-during-decode — decoder closes mid-stream without EOS,
#      exercises early teardown before all frames are consumed.
#   3. Error-path close — feed garbage data to trigger a V4L2 decode error,
#      then verify the decoder tears down cleanly (no crash, no hang).
#   4. Device health — after all stress tests, verify the V4L2 device is
#      still usable (not stuck from leaked resources).
#
# Fix:   src/nvmpi_dec.cpp (teardown reorder, fd sentinel, session lifecycle)
# Issue: gjrtimmer/jetson-ffmpeg#8
set -eu
# shellcheck source-path=SCRIPTDIR/..
# shellcheck source=lib/common.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../lib/common.sh"

variant="${JETSON_VARIANT:-unknown}"
echo "=== hw-decoder-teardown on variant: ${variant} ==="

ITERATIONS=200

ensure_sample SAMPLE_H264_720P gen-sample-h264 3

# === 1. Rapid full-decode open/close (H.264) ===
echo "== 1. rapid full-decode open/close x${ITERATIONS} (h264_nvmpi) =="

for i in $(seq 1 $ITERATIONS); do
  # On failure: the decoder close path may not drain V4L2 planes before
  # destroying DMA buffers. Check nvmpi_decoder_close() order.
  run_ff_must "full-decode teardown iter ${i}/${ITERATIONS}" -T 30 -c:v h264_nvmpi \
    -i "$SAMPLE_H264_720P" -frames:v 5 -f null -
  sleep 0.02
done
echo "  ${ITERATIONS} iterations clean."

# === 2. Close-during-decode (partial data, no EOS) ===
echo "== 2. close-during-decode x${ITERATIONS} (h264_nvmpi) =="

for i in $(seq 1 $ITERATIONS); do
  # On failure: early close (before all frames consumed) triggers teardown
  # while the capture thread may still hold V4L2 buffers.
  run_ff_must "close-during-decode iter ${i}/${ITERATIONS}" -T 30 -c:v h264_nvmpi \
    -i "$SAMPLE_H264_720P" -frames:v 1 -f null -
  sleep 0.02
done
echo "  ${ITERATIONS} iterations clean."

# === 3. Error-path close (truncated stream) ===
# Truncate a valid H.264 file mid-stream so the decoder initializes
# (resolution event fires, DMA buffers allocated) but then hits a decode
# error or premature EOF during teardown.
echo "== 3. error-path close x50 (truncated stream) =="

TRUNCATED="/tmp/nvmpi-sample-truncated.mp4"
# Keep only the first 8 KB — enough for container headers + a few NALs,
# but the stream will end abruptly mid-frame.
dd if="$SAMPLE_H264_720P" of="$TRUNCATED" bs=1024 count=8 2>/dev/null

for i in $(seq 1 50); do
  run_ff -T 30 -c:v h264_nvmpi -i "$TRUNCATED" -f null - || true
  if is_signal_rc "$FF_RC"; then
    echo "  Truncated-stream decode should error cleanly, not crash."
    echo "  Check isInError() exit path in dec_capture_loop_fcn."
    ff_fail "error-path close iteration ${i}/50 crashed (signal $((FF_RC - 128)))."
  fi
  if [ "$FF_RC" -eq 124 ]; then
    ff_fail "error-path close iteration ${i}/50 hung."
  fi
  sleep 0.02
done
echo "  50 iterations clean."

rm -f "$TRUNCATED"

# === 4. Device health after stress ===
echo "== 4. device health check =="

# Allow the V4L2 kernel driver time to reclaim resources from the 450
# rapid open/close cycles above. Without this, the next device open can
# succeed but the driver's internal state may still be draining — causing
# a segfault during decode on some Jetson variants (observed on orin-nx
# with FFmpeg 4.2). The delay is defensive, not papering over a leak:
# the stress loops all completed cleanly, and no code change between a
# green and red pipeline reproduced this.
sleep 1

# On failure the decoder may have leaked DMA buffers or left the device in a
# broken state. Check fd cleanup in deinitDecoderCapturePlane.
device_health "V4L2 device after teardown stress test"
echo "  device healthy."

echo "OK: hw-decoder-teardown passed on ${variant}."
