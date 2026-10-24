#!/usr/bin/env bash
# Decoder EOS frame-count suite (jetson-ffmpeg#149): an nvmpi decode of a
# stream with B-frames must output every frame software decode does. The
# hardware decoder keeps its reorder depth (the last few frames) until the
# wrapper sends the zero-size EOS packet and drains to the driver's LAST
# buffer; without that the tail is lost (296 of 300 H.264, 298 of 300 HEVC).
#
# Tests:
#   1. H.264 with B-frames: h264_nvmpi frame count == software count.
#   2. HEVC with B-frames: same for hevc_nvmpi (skipped without libx265).
#   3. -flags low_delay (decoder returns per packet, then drains): same count.
#
# Needs a libnvmpi that advertises NVMPI_CAP_DEC_EOS_DRAIN (nvmpi_get_caps, >= 4.7.0; jetson-nvmpi#161).
# On the mock, NVMPI_MOCK_DEC_REORDER holds back that many frames until EOS (a
# DPB depth stand-in; default 4 here, the caller's value wins).
set -eu
# shellcheck source-path=SCRIPTDIR/..
# shellcheck source=lib/common.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../lib/common.sh"

variant="${JETSON_VARIANT:-unknown}"
echo "=== hw-decoder-eos-count on variant: ${variant} ==="

FRAMES=60
if is_mock; then
  export NVMPI_MOCK_DEC_REORDER="${NVMPI_MOCK_DEC_REORDER:-4}"
  echo "   mock: holding back ${NVMPI_MOCK_DEC_REORDER} frames until EOS"
fi

ensure_sample SAMPLE_H264_BFRAMES gen-sample-h264-bframes "${FRAMES}"

# decoded_frames LABEL INPUT_ARGS... : null decode; sets DECODED to the last
# `frame=` count of the stats. -fps_mode passthrough (output option, after the
# input): no duplicate/drop adjustment, so the count is what the decoder
# delivered.
decoded_frames() {
  local label="$1"; shift
  run_ff_must "${label}" -T 60 "$@" -fps_mode passthrough -f null -
  DECODED=$(printf '%s\n' "$FF_OUT" | grep -oP 'frame=\s*\K[0-9]+' | tail -1 || true)
  DECODED="${DECODED:-0}"
}

# check_count LABEL NVMPI_CODEC SW_CODEC INPUT [EXTRA_INPUT_ARGS...]
check_count() {
  local label="$1" hw="$2" sw="$3" input="$4" sw_n hw_n
  shift 4
  decoded_frames "${label} software" -c:v "${sw}" -i "${input}"
  sw_n="${DECODED}"
  # The sample itself must hold FRAMES frames, else the reference is wrong.
  if [ "${sw_n}" -ne "${FRAMES}" ]; then
    ff_fail "${label}: software decode gave ${sw_n} frames, sample should hold ${FRAMES}."
  fi
  decoded_frames "${label} nvmpi" "$@" -c:v "${hw}" -i "${input}"
  hw_n="${DECODED}"
  if [ "${hw_n}" -ne "${sw_n}" ]; then
    echo "      ${hw} delivered ${hw_n} of ${sw_n} frames (missing $((sw_n - hw_n)))."
    echo "      The EOS packet is not sent or libnvmpi does not drain after it: see"
    echo "      nvmpi_dec_send_eos() (nvmpi_dec_drain.h) and jetson-nvmpi#161."
    ff_fail "${label}: nvmpi frame count differs from software decode."
  fi
  echo "   ${label}: ${hw_n} frames == software ${sw_n} - OK"
}

echo "== 1. H.264 with B-frames =="
check_count "h264-bframes" h264_nvmpi h264 "${SAMPLE_H264_BFRAMES}"

echo "== 2. HEVC with B-frames =="
if ! has_codec enc libx265; then
  echo "   skipped (libx265 not available in this FFmpeg build)"
else
  ensure_sample SAMPLE_HEVC_BFRAMES gen-sample-hevc-bframes "${FRAMES}"
  check_count "hevc-bframes" hevc_nvmpi hevc "${SAMPLE_HEVC_BFRAMES}"
fi

echo "== 3. H.264 with -flags low_delay =="
check_count "h264-low-delay" h264_nvmpi h264 "${SAMPLE_H264_BFRAMES}" -flags low_delay

echo "OK: hw-decoder-eos-count passed on ${variant}."
