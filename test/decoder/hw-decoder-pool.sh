#!/usr/bin/env bash
# Decoder frame_pool_size suite: boundary-value tests for the frame_pool_size
# AVOption (min=1, max=32, default=5). Controls the depth of libnvmpi's
# decoded-frame buffer pool (NVMPI_bufPool). A pool too small risks
# starvation; a pool too large wastes DMA memory. Both extremes must decode
# without deadlock, crash, or frame loss.
set -eu
# shellcheck source-path=SCRIPTDIR/..
# shellcheck source=lib/common.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../lib/common.sh"

variant="${JETSON_VARIANT:-unknown}"
echo "=== hw-decoder-pool on variant: ${variant} ==="

ensure_sample SAMPLE_H264_720P gen-sample-h264 3

# pool_decode LABEL POOL_SIZE
pool_decode() {
  local label="$1" pool="$2"
  # frame_pool_size is validated in nvmpi_init_decoder (ffmpeg/dev/common/
  # libavcodec/nvmpi_dec.c); the pool is NVMPI_bufPool (src/NVMPI_bufPool.hpp).
  # A timeout here points at a possible deadlock at pool depth ${pool}.
  run_ff_must "(${label}) decode with frame_pool_size=${pool}" -T 30 -loglevel error \
    -frame_pool_size:v "$pool" -c:v h264_nvmpi -i "${SAMPLE_H264_720P}" -f null -
  echo "   ${label}: decode OK with frame_pool_size=${pool}"
}

echo "== 1. minimum pool size (1) =="
pool_decode min 1

echo "== 2. default pool size (5) =="
pool_decode default 5

echo "== 3. maximum pool size (32) =="
pool_decode max 32

echo "OK: hw-decoder-pool passed on ${variant}."
