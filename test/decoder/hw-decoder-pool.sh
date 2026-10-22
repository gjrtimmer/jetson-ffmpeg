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

# ── 4. Resolution change mid-stream, software frames (#132, #137) ────────────
# One Annex B stream: 1 s still of one size, then 1 s still of another. With the
# old one-packet-in / at-most-one-frame-out decode callback, decoded frames piled
# up in libnvmpi's pool; at the resolution change put_packet then waited on the
# OUTPUT plane while the capture thread waited for a pool slot (hang, #132). The
# receive_frame callback drains ready frames before every put, and libnvmpi
# >= 4.5.1 services the change while back-pressured. Old-size frames that were
# not taken yet when the change is serviced are discarded by libnvmpi (by
# design, jetson-nvmpi#143); every new-size frame must arrive. Pool sizes 1 and
# 5 (default) must both finish.
# Software frames follow the decoded size (#137): libnvmpi >= 4.6.0
# (get_frame2) bounds the copy by the planes' capacity and reports a bigger
# frame instead of writing past the buffer; the wrapper then re-allocates at
# the new size. Before, a resolution INCREASE overflowed the heap buffer and
# every frame kept the first size. Both directions run: 1280x720 -> 642x358
# (shrink) and 642x358 -> 1280x720 (increase). ffv1 (lossless) keeps the
# decoded pixels for the PSNR check.
echo "== 4. resolution change mid-stream, software frames =="
SDN=/tmp/nvmpi-pool-shrink.h264
SUP=/tmp/nvmpi-pool-upsize.h264
SCTL=/tmp/nvmpi-pool-reschange-ctl.h264
OUT=/tmp/nvmpi-pool-reschange.mkv
RAW=/tmp/nvmpi-pool-reschange.yuv
trap 'rm -f "${SDN}" "${SDN}"-?.h264 "${SUP}" "${SUP}"-?.h264 "${SCTL}" "${SCTL}"-?.h264 "${OUT}" "${RAW}" "${RAW}.new" 2>/dev/null || true' EXIT
gen-sample-h264-reschange "${SDN}"
gen-sample-h264-upsize "${SUP}"
# Control: same change to a 16-aligned size, to tell alignment/crop problems
# at the change from general loss.
gen-sample-h264-reschange "${SCTL}" 640x352
count_src() {
  ffprobe -v error -count_frames -select_streams v:0 \
    -show_entries stream=nb_read_frames -of default=nokey=1:nw=1 -f h264 "$1"
}
# yuv_size WxH: bytes of one tightly packed yuv420p frame (odd sizes round
# the chroma planes up, as av_image_get_buffer_size with align 1).
yuv_size() {
  local w="${1%x*}" h="${1#*x}"
  echo $((w * h + 2 * ((w + 1) / 2) * ((h + 1) / 2)))
}
# reschange_case LABEL CLIP POOL OLD NEW: decode once with showinfo (what the
# decoder returns: the size of every frame, in order), then once more to raw
# video for the byte-exact size/count check and the PSNR check on what the
# CLI writes.
reschange_case() {
  local label="$1" clip="$2" pool="$3" old="$4" new="$5"
  local exp_old exp_new frames n_old n_new n_all last sz_old sz_new bytes rem vals
  exp_old=$(count_src "${clip}-a.h264")
  exp_new=$(count_src "${clip}-b.h264")
  run_ff_must "(${label}) decoder-side sizes, pool ${pool}" -T 60 -loglevel info \
    -frame_pool_size:v "$pool" -c:v h264_nvmpi -f h264 -i "$clip" \
    -vf showinfo -fps_mode passthrough -f null -
  printf '%s\n' "$FF_OUT" | grep -iE '_nvmpi @.*(error|size)|transform|NvBufSurf|dropp' | head -5 | sed 's/^/   log: /' || true
  # One "WxH" line per decoded frame, in output order.
  frames=$(printf '%s\n' "$FF_OUT" | grep 'Parsed_showinfo.* n: *[0-9]' \
    | sed -n 's/.* s:\([0-9]*x[0-9]*\) .*/\1/p')
  n_all=$(printf '%s\n' "$frames" | grep -c 'x' || true)
  n_old=$(printf '%s\n' "$frames" | grep -cx "${old}" || true)
  n_new=$(printf '%s\n' "$frames" | grep -cx "${new}" || true)
  last=$(printf '%s\n' "$frames" | tail -1)
  echo "   ${label} pool ${pool}: ${n_old}/${exp_old} frames ${old}, ${n_new}/${exp_new} frames ${new}, ${n_all} total, last ${last:-?}"
  [ "${n_new:-0}" -eq "$exp_new" ] \
    || ff_fail "(${label}) pool ${pool}: ${n_new:-0} frames of ${new}, expected exactly ${exp_new}."
  [ "${n_old:-0}" -ge 1 ] && [ "${n_old:-0}" -le "$exp_old" ] \
    || ff_fail "(${label}) pool ${pool}: ${n_old:-0} frames of ${old}, need 1..${exp_old}."
  [ "$n_all" -eq $((n_old + n_new)) ] \
    || ff_fail "(${label}) pool ${pool}: $((n_all - n_old - n_new)) frames of another size."
  [ "$last" = "$new" ] \
    || ff_fail "(${label}) pool ${pool}: frames after the change are ${last:-?}, expected ${new}."
  # Second pass to raw yuv420p, every frame at its own size (-noautoscale; the
  # rawvideo encoder writes each frame's own geometry): the byte count must be
  # some old-size frames plus exactly exp_new new-size frames, and the tail
  # is the new-size frames, wrapped in ffv1/mkv for the PSNR check.
  run_ff_must "(${label}) raw output, pool ${pool}" -T 60 -loglevel error \
    -frame_pool_size:v "$pool" -c:v h264_nvmpi -f h264 -i "$clip" \
    -noautoscale -fps_mode passthrough -pix_fmt yuv420p -c:v rawvideo -f rawvideo "${RAW}"
  sz_old=$(yuv_size "$old"); sz_new=$(yuv_size "$new")
  bytes=$(wc -c < "${RAW}")
  rem=$((bytes - exp_new * sz_new))
  echo "   ${label} pool ${pool}: raw ${bytes} bytes = $((rem / sz_old)) x ${old} + ${exp_new} x ${new} (remainder $((rem % sz_old)))"
  [ "$rem" -ge "$sz_old" ] && [ $((rem % sz_old)) -eq 0 ] && [ $((rem / sz_old)) -le "$exp_old" ] \
    || ff_fail "(${label}) pool ${pool}: raw output (${bytes} bytes) is not 1..${exp_old} x ${old} + ${exp_new} x ${new}."
  tail -c $((exp_new * sz_new)) "${RAW}" > "${RAW}.new"
  run_ff_must "(${label}) wrap the new-size frames" -T 60 -loglevel error \
    -f rawvideo -pix_fmt yuv420p -s "$new" -i "${RAW}.new" -c:v ffv1 "${OUT}"
  rm -f "${RAW}" "${RAW}.new"
  if is_mock; then
    mock_skip "(${label}) pixel check (mock frames are not the source picture)"
    return 0
  fi
  # Stills: the frames after the change must match the new source picture.
  vals=$(plane_psnr "${clip}-b.h264" "${OUT}" "${new%x*}" "${new#*x}")
  echo "   ${label} after the change: Y/U/V = ${vals} dB (min 30)"
  # shellcheck disable=SC2086
  set -- $vals
  awk -v a="${1:-0}" -v b="${2:-0}" -v c="${3:-0}" \
    'BEGIN { exit !(a >= 30 && b >= 30 && c >= 30) }' \
    || ff_fail "(${label}) pool ${pool}: a plane is below 30 dB after the change."
}
reschange_case aligned-ctl "${SCTL}" 5 1280x720 640x352
for pool in 1 5; do
  reschange_case shrink "${SDN}" "$pool" 1280x720 642x358
  reschange_case increase "${SUP}" "$pool" 642x358 1280x720
done

# ── 4b. libnvmpi without get_frame2 refuses software frames (#137) ──────────
# The old copying get_frame cannot be bounded, so software-frame output is
# refused at init (clear error, no crash) instead of risking the overflow.
# NVMPI_FORCE_NO_GET_FRAME2=1 fakes a libnvmpi < 4.6.0.
echo "== 4b. libnvmpi without get_frame2: software frames refused =="
export NVMPI_FORCE_NO_GET_FRAME2=1
run_ff -T 30 -loglevel error -c:v h264_nvmpi -f h264 -i "${SUP}" -f null - || true
unset NVMPI_FORCE_NO_GET_FRAME2
if [ "$FF_RC" -eq 0 ] || is_signal_rc "$FF_RC" || [ "$FF_RC" -eq 124 ] || [ "$FF_RC" -eq 137 ]; then
  ff_fail "(no get_frame2) expected a clean init failure, got rc=${FF_RC}."
fi
printf '%s\n' "$FF_OUT" | grep -q 'lacks nvmpi_decoder_get_frame2' \
  || ff_fail "(no get_frame2) the error does not name the missing get_frame2."
echo "   refused with a clear error (rc=${FF_RC})"

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
