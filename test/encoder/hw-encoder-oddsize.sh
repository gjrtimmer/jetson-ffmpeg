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
#      Odd sizes decode as the even-rounded size (4:2:0 crop units); PSNR runs
#      on the exact top-left w x h crop of each decoded frame (no scaling).
#      All cases are strict (odd-width chroma edge, jetson-nvmpi#154, fixed
#      in jetson-nvmpi 4.6.3).
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
PSNR_LOG=/tmp/nvmpi-psnr-oddsize.log
trap 'rm -f "$OUT" "$PSNR_LOG" /tmp/nvmpi-raw-oddsize.h264 /tmp/nvmpi-raw-oddsize.hevc' EXIT

# crop_psnr DEC SIZE CW CH CX CY : "Y U V" minimum PSNR over all FRAMES frames
# of the CW x CH window at (CX, CY) of DEC (decoded) vs the smptebars source at
# SIZE. The first FRAMES frames are paired by index (trim, setpts=N/TB; a raw
# Annex B stream has no timestamps). crop needs exact=1: without it crop
# rounds w/h/x/y down to the 4:2:0 subsampling (641x357 -> 640x356), and a
# rescale back to w x h shifts every edge (~31 dB on smptebars, the earlier
# false result).
# No scale: psnr refuses mismatched sizes instead of silently resampling.
crop_psnr() {
  local dec="$1" size="$2" crop="crop=${3}:${4}:${5}:${6}:exact=1"
  local pre="trim=end_frame=${FRAMES},setpts=N/TB,format=yuv420p"
  run_ff_must "psnr compare" -T 60 -loglevel error \
    -f lavfi -i "smptebars=s=${size}:r=30" -i "$dec" \
    -lavfi "[0:v]${pre},${crop}[a];[1:v]${pre},${crop}[b];[a][b]psnr=stats_file=${PSNR_LOG}" \
    -f null -
  # stats_file lines: n:1 mse_avg:.. psnr_y:48.3 psnr_u:.. psnr_v:..; inf -> 99
  awk '{ for (i = 1; i <= NF; i++) if ($i ~ /^psnr_[yuv]:/) { split($i, a, ":"); k = substr(a[1], 6, 1); v = (a[2] == "inf") ? 99 : a[2]; if (!(k in m) || v + 0 < m[k] + 0) m[k] = v } } END { printf "%s %s %s\n", m["y"], m["u"], m["v"] }' "$PSNR_LOG"
}

# odd_check ENC SIZE PIXFMT: encode a still FRAMES times, check count (+ quality on hw).
odd_check() {
  local enc="$1" size="$2" pf="$3" label packets w h psnr
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
  # Decoded size from the elementary stream (decoder output), not from the
  # container header: copy the packets to a raw .h264/.hevc file and probe the
  # first decoded frame. The container's stream size is printed for comparison.
  local raw_ext=h264 raw_fmt=h264 cont_dims dec_dims dec_n ew eh dw dh
  case "$enc" in hevc*) raw_ext=hevc; raw_fmt=hevc ;; esac
  local RAW="/tmp/nvmpi-raw-oddsize.${raw_ext}"
  run_ff_must "${label} raw copy" -T 60 -i "$OUT" -c:v copy -bsf:v "${raw_fmt}_mp4toannexb" -f "$raw_fmt" "$RAW"
  dec_dims=$(ffprobe -v error -select_streams v:0 -show_entries frame=width,height     -read_intervals "%+#1" -of csv=p=0 "$RAW" | awk -F, 'NR == 1 { print $1 "," $2 }')
  cont_dims=$(ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=p=0 "$OUT" | awk -F, 'NR == 1 { print $1 "," $2 }')
  dec_n=$(ffprobe -v error -count_frames -select_streams v:0 \
    -show_entries stream=nb_read_frames -of csv=p=0 "$OUT")
  echo "   ${label}: bitstream frame size ${dec_dims}, container size ${cont_dims}, ${dec_n} frames"
  # 4:2:0 cropping (H.264 frame_cropping, HEVC conformance window) is in units
  # of 2 luma pixels, so an odd size decodes as the even-rounded size.
  ew=$(( (w + 1) / 2 * 2 )); eh=$(( (h + 1) / 2 * 2 ))
  dw="${dec_dims%%,*}"; dh="${dec_dims##*,}"
  if [ "$dec_n" != "$FRAMES" ] || [ "$dw" != "$ew" ] || [ "$dh" != "$eh" ]; then
    ff_fail "${label}: decoded ${dec_dims} x${dec_n}, expected ${ew},${eh} x${FRAMES}."
  fi
  # PSNR on the original w x h area: top-left crop of each decoded frame of
  # the elementary stream, no scaling, paired by index with the w x h source.
  psnr=$(crop_psnr "$RAW" "$size" "$w" "$h" 0 0)
  echo "   ${label}: PSNR Y/U/V = ${psnr}"
  # Every frame must be paired: psnr stops at the shorter input.
  if [ "$(wc -l < "$PSNR_LOG")" -ne "$FRAMES" ]; then
    ff_fail "${label}: PSNR compared $(wc -l < "$PSNR_LOG") frames, expected ${FRAMES}."
  fi
  # Information only: the bottom 4 visible rows (next to the encoder padding).
  echo "   ${label}: bottom-4-rows PSNR Y/U/V = $(crop_psnr "$RAW" "$size" "$w" 4 0 $((h - 4)))"
  local ok=1
  echo "$psnr" | awk -v m="$PSNR_MIN" '{ exit !($1 >= m && $2 >= m && $3 >= m) }' || ok=0
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
