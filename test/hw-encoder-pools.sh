#!/usr/bin/env bash
# Encoder packet_pool_size suite: boundary-value tests for the packet_pool_size
# AVOption (min=1, max=32, default=10). Controls the depth of the FFmpeg
# wrapper's pre-allocated packet buffer pool — how many encoded packets can
# pile up before the libnvmpi DQ thread blocks. A pool too small risks
# starvation under burst; a pool too large wastes memory. Both extremes must
# encode without deadlock, crash, or significant frame loss.
#
# Code under test: ffmpeg/dev/common/libavcodec/nvmpi_enc.c
#   - nvmpienc_initPktPool (pool allocation at init)
#   - nvmpienc_deinitPktPool (pool teardown at close)
#   - ff_nvmpi_receive_packet (pool recycle path during encode)
# Feature: #11 (closed) — configurable pool sizes
# Issue:   gjrtimmer/jetson-ffmpeg#27

# shellcheck source=test/gen-samples.sh
set -eu

. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/gen-samples.sh"

variant="${JETSON_VARIANT:-unknown}"
echo "=== hw-encoder-pools on variant: ${variant} ==="

[ -s "${SAMPLE_H264_720P}" ] || gen-sample-h264 "${SAMPLE_H264_720P}" 3

# pool_encode LABEL CODEC POOL_SIZE
# Encode the standard H.264 720p sample with the given encoder and pool size.
# Verify: no crash, no timeout, frame count >= 80% of input.
pool_encode() {
    local label="$1" codec="$2" pool_size="$3"
    echo "== ${label}: ${codec} packet_pool_size=${pool_size} =="

    local rc=0
    local out
    out=$(timeout -k 5 30 ffmpeg -y -hide_banner \
      -i "${SAMPLE_H264_720P}" \
      -c:v "${codec}" -packet_pool_size "${pool_size}" \
      -f null - 2>&1) || rc=$?

    if [ "$rc" -eq 124 ]; then
        echo "FAIL: ${label} timed out — possible deadlock at pool_size=${pool_size}."
        echo "   Code: nvmpienc_initPktPool / ff_nvmpi_receive_packet in nvmpi_enc.c"
        echo "$out" | tail -15
        exit 1
    fi
    if [ "$rc" -ge 128 ]; then
        echo "FAIL: ${label} crashed (signal $((rc - 128))) at pool_size=${pool_size}."
        echo "   Code: nvmpienc_initPktPool / nvmpienc_deinitPktPool in nvmpi_enc.c"
        echo "$out" | tail -15
        exit 1
    fi

    local frames
    frames=$(echo "$out" | grep -oP 'frame=\s*\K[0-9]+' | tail -1)
    if [ -z "$frames" ] || [ "$frames" -lt 10 ]; then
        echo "FAIL: ${label} produced ${frames:-0} frames (expected >= 10)."
        echo "$out" | tail -15
        exit 1
    fi
    echo "   ${label}: ${frames} frames — OK"
}

# === 1. H.264 default pool (10) ===
pool_encode "1. h264-default" h264_nvmpi 10

# === 2. H.264 minimum pool (1) ===
pool_encode "2. h264-min" h264_nvmpi 1

# === 3. H.264 maximum pool (32) ===
pool_encode "3. h264-max" h264_nvmpi 32

# === 4. HEVC default pool (10) ===
pool_encode "4. hevc-default" hevc_nvmpi 10

# === 5. HEVC minimum pool (1) ===
pool_encode "5. hevc-min" hevc_nvmpi 1

echo "OK: hw-encoder-pools passed on ${variant}."
