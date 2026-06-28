#!/usr/bin/env bash
# VIC hardware scale filter suite: the scale_vic AVFilter uses the Tegra VIC
# engine for zero-copy DRM_PRIME scaling. Tests the full pipeline:
#   decoder (DRM_PRIME) → scale_vic → encoder (DRM_PRIME)
# Verifies output dimensions and that the filter is recognized by FFmpeg.
set -eu
# shellcheck source=test/gen-samples.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/gen-samples.sh"

variant="${JETSON_VARIANT:-unknown}"
echo "=== hw-filter-scale-vic on variant: ${variant} ==="

[ -s "${SAMPLE_H264_720P}" ] || gen-sample-h264 "${SAMPLE_H264_720P}" 3

# vic_scale_case LABEL WIDTH HEIGHT
vic_scale_case() {
  local label="$1" tgt_w="$2" tgt_h="$3"
  local tmpout="/tmp/nvmpi-scale-vic-${label}.mkv"
  local rc=0 out w h

  out=$(ffmpeg -y -hide_banner -loglevel error \
    -hwaccel nvmpi -c:v h264_nvmpi -i "${SAMPLE_H264_720P}" \
    -vf "scale_vic=${tgt_w}:${tgt_h}" \
    -c:v h264_nvmpi "$tmpout" 2>&1) || rc=$?
  if [ "$rc" -ne 0 ]; then
    echo "FAIL(${label}): scale_vic ${tgt_w}:${tgt_h} pipeline failed (rc=${rc})."
    echo "      Pipeline: h264_nvmpi -hwaccel nvmpi → scale_vic → h264_nvmpi"
    echo "      Filter source: ffmpeg/dev/common/libavfilter/vf_scale_vic.c"
    echo "--- ffmpeg output (last 15 lines) ---"
    echo "$out" | tail -15
    exit 1
  fi

  w=$(ffprobe -v error -select_streams v:0 \
    -show_entries stream=width -of default=nokey=1:nw=1 "$tmpout")
  h=$(ffprobe -v error -select_streams v:0 \
    -show_entries stream=height -of default=nokey=1:nw=1 "$tmpout")
  if [ "${w:-0}" -ne "$tgt_w" ] || [ "${h:-0}" -ne "$tgt_h" ]; then
    echo "FAIL(${label}): expected ${tgt_w}x${tgt_h}, got ${w:-?}x${h:-?}."
    echo "      Check that VIC transform produces correct output dimensions"
    echo "      (src/nvmpi_vic.cpp → NvBufSurfTransform)."
    exit 1
  fi
  echo "   ${label}: ${w}x${h}"
}

echo "== 1. filter availability check =="
if ! ffmpeg -hide_banner -filters 2>&1 | grep -q 'scale_vic'; then
  echo "FAIL: scale_vic filter not found in FFmpeg filter list."
  echo "      Check: configure --enable-nvmpi, allfilters.c registration,"
  echo "      and libavfilter/Makefile OBJS-\$(CONFIG_SCALE_VIC_FILTER)."
  exit 1
fi
echo "   scale_vic filter registered"

echo "== 2. downscale 1280x720 → 640x360 =="
vic_scale_case down-640x360 640 360

echo "== 3. downscale 1280x720 → 320x240 =="
vic_scale_case down-320x240 320 240

echo "== 4. non-proportional scale 1280x720 → 640x480 =="
vic_scale_case nonprop-640x480 640 480

echo "== 5. passthrough (same resolution) =="
vic_scale_case passthrough-1280x720 1280 720

echo "OK: hw-filter-scale-vic passed on ${variant}."
