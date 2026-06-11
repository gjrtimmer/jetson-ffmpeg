#!/usr/bin/env bash
# HW encode/decode smoke test — runs on any Jetson runner variant.
# Invoked per hardware variant via the CI matrix (JETSON_VARIANT).
# h264_nvmpi/hevc_nvmpi have no software fallback, so success proves
# NVDEC+NVENC actually engaged on the target hardware.
set -eu

variant="${JETSON_VARIANT:-unknown}"
echo "=== Running HW test on variant: ${variant} ==="

echo "== 1. nvmpi codecs present? =="
ffmpeg -hide_banner -encoders 2>/dev/null | grep -E 'h264_nvmpi|hevc_nvmpi' || { echo "FAIL: nvmpi encoders missing"; exit 1; }
ffmpeg -hide_banner -decoders 2>/dev/null | grep -E 'h264_nvmpi|hevc_nvmpi' || { echo "FAIL: nvmpi decoders missing"; exit 1; }

echo "== 2. generate short H.264 test sample (software) =="
ffmpeg -y -hide_banner -loglevel error \
  -f lavfi -i testsrc2=s=1280x720:r=30 -t 3 \
  -c:v libx264 /tmp/in.mp4

echo "== 3. HW transcode: h264_nvmpi decode -> hevc_nvmpi encode =="
ffmpeg -y -hide_banner -loglevel error \
  -c:v h264_nvmpi -i /tmp/in.mp4 \
  -c:v hevc_nvmpi -b:v 3M /tmp/out.mkv

codec=$(ffprobe -v error -select_streams v:0 \
  -show_entries stream=codec_name -of default=nokey=1:nw=1 /tmp/out.mkv)
echo "   output codec = ${codec}"
[ "$codec" = "hevc" ] || { echo "FAIL: expected hevc, got ${codec}"; exit 1; }

echo "== 4. HW transcode: h264_nvmpi decode -> h264_nvmpi encode =="
ffmpeg -y -hide_banner -loglevel error \
  -c:v h264_nvmpi -i /tmp/in.mp4 \
  -c:v h264_nvmpi -b:v 3M /tmp/out_h264.mkv

codec=$(ffprobe -v error -select_streams v:0 \
  -show_entries stream=codec_name -of default=nokey=1:nw=1 /tmp/out_h264.mkv)
echo "   output codec = ${codec}"
[ "$codec" = "h264" ] || { echo "FAIL: expected h264, got ${codec}"; exit 1; }

echo ""
echo "OK: nvmpi hardware decode+encode works on ${variant}."
