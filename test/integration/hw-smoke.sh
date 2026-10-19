#!/usr/bin/env bash
# Basic hardware smoke: are the nvmpi codecs registered and do plain
# decode->encode transcodes work at all? First gate — when this fails,
# nothing else is worth reading.
# h264_nvmpi/hevc_nvmpi have no software fallback, so success proves
# NVDEC+NVENC actually engaged on the target hardware.
set -eu
# shellcheck source-path=SCRIPTDIR/..
# shellcheck source=lib/common.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../lib/common.sh"

variant="${JETSON_VARIANT:-unknown}"
echo "=== hw-smoke on variant: ${variant} ==="

echo "== 1. nvmpi codecs present? =="
if has_codec enc h264_nvmpi || has_codec enc hevc_nvmpi; then
  ffmpeg -hide_banner -encoders 2>/dev/null | grep -E 'h264_nvmpi|hevc_nvmpi'
else
  echo "FAIL: nvmpi encoders missing"; exit 1
fi
if has_codec dec h264_nvmpi || has_codec dec hevc_nvmpi; then
  ffmpeg -hide_banner -decoders 2>/dev/null | grep -E 'h264_nvmpi|hevc_nvmpi'
else
  echo "FAIL: nvmpi decoders missing"; exit 1
fi

echo "== 2. H.264 test sample (software) =="
ensure_sample SAMPLE_H264_720P gen-sample-h264 3

echo "== 3. HW transcode: h264_nvmpi decode -> hevc_nvmpi encode =="
run_ff_must "h264_nvmpi -> hevc_nvmpi transcode" -T 60 \
  -c:v h264_nvmpi -i "${SAMPLE_H264_720P}" \
  -c:v hevc_nvmpi -b:v 3M /tmp/out.mkv

codec=$(ffprobe -v error -select_streams v:0 \
  -show_entries stream=codec_name -of default=nokey=1:nw=1 /tmp/out.mkv)
echo "   output codec = ${codec}"
[ "$codec" = "hevc" ] || { echo "FAIL: expected hevc, got ${codec}"; exit 1; }

echo "== 4. HW transcode: h264_nvmpi decode -> h264_nvmpi encode =="
run_ff_must "h264_nvmpi -> h264_nvmpi transcode" -T 60 \
  -c:v h264_nvmpi -i "${SAMPLE_H264_720P}" \
  -c:v h264_nvmpi -b:v 3M /tmp/out_h264.mkv

codec=$(ffprobe -v error -select_streams v:0 \
  -show_entries stream=codec_name -of default=nokey=1:nw=1 /tmp/out_h264.mkv)
echo "   output codec = ${codec}"
[ "$codec" = "h264" ] || { echo "FAIL: expected h264, got ${codec}"; exit 1; }

echo "OK: hw-smoke passed on ${variant}."
