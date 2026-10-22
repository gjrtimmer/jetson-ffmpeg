#!/usr/bin/env bash
# Encoder odd-frame-size suite: h264/hevc x {642x357, 641x357} x {yuv420p, nv12}.
# The wrapper must size each plane's payload_size from the real plane extent
# (chroma rows = ceil(height / 2)); rounding down one chroma row on an odd
# height is refused by libnvmpi >= 4.6.1 (put_frame: NVMPI_ERR_INVAL), which
# fails the encode (and the GLOBAL_HEADER extradata pre-encode of the mp4 muxer).
#
# Assertions (each is a hard failure):
#   1. Every combination encodes exactly FRAMES packets (no INVAL, no drops).
#   2. Real hardware only (the mock emits a canned bitstream): the output
#      decodes to FRAMES frames of the input size and the luma/chroma PSNR of a
#      still source stays above PSNR_MIN, i.e. the last chroma row is encoded.
#      Odd-width NV12 is an explicit XFAIL (jetson-nvmpi#154), XPASS fails.
# Code under test: ffmpeg/dev/common/libavcodec/nvmpi_enc_runtime.c
#   (nvmpienc_fill_plane_sizes, extradata blank frame, software send path)
# Issue: gjrtimmer/jetson-ffmpeg#138
set -eu
# shellcheck source-path=SCRIPTDIR/..
# shellcheck source=lib/common.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../lib/common.sh"

variant="${JETSON_VARIANT:-unknown}"
echo "=== hw-encoder-oddsize on variant: ${variant} ==="

FRAMES=10
PSNR_MIN=30
OUT=/tmp/nvmpi-out-oddsize.mp4
REF=/tmp/nvmpi-ref-oddsize.mkv
trap 'rm -f "$OUT" "$REF"' EXIT

# odd_check ENC SIZE PIXFMT: encode a still FRAMES times, check count (+ quality on hw).
odd_check() {
  local enc="$1" size="$2" pf="$3" label packets dec_frames w h psnr
  label="${enc} ${size} ${pf}"
  w="${size%x*}"; h="${size#*x}"
  rm -f "$OUT"

  # smptebars is static content, so frame-by-frame PSNR is meaningful.
  run_ff_must "${label} encode" -T 60 \
    -f lavfi -i "smptebars=s=${size}:r=30" \
    -frames:v ${FRAMES} -pix_fmt "$pf" -c:v "$enc" -b:v 3M "$OUT"
  packets=$(packet_count "$OUT")
  if [ "${packets:-0}" -ne "$FRAMES" ]; then
    ff_fail "${label}: expected ${FRAMES} packets, got ${packets:-0}."
  fi
  echo "   ${label}: ${packets}/${FRAMES} packets OK"

  if is_mock; then
    mock_skip "${label} decode + PSNR"
    return 0
  fi

  run_ff_must "${label} decode" -T 60 -i "$OUT" -f null -
  dec_frames=$(ffprobe -v error -count_frames -select_streams v:0 \
    -show_entries stream=nb_read_frames,width,height -of csv=p=0 "$OUT")
  if [ "$dec_frames" != "${w},${h},${FRAMES}" ]; then
    ff_fail "${label}: decoded ${dec_frames}, expected ${w},${h},${FRAMES}."
  fi

  run_ff_must "${label} reference" -T 60 \
    -f lavfi -i "smptebars=s=${size}:r=30" -frames:v ${FRAMES} -c:v ffv1 "$REF"
  psnr=$(plane_psnr "$REF" "$OUT" "$w" "$h")
  echo "   ${label}: PSNR Y/U/V = ${psnr}"
  local ok=1
  echo "$psnr" | awk -v m="$PSNR_MIN" '{ exit !($1 >= m && $2 >= m && $3 >= m) }' || ok=0
  # Known libnvmpi defect (jetson-nvmpi#154): the MMAP copy uses NvBuffer's
  # floored chroma geometry, so the last chroma column of an odd-WIDTH NV12
  # frame is never copied. Expected to fail; a pass (XPASS) is a hard failure
  # so the marker is removed once a fixed libnvmpi is pinned.
  if [ "$pf" = nv12 ] && [ $((w % 2)) -eq 1 ]; then
    if [ "$ok" -eq 1 ]; then
      ff_fail "${label}: XPASS - jetson-nvmpi#154 looks fixed; drop the XFAIL for odd-width NV12."
    fi
    echo "   XFAIL: chroma edge, jetson-nvmpi#154 (${label}, PSNR ${psnr})"
    return 0
  fi
  if [ "$ok" -eq 0 ]; then
    ff_fail "${label}: PSNR below ${PSNR_MIN} dB (${psnr})."
  fi
}

for enc in h264_nvmpi hevc_nvmpi; do
  for size in 642x357 641x357; do
    for pf in yuv420p nv12; do
      odd_check "$enc" "$size" "$pf"
    done
  done
done

echo "OK: hw-encoder-oddsize passed on ${variant}."
