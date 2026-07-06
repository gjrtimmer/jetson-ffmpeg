#!/usr/bin/env bash
# Basic hardware smoke: are the nvmpi codecs registered and do plain
# decode->encode transcodes work at all? First gate — when this fails,
# nothing else is worth reading.
# h264_nvmpi/hevc_nvmpi have no software fallback, so success proves
# NVDEC+NVENC actually engaged on the target hardware.
set -eu
# shellcheck source=test/gen-samples.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../gen-samples.sh"

variant="${JETSON_VARIANT:-unknown}"
echo "=== hw-smoke on variant: ${variant} ==="

echo "== 1. nvmpi codecs present? =="
ffmpeg -hide_banner -encoders 2>/dev/null | grep -E 'h264_nvmpi|hevc_nvmpi' || { echo "FAIL: nvmpi encoders missing"; exit 1; }
ffmpeg -hide_banner -decoders 2>/dev/null | grep -E 'h264_nvmpi|hevc_nvmpi' || { echo "FAIL: nvmpi decoders missing"; exit 1; }

echo "== 2. generate short H.264 test sample (software) =="
gen-sample-h264 "${SAMPLE_H264_720P}" 3

echo "== 3. HW transcode: h264_nvmpi decode -> hevc_nvmpi encode =="
rc=0
out=$(ffmpeg -y -hide_banner \
  -c:v h264_nvmpi -i "${SAMPLE_H264_720P}" \
  -c:v hevc_nvmpi -b:v 3M /tmp/out.mkv 2>&1) || rc=$?
if [ "$rc" -ne 0 ]; then
  echo "FAIL: h264_nvmpi -> hevc_nvmpi transcode failed (rc=${rc})."
  echo "--- ffmpeg output (last 15 lines) ---"
  echo "$out" | tail -15
  exit 1
fi

codec=$(ffprobe -v error -select_streams v:0 \
  -show_entries stream=codec_name -of default=nokey=1:nw=1 /tmp/out.mkv)
echo "   output codec = ${codec}"
[ "$codec" = "hevc" ] || { echo "FAIL: expected hevc, got ${codec}"; exit 1; }

echo "== 4. HW transcode: h264_nvmpi decode -> h264_nvmpi encode =="
rc=0
out=$(ffmpeg -y -hide_banner \
  -c:v h264_nvmpi -i "${SAMPLE_H264_720P}" \
  -c:v h264_nvmpi -b:v 3M /tmp/out_h264.mkv 2>&1) || rc=$?
if [ "$rc" -ne 0 ]; then
  echo "FAIL: h264_nvmpi -> h264_nvmpi transcode failed (rc=${rc})."
  echo "--- ffmpeg output (last 15 lines) ---"
  echo "$out" | tail -15
  exit 1
fi

codec=$(ffprobe -v error -select_streams v:0 \
  -show_entries stream=codec_name -of default=nokey=1:nw=1 /tmp/out_h264.mkv)
echo "   output codec = ${codec}"
[ "$codec" = "h264" ] || { echo "FAIL: expected h264, got ${codec}"; exit 1; }

echo "OK: hw-smoke passed on ${variant}."
