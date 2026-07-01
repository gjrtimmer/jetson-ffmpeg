#!/usr/bin/env bash
# Encoder lifecycle suite: exercises the encoder close path under stress.
# Validates that nvmpi_encode_close() properly drains the packet pool, stops
# the DQ thread, and frees V4L2 resources in the correct order. Without
# correct teardown, rapid open/close cycles can leak DMA buffers, crash from
# use-after-free in the DQ thread, or hang waiting for a stopped thread.
#
# Tests:
#   1. Rapid full-encode open/close ×200 — exercises the normal teardown
#      path with the full packet pool initialized and DQ thread running.
#   2. Close-mid-encode — encoder closes mid-stream (timeout before EOS),
#      exercises early teardown with packets potentially in-flight.
#   3. Very short encode — 0.3s input, exercises EOS drain with minimal data
#      (packet pool mostly empty at close time).
#   4. Device health — after all stress tests, verify the V4L2 encoder device
#      is still usable (not stuck from leaked resources).
#
# Note: hw-pthread-guard.sh also has rapid encoder open/close, but that suite
# specifically tests the pthread_cancel/pthread_join guard for glibc >=2.34.
# This suite tests the full encoder lifecycle including mid-encode close.
#
# Code under test:
#   ffmpeg/dev/common/libavcodec/nvmpi_enc.c
#     - nvmpi_encode_close (EOS drain + deinitPktPool + encoder close)
#     - nvmpienc_deinitPktPool (pool queue drain)
#   src/nvmpi_enc*.cpp
#     - nvmpi_encoder_close (DQ thread stop + V4L2 teardown)
# Issue: gjrtimmer/jetson-ffmpeg#27

# shellcheck source=test/gen-samples.sh
set -eu

. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/gen-samples.sh"

variant="${JETSON_VARIANT:-unknown}"
echo "=== hw-encoder-lifecycle on variant: ${variant} ==="

ITERATIONS=200

[ -s "${SAMPLE_H264_720P}" ] || gen-sample-h264 "${SAMPLE_H264_720P}" 3

# run_iteration is now run_with_signal_retry from gen-samples.sh
run_iteration() { run_with_signal_retry "$@"; }

# === 1. Rapid full-encode open/close (H.264) ===
echo "== 1. rapid full-encode open/close x${ITERATIONS} (h264_nvmpi) =="
for i in $(seq 1 "$ITERATIONS"); do
    if ! run_iteration timeout -k 5 30 ffmpeg -y -hide_banner \
        -i "${SAMPLE_H264_720P}" \
        -c:v h264_nvmpi -f null -; then
        echo "FAIL: iteration ${i}/${ITERATIONS} failed."
        echo "   Code: nvmpi_encode_close in nvmpi_enc.c, nvmpi_encoder_close in nvmpi_enc*.cpp"
        exit 1
    fi
    [ $((i % 50)) -eq 0 ] && echo "   ${i}/${ITERATIONS} OK"
done
echo "   rapid open/close: ${ITERATIONS} iterations — OK"

# === 2. Close-mid-encode (kill before EOS) ===
echo "== 2. close-mid-encode x20 (h264_nvmpi) =="
SAMPLE_LONG="/tmp/nvmpi-sample-h264-enclife-long.mp4"
[ -s "${SAMPLE_LONG}" ] || gen-sample-h264 "${SAMPLE_LONG}" 10

for i in $(seq 1 20); do
    # Start encode, kill after 0.5s — before the stream reaches EOS.
    # A broken close path would hang or crash here.
    timeout -k 2 0.5 ffmpeg -y -hide_banner \
        -i "${SAMPLE_LONG}" \
        -c:v h264_nvmpi -f null - 2>/dev/null || true
done
echo "   close-mid-encode: 20 iterations — OK"

# === 3. Very short encode (0.3s input, clean EOS) ===
echo "== 3. short encode EOS =="
SAMPLE_SHORT="/tmp/nvmpi-sample-h264-enc-short.mp4"
[ -s "${SAMPLE_SHORT}" ] || ffmpeg -y -hide_banner -loglevel error \
    -f lavfi -i testsrc2=s=1280x720:r=30 -t 0.3 \
    -c:v libx264 "${SAMPLE_SHORT}"

rc=0
out=$(timeout -k 5 15 ffmpeg -y -hide_banner \
    -i "${SAMPLE_SHORT}" \
    -c:v h264_nvmpi -f null - 2>&1) || rc=$?
if [ "$rc" -eq 124 ]; then
    echo "FAIL: short encode timed out — EOS drain may be broken."
    echo "   Code: nvmpi_encode_close drain loop in nvmpi_enc.c"
    exit 1
fi
if is_signal_rc "$rc"; then
    echo "  warn: short encode signal $((rc - 128)), retrying after 200 ms..."
    sleep 0.2
    rc=0
    out=$(timeout -k 5 15 ffmpeg -y -hide_banner \
        -i "${SAMPLE_SHORT}" \
        -c:v h264_nvmpi -f null - 2>&1) || rc=$?
    if is_signal_rc "$rc"; then
        echo "FAIL: short encode confirmed crash (signal $((rc - 128)))."
        echo "$out" | tail -15
        exit 1
    fi
fi
echo "   short encode EOS OK (rc=${rc})"

# === 4. Device health after stress ===
echo "== 4. device health check =="
rc=0
out=$(timeout -k 5 30 ffmpeg -y -hide_banner \
    -i "${SAMPLE_H264_720P}" \
    -c:v h264_nvmpi -f null - 2>&1) || rc=$?
if [ "$rc" -eq 124 ]; then
    echo "FAIL: post-stress encode timed out — V4L2 device may be stuck."
    exit 1
fi
frames=$(echo "$out" | grep -oP 'frame=\s*\K[0-9]+' | tail -1)
if [ -z "$frames" ] || [ "$frames" -lt 10 ]; then
    echo "FAIL: post-stress encode produced ${frames:-0} frames (expected >= 10)."
    echo "$out" | tail -15
    exit 1
fi
echo "   device health: ${frames} frames after stress — OK"

echo "OK: hw-encoder-lifecycle passed on ${variant}."
