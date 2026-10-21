#!/usr/bin/env bash
# Decoder frame_pool_size suite: boundary-value tests for the frame_pool_size
# AVOption (min=1, max=32, default=5). Controls the depth of libnvmpi's
# decoded-frame buffer pool (NVMPI_bufPool). A pool too small risks
# starvation; a pool too large wastes DMA memory. Both extremes must decode
# without deadlock, crash, or frame loss.
set -eu
# shellcheck source-path=SCRIPTDIR/..
# shellcheck source=lib/common.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../lib/common.sh"

variant="${JETSON_VARIANT:-unknown}"
echo "=== hw-decoder-pool on variant: ${variant} ==="

ensure_sample SAMPLE_H264_720P gen-sample-h264 3

# pool_decode LABEL POOL_SIZE
pool_decode() {
  local label="$1" pool="$2"
  # frame_pool_size is validated in nvmpi_init_decoder (ffmpeg/dev/common/
  # libavcodec/nvmpi_dec.c); the pool is NVMPI_bufPool (src/NVMPI_bufPool.hpp).
  # A timeout here points at a possible deadlock at pool depth ${pool}.
  run_ff_must "(${label}) decode with frame_pool_size=${pool}" -T 30 -loglevel error \
    -frame_pool_size:v "$pool" -c:v h264_nvmpi -i "${SAMPLE_H264_720P}" -f null -
  echo "   ${label}: decode OK with frame_pool_size=${pool}"
}

echo "== 1. minimum pool size (1) =="
pool_decode min 1

echo "== 2. default pool size (5) =="
pool_decode default 5

echo "== 3. maximum pool size (32) =="
pool_decode max 32

# ── 4. Resolution change with the frame pool full (jetson-ffmpeg#132) ──────
# One Annex B stream: 1 s 1280x720 still, then 1 s 642x358 still. With the old
# one-packet-in / at-most-one-frame-out decode callback, decoded frames piled up
# in libnvmpi's pool; at the resolution change put_packet then waited on the
# OUTPUT plane while the capture thread waited for a pool slot (hang). The
# receive_frame callback drains ready frames before every put, and libnvmpi
# >= 4.5.1 services the change while back-pressured. Old-size frames that were
# not taken yet when the change is serviced are discarded by libnvmpi (by
# design, jetson-nvmpi#143); every new-size frame must arrive. Pool sizes 1 and
# 5 (default) must both finish. Known limit: software
# frames keep the first geometry (libnvmpi's copying get_frame reports no
# size); DRM_PRIME frames carry the new one (hw-decoder-drmprime-planes).
# ffv1 (lossless) keeps the decoded pixels for the PSNR check.
echo "== 4. resolution change mid-stream, software frames (1280x720 -> 642x358) =="
SCHG=/tmp/nvmpi-pool-reschange.h264
SCTL=/tmp/nvmpi-pool-reschange-ctl.h264
OUT=/tmp/nvmpi-pool-reschange.mkv
trap 'rm -f "${SCHG}" "${SCHG}"-?.h264 "${SCTL}" "${SCTL}"-?.h264 "${OUT}" 2>/dev/null || true' EXIT
gen-sample-h264-reschange "${SCHG}"
# Control: same change to a 16-aligned size, to tell alignment/crop problems
# at the change from general loss.
gen-sample-h264-reschange "${SCTL}" 640x352
count_src() {
  ffprobe -v error -count_frames -select_streams v:0 \
    -show_entries stream=nb_read_frames -of default=nokey=1:nw=1 -f h264 "$1"
}
# reschange_case LABEL CLIP POOL: decode once to count what the decoder
# returns (showinfo, timestamps passed through, no muxer checks), once to
# ffv1/mkv (what survives the CLI's timestamp handling); both are printed so
# a failure says whether frames were lost in the decoder or dropped later.
# Floor: every new-size frame plus at least one old-size frame.
reschange_case() {
  local label="$1" clip="$2" pool="$3" exp min dec got w_h
  exp=$(count_src "$clip")
  min=$(( $(count_src "${clip}-b.h264") + 1 ))
  run_ff_must "(${label}) decoder-side count, pool ${pool}" -T 60 -loglevel info \
    -frame_pool_size:v "$pool" -c:v h264_nvmpi -f h264 -i "$clip" \
    -vf showinfo -fps_mode passthrough -f null -
  dec=$(printf '%s\n' "$FF_OUT" | grep -c 'Parsed_showinfo.* n: *[0-9]' || true)
  printf '%s\n' "$FF_OUT" | grep -iE 'transform|NvBufSurf|dropp|error' | head -5 | sed 's/^/   log: /' || true
  run_ff_must "(${label}) decode with frame_pool_size=${pool}" -T 60 -loglevel error \
    -frame_pool_size:v "$pool" -c:v h264_nvmpi -f h264 -i "$clip" -c:v ffv1 "${OUT}"
  got=$(packet_count "${OUT}")
  w_h=$(ffprobe -v error -select_streams v:0 -show_entries stream=width,height \
    -of csv=p=0:s=x "${OUT}")
  echo "   ${label} pool ${pool}: decoded ${dec}, muxed ${got}/${exp} frames (need ${min}..${exp}), ${w_h}"
  [ "${dec:-0}" -ge "$min" ] && [ "${got:-0}" -ge "$min" ] && [ "${got:-0}" -le "$exp" ] \
    || ff_fail "(${label}) pool ${pool}: decoded ${dec:-0}, muxed ${got:-0}/${exp} (need ${min}..${exp})."
  [ "$w_h" = "1280x720" ] || ff_fail "(${label}) pool ${pool}: expected 1280x720 frames, got ${w_h:-?}."
}
reschange_case aligned-ctl "${SCTL}" 5
for pool in 1 5; do
  reschange_case reschange "${SCHG}" "$pool"
