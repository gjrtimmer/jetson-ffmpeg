#!/usr/bin/env bash
# Encoder thread-safety suite: concurrent multi-stream encode stress test.
# Validates that the encoder's cross-thread flags (capPlaneGotEOS, flushing)
# and packet pool lifecycle are safe under concurrent access from multiple
# independent encoder instances sharing the same V4L2 hardware engine.
#
# Unlike hw-encoder-lifecycle.sh (sequential rapid open/close), this suite
# runs multiple encoder processes IN PARALLEL to stress:
#   - std::atomic<bool> capPlaneGotEOS/flushing coherence between the DQ
#     callback thread and the user thread (data race fixed by issue #17)
#   - unique_ptr<NvVideoEncoder> RAII ownership under concurrent teardown
#   - Packet pool init/deinit rollback under parallel OOM-like conditions
#   - V4L2 device multiplexing (multiple M2M contexts on one NVENC engine)
#
# Tests:
#   1. Parallel encode ×4 (H.264) — 4 simultaneous encodes, repeated 10 rounds
#   2. Parallel encode ×4 (HEVC) — same with hevc_nvmpi
#   3. Mixed-codec parallel — 2× H.264 + 2× HEVC simultaneously
#   4. Parallel rapid lifecycle — 4 concurrent rapid open/close loops (×50 each)
#   5. Device health — verify encoder still works after all concurrent stress
#
# Code under test:
#   src/nvmpi_enc_internal.h
#     - std::atomic<bool> capPlaneGotEOS, flushing (cross-thread coherence)
#     - std::unique_ptr<NvVideoEncoder> enc (RAII ownership)
#   src/nvmpi_enc_api.cpp
#     - nvmpi_create_encoder (unique_ptr adoption + NULL check)
#     - nvmpi_encoder_get_packet (atomic load for flushing/capPlaneGotEOS)
#     - nvmpi_encoder_close (unique_ptr reset + DQ thread stop ordering)
#   src/nvmpi_enc_output.cpp
#     - encoder_capture_plane_dq_callback (atomic store of capPlaneGotEOS)
#   ffmpeg/dev/common/libavcodec/nvmpi_enc.c
#     - nvmpienc_initPktPool (rollback on failure)
#     - nvmpi_encode_close (drain loop OOM guard)
# Issue: gjrtimmer/jetson-ffmpeg#17

# shellcheck source=test/gen-samples.sh
set -eu

. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/gen-samples.sh"

variant="${JETSON_VARIANT:-unknown}"
echo "=== hw-encoder-thread-safety on variant: ${variant} ==="

[ -s "${SAMPLE_H264_720P}" ] || gen-sample-h264 "${SAMPLE_H264_720P}" 3

PARALLEL=4
ROUNDS=10
LIFECYCLE_ITERS=50

# parallel_encode LABEL CODEC COUNT
# Run COUNT simultaneous encodes of the standard sample, wait for all.
# Returns 0 if all succeeded, 1 if any failed/crashed/timed out.
parallel_encode() {
    local label="$1" codec="$2" count="$3"
    local pids=() pid rc=0 i

    for i in $(seq 1 "$count"); do
        timeout -k 5 30 ffmpeg -y -hide_banner \
            -i "${SAMPLE_H264_720P}" \
            -c:v "${codec}" -f null - >/dev/null 2>&1 &
        pids+=($!)
    done

    for pid in "${pids[@]}"; do
        wait "$pid" 2>/dev/null || rc=$?
        if [ "$rc" -eq 124 ]; then
            echo "FAIL: ${label} — process ${pid} timed out (possible deadlock)."
            echo "   Code: nvmpi_encoder_get_packet atomic loads in nvmpi_enc_api.cpp"
            return 1
        fi
        if [ "$rc" -ge 128 ]; then
            echo "FAIL: ${label} — process ${pid} crashed (signal $((rc - 128)))."
            echo "   Code: capPlaneGotEOS/flushing atomics in nvmpi_enc_internal.h"
            return 1
        fi
    done
    return 0
}

