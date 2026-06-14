#!/usr/bin/env bash
# Functional test for performance-mode options: max_perf, disable_dpb, poc_type.
# Verifies each option is accepted by the hardware encoder/decoder without
# error and produces valid output. Does NOT measure performance — see
# hw-perf-bench.sh for that.
set -eu
# shellcheck source=test/gen-samples.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/gen-samples.sh"

variant="${JETSON_VARIANT:-unknown}"
echo "=== hw-perf-mode on variant: ${variant} ==="

input_frames() {
  ffprobe -v error -count_packets -select_streams v:0 \
    -show_entries stream=nb_read_packets -of default=nokey=1:nw=1 "$1"
}

# decode_ok LABEL DECODER INPUT [EXTRA_OPTS...]
#   Decodes INPUT with DECODER, verifies output frame count >= 75% of input.
decode_ok() {
  local label="$1" dec="$2" sample="$3"
  shift 3
  local tmpout="/tmp/nvmpi-perf-dec-${label}.mkv"
  local rc=0 out expected actual

  expected=$(input_frames "$sample")
  out=$(ffmpeg -y -hide_banner -loglevel error \
    -c:v "$dec" "$@" -i "$sample" \
    -c:v ffv1 "$tmpout" 2>&1) || rc=$?
  if [ "$rc" -ne 0 ]; then
    echo "FAIL(${label}): decode failed (rc=${rc})."
    echo "$out" | tail -10
    exit 1
  fi
  actual=$(input_frames "$tmpout")
  local min_expected=$((expected * 75 / 100))
  if [ "${actual:-0}" -lt "$min_expected" ]; then
    echo "FAIL(${label}): decoded ${actual:-0}/${expected} frames (min ${min_expected})."
    exit 1
  fi
  echo "   ${label}: ${actual}/${expected} frames — OK"
}

# encode_ok LABEL ENCODER [EXTRA_OPTS...]
#   Encodes testsrc2 with ENCODER, verifies output has frames.
encode_ok() {
  local label="$1" enc="$2"
  shift 2
  local tmpout="/tmp/nvmpi-perf-enc-${label}.mp4"
  local rc=0 out actual

  out=$(ffmpeg -y -hide_banner -loglevel error \
    -f lavfi -i testsrc2=s=1280x720:r=30 -t 3 \
    -c:v "$enc" -b:v 3M "$@" "$tmpout" 2>&1) || rc=$?
  if [ "$rc" -ne 0 ]; then
    echo "FAIL(${label}): encode failed (rc=${rc})."
    echo "$out" | tail -10
    exit 1
  fi
  actual=$(input_frames "$tmpout")
  if [ "${actual:-0}" -lt 60 ]; then
    echo "FAIL(${label}): encoded only ${actual:-0} frames (expected ~90)."
    exit 1
  fi
  echo "   ${label}: ${actual} frames — OK"
}

# has_sw_encoder NAME — true when the software encoder is compiled into ffmpeg
has_sw_encoder() {
  ffmpeg -hide_banner -encoders 2>/dev/null | awk '{print $2}' | grep -qx "$1"
}

# --- generate samples ---
gen-sample-h264 "${SAMPLE_H264_720P}" 3

# --- decoder: max_perf ---
echo "== 1. Decoder: max_perf=1 (default) =="
decode_ok dec-maxperf-on h264_nvmpi "${SAMPLE_H264_720P}" -max_perf 1

echo "== 2. Decoder: max_perf=0 (opt-out) =="
decode_ok dec-maxperf-off h264_nvmpi "${SAMPLE_H264_720P}" -max_perf 0

# --- decoder: disable_dpb ---
echo "== 3. Decoder: disable_dpb=1 (low-latency, B-frame-free H.264) =="
decode_ok dec-dpb-off h264_nvmpi "${SAMPLE_H264_720P}" -disable_dpb 1

if has_sw_encoder libx265; then
  echo "== 4. Decoder: disable_dpb=1 + HEVC =="
  gen-sample-hevc "${SAMPLE_HEVC_720P}" 3
  decode_ok dec-dpb-hevc hevc_nvmpi "${SAMPLE_HEVC_720P}" -disable_dpb 1
else
  echo "== 4. SKIP: libx265 not available for HEVC sample generation =="
fi

# --- encoder: max_perf ---
echo "== 5. Encoder: max_perf=1 (default) =="
encode_ok enc-maxperf-on h264_nvmpi -max_perf 1

echo "== 6. Encoder: max_perf=0 (opt-out) =="
encode_ok enc-maxperf-off h264_nvmpi -max_perf 0

# --- encoder: poc_type ---
echo "== 7. Encoder: poc_type=2 (decode-order, low-latency) =="
encode_ok enc-poc2 h264_nvmpi -poc_type 2

echo "== 8. Encoder: poc_type=0 (default) =="
encode_ok enc-poc0 h264_nvmpi -poc_type 0

# --- combined ---
echo "== 9. Encoder: max_perf=1 + poc_type=2 (combined) =="
encode_ok enc-combo h264_nvmpi -max_perf 1 -poc_type 2

echo "OK: hw-perf-mode passed on ${variant}."
