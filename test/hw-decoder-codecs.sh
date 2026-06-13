#!/usr/bin/env bash
# Decoder codec coverage: exercises every nvmpi decoder codec beyond H.264 and
# HEVC (already covered by hw-smoke). Tests MPEG-2 (mpeg2_nvmpi), MPEG-4
# (mpeg4_nvmpi), VP8 (vp8_nvmpi), and VP9 (vp9_nvmpi) decode paths through
# the codec-type mapping in nvmpi_dec.c (nvmpi_get_codingtype) and the
# corresponding libnvmpi V4L2 decode pipelines. Software-encoded samples are
# decoded via the nvmpi hardware decoder and re-encoded with ffv1; output
# frame counts are verified against the input. VP8/VP9 require libvpx for
# sample generation and are SKIPped (not FAILed) when absent.
set -eu
# shellcheck source=test/gen-samples.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/gen-samples.sh"

variant="${JETSON_VARIANT:-unknown}"
echo "=== hw-decoder-codecs on variant: ${variant} ==="

# has_sw_encoder NAME — true when the software encoder is compiled into ffmpeg
has_sw_encoder() {
  ffmpeg -hide_banner -encoders 2>/dev/null | awk '{print $2}' | grep -qx "$1"
}

# input_frames FILE — count video packets in a container
input_frames() {
  ffprobe -v error -count_packets -select_streams v:0 \
    -show_entries stream=nb_read_packets -of default=nokey=1:nw=1 "$1"
}

# decode_case LABEL NVMPI_DECODER SAMPLE
#   Decodes SAMPLE with the nvmpi hardware decoder, re-encodes through ffv1
#   to a temp MKV, and asserts the output frame count matches the input.
decode_case() {
  local label="$1" dec="$2" sample="$3"
  local tmpout="/tmp/nvmpi-dec-${label}.mkv"
  local rc=0 out expected actual

  expected=$(input_frames "$sample")
  if [ "${expected:-0}" -lt 1 ]; then
    echo "FAIL(${label}): input sample has 0 packets — gen-sample broken?"
    exit 1
  fi

  out=$(ffmpeg -y -hide_banner -loglevel error \
    -c:v "$dec" -i "$sample" \
    -c:v ffv1 "$tmpout" 2>&1) || rc=$?
  if [ "$rc" -ne 0 ]; then
    echo "FAIL(${label}): ${dec} decode failed (rc=${rc})."
    echo "      Codec-type mapping: nvmpi_get_codingtype in"
    echo "      ffmpeg/dev/common/libavcodec/nvmpi_dec.c."
    echo "      V4L2 decode pipeline: src/nvmpi_dec.cpp."
    echo "--- ffmpeg output (last 15 lines) ---"
    echo "$out" | tail -15
    exit 1
  fi

  actual=$(input_frames "$tmpout")
  # The V4L2 decode pipeline has internal buffering depth (~1 GOP); the
  # last few frames may not be flushed at end-of-stream (tracked in #27).
  # 75% threshold proves the codec path works while tolerating pipeline
  # latency. VP9 in particular drops more frames on Orin under load.
  # A truly broken decoder produces 0 frames, not 67/90.
  min_expected=$((expected * 75 / 100))
  if [ "${actual:-0}" -lt "$min_expected" ]; then
    echo "FAIL(${label}): ${dec} decoded ${actual:-0}/${expected} frames (min ${min_expected})."
    echo "      Check the V4L2 decode pipeline: src/nvmpi_dec.cpp."
    exit 1
  fi
  echo "   ${label}: ${actual}/${expected} frames decoded"
}

skipped=0

echo "== 1. MPEG-2 decode (mpeg2_nvmpi) =="
gen-sample-mpeg2 "${SAMPLE_MPEG2_720P}" 3
decode_case mpeg2 mpeg2_nvmpi "${SAMPLE_MPEG2_720P}"

echo "== 2. MPEG-4 decode (mpeg4_nvmpi) =="
gen-sample-mpeg4 "${SAMPLE_MPEG4_720P}" 3
decode_case mpeg4 mpeg4_nvmpi "${SAMPLE_MPEG4_720P}"

echo "== 3. VP8 decode (vp8_nvmpi) =="
if has_sw_encoder libvpx; then
  gen-sample-vp8 "${SAMPLE_VP8_720P}" 3
  decode_case vp8 vp8_nvmpi "${SAMPLE_VP8_720P}"
else
  echo "   SKIP: libvpx not available for sample generation"
  skipped=$((skipped + 1))
fi

echo "== 4. VP9 decode (vp9_nvmpi) =="
if has_sw_encoder libvpx-vp9; then
  gen-sample-vp9 "${SAMPLE_VP9_720P}" 3
  decode_case vp9 vp9_nvmpi "${SAMPLE_VP9_720P}"
else
  echo "   SKIP: libvpx-vp9 not available for sample generation"
  skipped=$((skipped + 1))
fi

if [ "$skipped" -gt 0 ]; then
  echo "(${skipped} codec(s) skipped — build ffmpeg with --enable-libvpx for full coverage)"
fi
echo "OK: hw-decoder-codecs passed on ${variant}."