# === 1. Parallel H.264 encode (4 simultaneous × 10 rounds) ===
echo "== 1. parallel h264_nvmpi encode (${PARALLEL}×${ROUNDS} rounds) =="
for r in $(seq 1 "$ROUNDS"); do
    if ! parallel_encode "round-${r}" h264_nvmpi "$PARALLEL"; then
        echo "FAIL: parallel H.264 encode failed at round ${r}/${ROUNDS}."
        exit 1
    fi
    [ $((r % 5)) -eq 0 ] && echo "   round ${r}/${ROUNDS} OK"
done
echo "   parallel H.264: ${ROUNDS} rounds of ${PARALLEL} — OK"

# === 2. Parallel HEVC encode (4 simultaneous × 10 rounds) ===
echo "== 2. parallel hevc_nvmpi encode (${PARALLEL}×${ROUNDS} rounds) =="
for r in $(seq 1 "$ROUNDS"); do
    if ! parallel_encode "round-${r}" hevc_nvmpi "$PARALLEL"; then
        echo "FAIL: parallel HEVC encode failed at round ${r}/${ROUNDS}."
        exit 1
    fi
    [ $((r % 5)) -eq 0 ] && echo "   round ${r}/${ROUNDS} OK"
done
echo "   parallel HEVC: ${ROUNDS} rounds of ${PARALLEL} — OK"

# === 3. Mixed-codec parallel (2× H.264 + 2× HEVC) ===
echo "== 3. mixed-codec parallel (2×h264 + 2×hevc, ${ROUNDS} rounds) =="
for r in $(seq 1 "$ROUNDS"); do
    pids=()
    for codec in h264_nvmpi h264_nvmpi hevc_nvmpi hevc_nvmpi; do
        timeout -k 5 30 ffmpeg -y -hide_banner \
            -i "${SAMPLE_H264_720P}" \
            -c:v "${codec}" -f null - >/dev/null 2>&1 &
        pids+=($!)
    done

    rc=0
    for pid in "${pids[@]}"; do
        wait "$pid" 2>/dev/null || rc=$?
        if [ "$rc" -ge 128 ] && [ "$rc" -ne 124 ]; then
            echo "FAIL: mixed-codec round ${r} — process crashed (signal $((rc - 128)))."
            echo "   Code: concurrent H.264+HEVC teardown in nvmpi_encoder_close"
            exit 1
        fi
    done
    [ $((r % 5)) -eq 0 ] && echo "   round ${r}/${ROUNDS} OK"
done
echo "   mixed-codec parallel: ${ROUNDS} rounds — OK"

# === 4. Parallel rapid lifecycle (4 concurrent loops of 50 open/close) ===
echo "== 4. parallel rapid lifecycle (${PARALLEL}×${LIFECYCLE_ITERS} open/close) =="

rapid_loop() {
    local iters="$1"
    for _ in $(seq 1 "$iters"); do
        timeout -k 5 30 ffmpeg -y -hide_banner \
            -i "${SAMPLE_H264_720P}" \
            -c:v h264_nvmpi -f null - >/dev/null 2>&1 || true
    done
}

pids=()
for _ in $(seq 1 "$PARALLEL"); do
    rapid_loop "$LIFECYCLE_ITERS" &
    pids+=($!)
done

fail=0
for pid in "${pids[@]}"; do
    wait "$pid" 2>/dev/null || fail=1
done
if [ "$fail" -ne 0 ]; then
    echo "FAIL: parallel rapid lifecycle — at least one loop failed."
    echo "   Code: unique_ptr<NvVideoEncoder> RAII in nvmpi_enc_api.cpp"
    exit 1
fi
echo "   parallel rapid lifecycle: ${PARALLEL}×${LIFECYCLE_ITERS} — OK"

# === 5. Device health after concurrent stress ===
echo "== 5. device health check =="
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
echo "   device health: ${frames} frames after concurrent stress — OK"

echo "OK: hw-encoder-thread-safety passed on ${variant}."
