#!/usr/bin/env bash
# Decoder hardware downscale suite: the -resize WxH AVOption (nvmpi_dec.c
# resize_expr -> param.resized) triggers libnvmpi's hardware scaler (VIC on
# Jetson). The decoded output must have exactly the requested dimensions.
set -eu
# shellcheck source=test/gen-samples.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/gen-samples.sh"

variant="${JETSON_VARIANT:-unknown}"
echo "=== hw-decoder-downscale on variant: ${variant} ==="

[ -s "${SAMPLE_H264_720P}" ] || gen-sample-h264 "${SAMPLE_H264_720P}" 3

# downscale_case LABEL RESIZE_EXPR EXPECTED_W EXPECTED_H
downscale_case() {
  local label="$1" resize="$2" exp_w="$3" exp_h="$4"
  local tmpout="/tmp/nvmpi-dec-downscale-${label}.mkv"
  local rc=0 out w h

  out=$(ffmpeg -y -hide_banner -loglevel error \
    -resize:v "$resize" -c:v h264_nvmpi -i "${SAMPLE_H264_720P}" \
    -c:v ffv1 "$tmpout" 2>&1) || rc=$?
  if [ "$rc" -ne 0 ]; then
    echo "FAIL(${label}): h264_nvmpi decode with -resize ${resize} failed (rc=${rc})."
    echo "      resize_expr parsed in nvmpi_init_decoder"
    echo "      (ffmpeg/dev/common/libavcodec/nvmpi_dec.c),"
    echo "      forwarded to libnvmpi as param.resized (src/nvmpi_dec.cpp)."
    echo "--- ffmpeg output (last 15 lines) ---"
    echo "$out" | tail -15
    exit 1
  fi

  w=$(ffprobe -v error -select_streams v:0 \
    -show_entries stream=width -of default=nokey=1:nw=1 "$tmpout")
  h=$(ffprobe -v error -select_streams v:0 \
    -show_entries stream=height -of default=nokey=1:nw=1 "$tmpout")
  if [ "${w:-0}" -ne "$exp_w" ] || [ "${h:-0}" -ne "$exp_h" ]; then
    echo "FAIL(${label}): expected ${exp_w}x${exp_h}, got ${w:-?}x${h:-?}."
    echo "      Check that libnvmpi applies the resize (V4L2 SELECTION on"
    echo "      the capture plane in src/nvmpi_dec.cpp)."
    exit 1
  fi
  echo "   ${label}: output ${w}x${h}"
}

echo "== 1. proportional downscale: 1280x720 -> 640x360 =="
downscale_case 640x360 640x360 640 360

echo "== 2. non-proportional downscale: 1280x720 -> 640x480 =="
downscale_case 640x480 640x480 640 480

echo "OK: hw-decoder-downscale passed on ${variant}."
