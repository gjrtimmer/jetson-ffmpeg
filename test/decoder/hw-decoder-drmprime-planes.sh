#!/usr/bin/env bash
# Video DRM_PRIME plane-layout suite (jetson-ffmpeg#131): the h264/hevc/...
# decoders describe each DRM_PRIME frame with the REAL per-plane pitch/offset
# from libnvmpi >= 4.3.0 (nvmpi_decoder_get_frame_fd2) instead of deriving the
# chroma offset as pitch*height, which is wrong whenever the allocator pads
# the luma plane. Covers: fd2 in use (no legacy warning), chroma correctness on
# a size that is not 16-aligned (encoder fd path reads the descriptor offsets;
# a wrong chroma offset corrupts U/V), a resolution change mid-stream, and the
# forced pre-4.3.0 fallback (NVMPI_FORCE_NO_FD2=1). On the mock the pixel
# comparisons are skipped (canned slices are not software-decodable; asserted
# on hardware only). No rapid open/close loops (Tegra driver segfaults).
set -eu
# shellcheck source-path=SCRIPTDIR/..
# shellcheck source=lib/common.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../lib/common.sh"

variant="${JETSON_VARIANT:-unknown}"
echo "=== hw-decoder-drmprime-planes on variant: ${variant} ==="

has_codec dec h264_nvmpi || { echo "SKIP: h264_nvmpi not compiled in."; exit 0; }
ffmpeg -hide_banner -filters 2>&1 | grep -q scale_vic || ff_fail "scale_vic filter missing."

T=/tmp/nvmpi-drm-planes
trap 'rm -f "${T}"-* 2>/dev/null || true' EXIT

# DRM_PRIME output via the nvmpi hwaccel device (also attaches hw_frames_ctx).
DEC_HW=(-hwaccel nvmpi -c:v h264_nvmpi)
FALLBACK_MSG="lacks nvmpi_decoder_get_frame_fd2"

# gen_h264 FILE WxH SECONDS: software-encoded raw Annex B H.264 of a STILL
# (frame 0 of testsrc2 repeated, no B-frames). Still content makes every
# PSNR comparison independent of frame pairing: the hw decode+encode chain may
# shift, drop or duplicate a frame, and on the moving pattern that alone costs
# ~25 dB (the first version of this suite measured ~22.8 dB for every variant).
gen_h264() {
  ffmpeg -y -hide_banner -loglevel error \
    -f lavfi -i "testsrc2=s=$2:r=30" \
    -vf "select='eq(n\,0)',loop=loop=-1:size=1:start=0" -t "$3" -pix_fmt yuv420p \
    -c:v libx264 -g 15 -bf 0 -f h264 "$1"
}

# plane_psnr (lib/common.sh) prints "Y U V" min PSNR over 5 frames.
# psnr_ok "Y U V" LABEL THRESHOLD: every plane must reach THRESHOLD dB.
psnr_ok() {
  local vals="$1" label="$2" min="$3"
  echo "   ${label}: Y/U/V = ${vals} dB (min ${min})"
  # shellcheck disable=SC2086
  set -- $vals
  awk -v a="${1:-0}" -v b="${2:-0}" -v c="${3:-0}" -v m="$min" \
    'BEGIN { exit !(a >= m && b >= m && c >= m) }' \
    || { echo "FAIL(${label}): a plane is below ${min} dB."; exit 1; }
}

dims_of() {
  ffprobe -v error -select_streams v:0 -show_entries stream=width,height \
    -of csv=p=0:s=x "$1"
}

# ── 1. fd2 in use: the legacy pitch*height path must not be taken ───────────
echo "== 1. libnvmpi fd2 API used (no pitch*height fallback) =="
S720="${T}-720.h264"
gen_h264 "${S720}" 1280x720 2
run_ff_must "fd2 decode" -T 60 -loglevel warning \
  "${DEC_HW[@]}" -f h264 -i "${S720}" -vf scale_vic=640:360 -c:v h264_nvmpi "${T}-fd2.mkv"
if printf '%s\n' "$FF_OUT" | grep -q "$FALLBACK_MSG"; then
  ff_fail "legacy pitch*height path used: libnvmpi >= 4.3.0 is required for this suite."
fi
[ "$(packet_count "${T}-fd2.mkv")" -ge 1 ] || ff_fail "fd2 decode produced no packets."
echo "   fd2 path: output $(dims_of "${T}-fd2.mkv")"

# ── 2. Chroma correctness on a size the surface pads ────────────────────────
# 642x358 is even but not 16-aligned: the allocator pads the planes, so a chroma
# offset of pitch*height lands inside the luma padding. Encode straight from the
# DRM_PRIME frame (encoder fd path, which reads the descriptor offsets) and
# compare to the source; a wrong offset drops U/V far below 30 dB. Primary
# check: the same decoder's software-frame output through the same encoder
# (no DRM_PRIME) must match the drm_prime output (>= 40 dB against each other).
echo "== 2. chroma correctness on a padded size (642x358) =="
# Control first: a 16-aligned size through the same pipeline and PSNR method.
# If it fails, the harness (encoder/decoder chain) is at fault, not the layout.
SCTL="${T}-ctl.h264"
gen_h264 "${SCTL}" 640x352 1
run_ff_must "aligned-size hw decode+encode" -T 60 -loglevel error \
  "${DEC_HW[@]}" -f h264 -i "${SCTL}" -c:v h264_nvmpi -b:v 20M "${T}-ctl.mkv"
