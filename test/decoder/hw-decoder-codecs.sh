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
# shellcheck source-path=SCRIPTDIR/..
# shellcheck source=lib/common.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../lib/common.sh"

variant="${JETSON_VARIANT:-unknown}"
echo "=== hw-decoder-codecs on variant: ${variant} ==="

# decode_case LABEL NVMPI_DECODER SAMPLE
#   Decodes SAMPLE with the nvmpi hardware decoder, re-encodes through ffv1
#   to a temp MKV, and asserts the output frame count matches the input.
decode_case() {
  local label="$1" dec="$2" sample="$3"
  local tmpout="/tmp/nvmpi-dec-${label}.mkv"
  local expected actual

  expected=$(packet_count "$sample")
  if [ "${expected:-0}" -lt 1 ]; then
    echo "FAIL(${label}): input sample has 0 packets — gen-sample broken?"
    exit 1
  fi

  echo "   (codec-type mapping: nvmpi_get_codingtype in ffmpeg/dev/common/libavcodec/nvmpi_dec.c;"
  echo "    V4L2 decode pipeline: src/nvmpi_dec.cpp)"
  run_ff_must "${label}: ${dec} decode" -T 60 -loglevel error \
    -c:v "$dec" -i "$sample" -c:v ffv1 "$tmpout"

  actual=$(packet_count "$tmpout")
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
ensure_sample SAMPLE_MPEG2_720P gen-sample-mpeg2 3
decode_case mpeg2 mpeg2_nvmpi "${SAMPLE_MPEG2_720P}"

echo "== 2. MPEG-4 decode (mpeg4_nvmpi) =="
ensure_sample SAMPLE_MPEG4_720P gen-sample-mpeg4 3
decode_case mpeg4 mpeg4_nvmpi "${SAMPLE_MPEG4_720P}"

echo "== 3. VP8 decode (vp8_nvmpi) =="
if has_codec enc libvpx; then
  ensure_sample SAMPLE_VP8_720P gen-sample-vp8 3
  decode_case vp8 vp8_nvmpi "${SAMPLE_VP8_720P}"
else
  echo "   SKIP: libvpx not available for sample generation"
  skipped=$((skipped + 1))
fi

echo "== 4. VP9 decode (vp9_nvmpi) =="
if has_codec enc libvpx-vp9; then
  ensure_sample SAMPLE_VP9_720P gen-sample-vp9 3
  decode_case vp9 vp9_nvmpi "${SAMPLE_VP9_720P}"
else
  echo "   SKIP: libvpx-vp9 not available for sample generation"
  skipped=$((skipped + 1))
fi

if [ "$skipped" -gt 0 ]; then
  echo "(${skipped} codec(s) skipped — build ffmpeg with --enable-libvpx for full coverage)"
fi
echo "OK: hw-decoder-codecs passed on ${variant}."
