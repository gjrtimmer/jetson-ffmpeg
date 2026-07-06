#!/usr/bin/env bash
# Decoder frame_pool_size suite: boundary-value tests for the frame_pool_size
# AVOption (min=1, max=32, default=5). Controls the depth of libnvmpi's
# decoded-frame buffer pool (NVMPI_bufPool). A pool too small risks
# starvation; a pool too large wastes DMA memory. Both extremes must decode
# without deadlock, crash, or frame loss.
set -eu
# shellcheck source=test/gen-samples.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../gen-samples.sh"

variant="${JETSON_VARIANT:-unknown}"
echo "=== hw-decoder-pool on variant: ${variant} ==="

[ -s "${SAMPLE_H264_720P}" ] || gen-sample-h264 "${SAMPLE_H264_720P}" 3

# pool_decode LABEL POOL_SIZE
pool_decode() {
  local label="$1" pool="$2"
  local rc=0 out
  out=$(timeout -k 5 30 ffmpeg -y -hide_banner -loglevel error \
    -frame_pool_size:v "$pool" -c:v h264_nvmpi -i "${SAMPLE_H264_720P}" \
    -f null - 2>&1) || rc=$?
  if [ "$rc" -eq 124 ]; then
    echo "FAIL(${label}): decode with frame_pool_size=${pool} timed out (30 s)."
    echo "      Possible deadlock in NVMPI_bufPool (src/NVMPI_bufPool.hpp)"
    echo "      when pool depth = ${pool}."
    exit 1
  fi
  if [ "$rc" -ne 0 ]; then
    echo "FAIL(${label}): decode with frame_pool_size=${pool} failed (rc=${rc})."
    echo "      frame_pool_size validated in nvmpi_init_decoder"
    echo "      (ffmpeg/dev/common/libavcodec/nvmpi_dec.c), pool managed by"
    echo "      NVMPI_bufPool (src/NVMPI_bufPool.hpp)."
    echo "--- ffmpeg output (last 15 lines) ---"
    echo "$out" | tail -15
    exit 1
  fi
  echo "   ${label}: decode OK with frame_pool_size=${pool}"
}

echo "== 1. minimum pool size (1) =="
pool_decode min 1

echo "== 2. default pool size (5) =="
pool_decode default 5

echo "== 3. maximum pool size (32) =="
pool_decode max 32

echo "OK: hw-decoder-pool passed on ${variant}."