done
if is_mock; then
  mock_skip "pixel check before the change (mock frames are not the source picture)"
else
  # Diagnostic (printed): software frames keep the 1280x720 allocation after
  # the change; libnvmpi copies the 642x358 picture into its top-left corner.
  # Cropped, the last frames show whether the decoder output itself is right
  # after the change (compare with the DRM_PRIME path in drmprime-planes).
  n=$(packet_count "${OUT}")
  run_ff_must "crop software frames after the change" -T 60 -loglevel error \
    -i "${OUT}" -vf "crop=642:358:0:0" -c:v ffv1 "${OUT%.mkv}-crop.mkv"
  echo "   diag: sw last 5 (cropped) vs 642x358 still: Y/U/V = $(plane_psnr "${SCHG}-b.h264" "${OUT%.mkv}-crop.mkv" 642 358 $((n - 5)) "" 15) dB"
  rm -f "${OUT%.mkv}-crop.mkv"
  # Frames 0-4 are the 1280x720 still: decoded pixels must match the source.
  vals=$(plane_psnr "${SCHG}-a.h264" "${OUT}" 1280 720)
  echo "   before the change: Y/U/V = ${vals} dB (min 30)"
  # shellcheck disable=SC2086
  set -- $vals
  awk -v a="${1:-0}" -v b="${2:-0}" -v c="${3:-0}" \
    'BEGIN { exit !(a >= 30 && b >= 30 && c >= 30) }' \
    || ff_fail "(reschange) a plane is below 30 dB before the change."
fi

# ── 5. Decode-order output keeps every frame (timestamps) ────────────────────
# disable_dpb=1 hands frames out in decode order, so their pts go backwards on
# a B-frame stream. Each output frame must carry an input packet's dts as
# pkt_dts (as libavcodec stamped it for the old decode callback), so that
# best_effort_timestamp stays monotonic; without it the CLI drops ~2/3 of the
# frames when muxing (35/90 seen on the #132 MR pipeline, same on the mock).
echo "== 5. decode-order output (disable_dpb=1) keeps its frames =="
OUT5=/tmp/nvmpi-pool-dpb.mkv
exp=$(packet_count "${SAMPLE_H264_720P}")
run_ff_must "(dpb) decode with disable_dpb=1" -T 60 -loglevel error \
  -disable_dpb 1 -c:v h264_nvmpi -i "${SAMPLE_H264_720P}" -c:v ffv1 "${OUT5}"
got=$(packet_count "${OUT5}")
rm -f "${OUT5}"
echo "   disable_dpb: ${got}/${exp} frames (min $((exp * 3 / 4)))"
[ "${got:-0}" -ge $((exp * 3 / 4)) ] \
  || ff_fail "(dpb) only ${got:-0}/${exp} frames survived muxing (frame timestamps out of order?)."

echo "OK: hw-decoder-pool passed on ${variant}."
