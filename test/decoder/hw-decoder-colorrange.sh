#!/usr/bin/env bash
# Decoder colour-range suite (jetson-ffmpeg#152): the video decoders tag
# DRM_PRIME frames with the stream's range. libnvmpi >= 4.10.1 sets
# NVMPI_FD_FRAME_FULL_RANGE in nvmpi_decoder_get_frame_fd2 for full-range
# streams; the wrapper maps it to frame->color_range.
#
# Tests (H.264 clips with the range signalled in the VUI):
#   1. Full-range clip, DRM_PRIME output: every frame is color_range pc.
#   2. Limited-range clip, DRM_PRIME output: no frame is pc (tv, or unknown:
#      libnvmpi gives no capability bit, so a clear flag is not asserted as tv).
#   3. Full-range clip, software-frame output: pc (the wrapper sets nothing
#      here; libavcodec takes the range from avctx, i.e. the stream VUI).
# Tests 1 and 2 clear avctx->color_range (-color_range unknown) so that
# libavcodec cannot fill the range in itself; only the wrapper can make 1 pass.
# On the mock the range comes from NVMPI_MOCK_DEC_FULL_RANGE (the mock does not
# parse the VUI), set here to match each clip. On hardware the flag follows the
# stream. showinfo reports the range per frame; no rapid open/close loops.
set -eu
# shellcheck source-path=SCRIPTDIR/..
# shellcheck source=lib/common.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../lib/common.sh"

variant="${JETSON_VARIANT:-unknown}"
echo "=== hw-decoder-colorrange on variant: ${variant} ==="

has_codec dec h264_nvmpi || { echo "SKIP: h264_nvmpi not compiled in."; exit 0; }

T=/tmp/nvmpi-colorrange
trap 'rm -f "${T}"-* 2>/dev/null || true' EXIT

# DRM_PRIME output (fd2 path). libavcodec copies avctx->color_range (the stream
# VUI, parsed by FFmpeg) into frames whose range is unspecified, so the decoder
# option clears avctx: the range can then only come from the wrapper.
DEC_DRM=(-hwaccel nvmpi -c:v h264_nvmpi -color_range unknown)
DEC_SW=(-c:v h264_nvmpi)                   # software-frame (copy) output

# ranges_of LABEL RANGE DEC_ARGS... : decode the RANGE clip with showinfo and
# set PC_N / OTHER_N to the frames with / without color_range:pc.
ranges_of() {
  local label="$1" range="$2" full=0
  shift 2
  [ "$range" = pc ] && full=1
  # Mock only: the mock reports the range from this variable, not the VUI.
  NVMPI_MOCK_DEC_FULL_RANGE="$full" run_ff_must "${label}" -T 60 -loglevel info \
    "$@" -f h264 -i "${T}-${range}.h264" -vf showinfo -noautoscale -f null -
  PC_N=$(printf '%s\n' "$FF_OUT" | grep -c 'color_range:pc' || true)
  OTHER_N=$(printf '%s\n' "$FF_OUT" | grep -c 'Parsed_showinfo.*color_range:' || true)
  OTHER_N=$((OTHER_N - PC_N))
  echo "   ${label}: pc=${PC_N} other=${OTHER_N}"
}

gen-sample-h264-range "${T}-pc.h264" pc 2
gen-sample-h264-range "${T}-tv.h264" tv 2

echo "== 1. full-range stream, DRM_PRIME =="
ranges_of "full DRM_PRIME" pc "${DEC_DRM[@]}"
[ "$PC_N" -ge 1 ] && [ "$OTHER_N" -eq 0 ] \
  || { echo "FAIL(full DRM_PRIME): expected only color_range:pc frames (pc=${PC_N} other=${OTHER_N})."; exit 1; }

echo "== 2. limited-range stream, DRM_PRIME =="
ranges_of "limited DRM_PRIME" tv "${DEC_DRM[@]}"
[ "$PC_N" -eq 0 ] && [ "$OTHER_N" -ge 1 ] \
  || { echo "FAIL(limited DRM_PRIME): a limited stream must not report pc (pc=${PC_N} other=${OTHER_N})."; exit 1; }

echo "== 3. full-range stream, software frames =="
ranges_of "full software" pc "${DEC_SW[@]}"
[ "$PC_N" -ge 1 ] && [ "$OTHER_N" -eq 0 ] \
  || { echo "FAIL(full software): expected only color_range:pc frames (pc=${PC_N} other=${OTHER_N})."; exit 1; }

echo "OK: hw-decoder-colorrange passed on ${variant}."
