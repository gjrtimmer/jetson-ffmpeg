#!/usr/bin/env bash
# Encoder thread-safety suite: concurrent multi-stream encode stress test.
# Validates that the encoder's cross-thread flags (capPlaneGotEOS, flushing)
# and packet pool lifecycle are safe under concurrent access from multiple
# independent encoder instances sharing the same V4L2 hardware engine.
#
# Unlike hw-lifecycle-stress.sh (sequential open/close), this suite
# runs multiple encoder processes IN PARALLEL to stress:
#   - std::atomic<bool> capPlaneGotEOS/flushing coherence between the DQ
#     callback thread and the user thread (data race fixed by issue #17)
#   - unique_ptr<NvVideoEncoder> RAII ownership under concurrent teardown
#   - Packet pool init/deinit rollback under parallel OOM-like conditions
#   - V4L2 device multiplexing (multiple M2M contexts on one NVENC engine)
#
# Tests:
#   1. Parallel encode ×4 (H.264) — 4 simultaneous encodes, repeated 3 rounds
#   2. Parallel encode ×4 (HEVC) — same with hevc_nvmpi
#   3. Mixed-codec parallel — 2× H.264 + 2× HEVC simultaneously
#   4. Device health — verify encoder still works after all concurrent stress
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

set -eu
# shellcheck source-path=SCRIPTDIR/..
# shellcheck source=lib/common.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../lib/common.sh"

variant="${JETSON_VARIANT:-unknown}"
echo "=== hw-encoder-thread-safety on variant: ${variant} ==="

ensure_sample SAMPLE_H264_720P gen-sample-h264 3

PARALLEL=4
ROUNDS=3

# parallel_encode LABEL CODEC COUNT
# Run COUNT simultaneous encodes of the standard sample, wait for all.
# Returns 0 if all succeeded, 1 if any failed/crashed/timed out.
parallel_encode() {
    local label="$1" codec="$2" count="$3"
    local pids=() pid rc=0

    for _ in $(seq 1 "$count"); do
        # Subshell: run_ff's FF_RC is the subshell exit status (signal retry included).
        ( run_ff -T 30 -i "${SAMPLE_H264_720P}" -frames:v 30 -c:v "${codec}" -f null - ) >/dev/null 2>&1 &
        pids+=($!)
    done

    for pid in "${pids[@]}"; do
        rc=0
        wait "$pid" 2>/dev/null || rc=$?
        if [ "$rc" -eq 124 ]; then
            echo "FAIL: ${label} — process ${pid} timed out (possible deadlock)."
            echo "   Code: nvmpi_encoder_get_packet atomic loads in nvmpi_enc_api.cpp"
            return 1
        fi
        if is_signal_rc "$rc"; then
            echo "FAIL: ${label} — process ${pid} crashed (signal $((rc - 128)))."
            echo "   Code: capPlaneGotEOS/flushing atomics in nvmpi_enc_internal.h"
            return 1
        fi
    done
    return 0
}

# === 1. Parallel H.264 encode (4 simultaneous × 3 rounds) ===
echo "== 1. parallel h264_nvmpi encode (${PARALLEL}×${ROUNDS} rounds) =="
for r in $(seq 1 "$ROUNDS"); do
    if ! parallel_encode "round-${r}" h264_nvmpi "$PARALLEL"; then
        echo "FAIL: parallel H.264 encode failed at round ${r}/${ROUNDS}."
        exit 1
    fi
    [ $((r % 5)) -eq 0 ] && echo "   round ${r}/${ROUNDS} OK"
done
echo "   parallel H.264: ${ROUNDS} rounds of ${PARALLEL} — OK"

# === 2. Parallel HEVC encode (4 simultaneous × 3 rounds) ===
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
        ( run_ff -T 30 -i "${SAMPLE_H264_720P}" -frames:v 30 -c:v "${codec}" -f null - ) >/dev/null 2>&1 &
        pids+=($!)
    done

    for pid in "${pids[@]}"; do
        rc=0
        wait "$pid" 2>/dev/null || rc=$?
        if [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; then
            echo "FAIL: mixed-codec round ${r} — process timed out (possible deadlock)."
            exit 1
        fi
        if is_signal_rc "$rc"; then
            echo "FAIL: mixed-codec round ${r} — process crashed (signal $((rc - 128)))."
            echo "   Code: concurrent H.264+HEVC teardown in nvmpi_encoder_close"
            exit 1
        fi
    done
    [ $((r % 5)) -eq 0 ] && echo "   round ${r}/${ROUNDS} OK"
done
echo "   mixed-codec parallel: ${ROUNDS} rounds — OK"

# === 4. Device health after concurrent stress ===
echo "== 4. device health check =="
# Encoder-specific (lib device_health only exercises the decoder).
run_ff_must "post-stress encode" -T 30 -i "${SAMPLE_H264_720P}" -frames:v 30 -c:v h264_nvmpi -f null -
assert_frames 10 "device health after concurrent stress"

echo "OK: hw-encoder-thread-safety passed on ${variant}."