SODD="${T}-odd.h264"
gen_h264 "${SODD}" 642x358 1
run_ff_must "odd-size hw decode+encode" -T 60 -loglevel verbose \
  "${DEC_HW[@]}" -f h264 -i "${SODD}" -c:v h264_nvmpi -b:v 20M "${T}-odd.mkv"
printf '%s\n' "$FF_OUT" | grep "DRM_PRIME layout" | sed 's/^/   /' || true
# Software-frame baseline (same decoder, no DRM_PRIME, same encoder).
run_ff_must "odd-size software-frame baseline" -T 60 -loglevel error \
  -c:v h264_nvmpi -f h264 -i "${SODD}" -pix_fmt yuv420p -c:v h264_nvmpi -b:v 20M "${T}-odd-sw.mkv"
if is_mock; then
  mock_skip "pixel comparison (canned slices are not software-decodable)"
else
  psnr_ok "$(plane_psnr "${SCTL}" "${T}-ctl.mkv" 640 352)" "aligned control 640x352 vs source" 30
  psnr_ok "$(plane_psnr "${SODD}" "${T}-odd-sw.mkv" 642 358)" "642x358 software-frame vs source" 30
  psnr_ok "$(plane_psnr "${SODD}" "${T}-odd.mkv" 642 358)" "642x358 drm_prime vs source" 30
  psnr_ok "$(plane_psnr "${T}-odd-sw.mkv" "${T}-odd.mkv" 642 358)" "642x358 drm_prime vs software-frame" 40
fi

# ── 3. Resolution change mid-stream ─────────────────────────────────────────
# Two Annex B streams back to back: 1280x720 then 642x358 (new SPS at an IDR).
# libnvmpi starts a new pool generation, frames keep their own geometry, and
# scale_vic reconfigures to the new hw_frames_ctx. No frame may be lost or
# carry the old layout. -noautoscale: after the encoder opened, ffmpeg's graph
# reinit would otherwise insert a software "auto_scale" behind scale_vic, which
# cannot convert DRM_PRIME frames ("Impossible to convert between the formats").
echo "== 3. resolution change mid-stream (1280x720 -> 642x358) =="
SCHG="${T}-change.h264"
gen_h264 "${T}-chg-a.h264" 1280x720 1
gen_h264 "${T}-chg-b.h264" 642x358 1
cat "${T}-chg-a.h264" "${T}-chg-b.h264" > "${SCHG}"
run_ff_must "resolution-change decode" -T 90 -loglevel warning \
  "${DEC_HW[@]}" -f h264 -i "${SCHG}" -vf scale_vic=640:360 -noautoscale \
  -c:v h264_nvmpi "${T}-change.mkv"
if printf '%s\n' "$FF_OUT" | grep -qi "inconsistent DMA-BUF layout"; then
  ff_fail "decoder reported an inconsistent DMA-BUF layout after the resolution change."
fi
if is_mock; then
  mock_skip "frame count and chroma check across the change"
else
  exp=$(ffprobe -v error -count_frames -select_streams v:0 \
    -show_entries stream=nb_read_frames -of default=nokey=1:nw=1 -f h264 "${SCHG}")
  got=$(packet_count "${T}-change.mkv")
  w_h=$(dims_of "${T}-change.mkv")
  if [ "$w_h" != "640x360" ]; then
    echo "FAIL(change): expected 640x360, got ${w_h:-?}."; exit 1
  fi
  if [ "${got:-0}" -lt $((exp - 1)) ]; then
    echo "FAIL(change): ${got:-0}/${exp} frames survived the resolution change."; exit 1
  fi
  echo "   change: ${got}/${exp} frames, ${w_h}"
  # Chroma after the change: the stream is scaled to 640x360 by VIC (hw) and
  # swscale (reference), so the bound is loose (25 dB), but a wrong chroma
  # offset still lands far below it. Frames 45-49 of the output are all the
  # 642x358 still; the reference is the 642x358 clip alone (a resolution
  # change inside the reference would reset ffmpeg's frame counter).
  psnr_ok "$(plane_psnr "${T}-chg-b.h264" "${T}-change.mkv" 640 360 45 "" 15)" "change" 25
fi

# ── 4. Forced pre-4.3.0 library: legacy path still works, warns once ────────
echo "== 4. fallback without nvmpi_decoder_get_frame_fd2 =="
NVMPI_FORCE_NO_FD2=1 run_ff_must "legacy-path decode" -T 60 -loglevel warning \
  "${DEC_HW[@]}" -f h264 -i "${S720}" -vf scale_vic=640:360 -c:v h264_nvmpi "${T}-legacy.mkv"
printf '%s\n' "$FF_OUT" | grep -q "$FALLBACK_MSG" \
  || ff_fail "legacy path did not log the fd2 warning (NVMPI_FORCE_NO_FD2 hook broken?)."
[ "$(packet_count "${T}-legacy.mkv")" -ge 1 ] || ff_fail "legacy path produced no packets."
echo "   legacy path: output $(dims_of "${T}-legacy.mkv"), warning logged"

echo "OK: hw-decoder-drmprime-planes passed on ${variant}."
