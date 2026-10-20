#!/usr/bin/env bash
# MJPEG DRM_PRIME (zero-copy) suite: mjpeg_nvmpi hands decoded JPEGs to the
# next stage as DMA-BUF fds (libnvmpi >= 4.1.0 JPEG fd API) instead of copying
# them into software frames. Covers: the full zero-copy pipeline
# (MJPEG -> drm_prime -> scale_vic -> h264_nvmpi), pixel correctness on
# padded/unaligned surfaces and 4:2:2 / 4:4:4 sources, odd sizes (decode
# only), early exit with frames still queued (the codec is closed before the
# frames are unref'd), a dup'd-fd leak check, the range tag, the 4.0.x
# fallback (symbols forced off) and the unchanged default software path.
# Hardware only: CI's mock job runs the released libnvmpi, which links the real
# NvJPEGDecoder sample class (libjpeg ABI mismatch against the mock libs), so the
# mock JPEG model of jetson-nvmpi 4.2.1 cannot be used here and this suite is not
# in MOCK_SUITES. No rapid open/close loops (Tegra driver segfaults).
set -eu
# shellcheck source-path=SCRIPTDIR/..
# shellcheck source=lib/common.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../lib/common.sh"

variant="${JETSON_VARIANT:-unknown}"
echo "=== hw-decoder-mjpeg-drmprime on variant: ${variant} ==="

if is_mock; then
  mock_skip "NvJPEG decode is not mocked for the released libnvmpi"
  exit 0
fi
has_codec dec mjpeg_nvmpi || { echo "SKIP: mjpeg_nvmpi not compiled in."; exit 0; }
has_sw_encoder mjpeg || { echo "SKIP: software mjpeg encoder missing (sample generation)."; exit 0; }
ffmpeg -hide_banner -filters 2>&1 | grep -q scale_vic || ff_fail "scale_vic filter missing."

T=/tmp/nvmpi-mjpeg-drm
S720="${T}-720p.mkv"          # 1280x720, 3 s
SLONG="${T}-long.mkv"         # 1280x720, 10 s (fd-leak run)
ensure_sample SAMPLE_MJPEG_720P gen-sample-mjpeg 3
gen-sample-mjpeg "${SLONG}" 10
cp -f "${SAMPLE_MJPEG_720P}" "${S720}"
trap 'rm -f "${T}"-* 2>/dev/null || true' EXIT

# Decode with DRM_PRIME output via the nvmpi hwaccel device (the same trigger
# the video decoders use; -hwaccel also attaches the hw_frames_ctx filters need).
DEC_HW=(-hwaccel nvmpi -c:v mjpeg_nvmpi)

frames_of() { packet_count "$1"; }   # demuxed packet count (0 on error)

# plane_psnr REF DEC W H: prints "Y U V" (minimum PSNR per plane over the
# compared frames) between two files, both software decoded and treated as full
# range at WxH. Used to compare the hw path against an encoder-only baseline.
plane_psnr() {
  local ref="$1" dec="$2" w="$3" h="$4" log="${T}-psnr.log"
  run_ff_must "psnr: compare" -T 60 -loglevel error \
    -i "$ref" -i "$dec" -frames:v 5 \
    -lavfi "[0:v]scale=${w}:${h}:in_range=pc:out_range=pc,format=yuv420p[a];[1:v]scale=${w}:${h}:in_range=pc:out_range=pc,format=yuv420p[b];[a][b]psnr=stats_file=${log}" \
    -f null -
  # stats_file lines: n:1 mse_avg:.. psnr_y:48.3 psnr_u:.. psnr_v:..
  awk '{ for (i = 1; i <= NF; i++) if ($i ~ /^psnr_[yuv]:/) { split($i, a, ":"); k = substr(a[1], 6, 1); v = (a[2] == "inf") ? 99 : a[2]; if (!(k in m) || v + 0 < m[k] + 0) m[k] = v } } END { printf "%s %s %s\n", m["y"], m["u"], m["v"] }' "$log"
}

# psnr_ok HW BASE LABEL: every plane of the drm_prime path must reach 30 dB
# against the libjpeg decode. A wrong plane layout or chroma offset turns
# chroma into garbage (~10-20 dB); the hardware decode itself is ~40 dB (VIC
# rounding, NV12 pool vs the software-frame path). BASE (the same decoder's
# software-frame path through the same encoder) is printed for diagnosis only.
psnr_ok() {
  local hw="$1" base="$2" label="$3"
  echo "   ${label}: drm_prime Y/U/V = ${hw} dB (software-frame path: ${base} dB)"
  # shellcheck disable=SC2086
  set -- $hw
  awk -v a="${1:-0}" -v b="${2:-0}" -v c="${3:-0}" \
    'BEGIN { exit !(a >= 30 && b >= 30 && c >= 30) }' \
    || { echo "FAIL(${label}): a plane is below 30 dB (plane layout wrong?)."; exit 1; }
}

# ── 1. Zero-copy pipeline: MJPEG -> drm_prime -> scale_vic -> h264_nvmpi ─────
echo "== 1. MJPEG drm_prime -> scale_vic -> h264_nvmpi =="
out="${T}-pipe.mkv"
run_ff_must "pipeline" -T 90 -loglevel warning \
  "${DEC_HW[@]}" -i "${S720}" -vf scale_vic=640:360 -c:v h264_nvmpi "$out"
if printf '%s\n' "$FF_OUT" | grep -qi "falling back to software"; then
  ff_fail "drm_prime fell back to software: libnvmpi >= 4.1.0 is required for this suite."
fi
exp=$(frames_of "${S720}"); got=$(frames_of "$out")
w=$(ffprobe -v error -select_streams v:0 -show_entries stream=width -of default=nokey=1:nw=1 "$out")
h=$(ffprobe -v error -select_streams v:0 -show_entries stream=height -of default=nokey=1:nw=1 "$out")
if [ "${w:-0}" -ne 640 ] || [ "${h:-0}" -ne 360 ]; then
  echo "FAIL(pipeline): expected 640x360, got ${w:-?}x${h:-?}."; exit 1
fi
if [ "${got:-0}" -lt $((exp - 1)) ]; then
  echo "FAIL(pipeline): ${got:-0}/${exp} frames reached the encoder."; exit 1
fi
echo "   pipeline: ${got}/${exp} frames, ${w}x${h}"

# ── 2. Pixel correctness: padded surfaces and chroma formats ─────────────────
# Encode straight from the DRM_PRIME frame (encoder fd path, which reads the
# descriptor's real chroma offset) and compare to the software decode. A wrong
# plane offset corrupts chroma, so the minimum of psnr_y/u/v is the check.
echo "== 2. pixel correctness (encoder fd path vs software decode) =="
# CASES: WxH:pix_fmt. 1000x562 and 642x358 are even but not 16-aligned (the
# surface pads the plane height); 422/444 are converted to NV12 by the VIC.
for c in 1280x720:yuvj420p 1000x562:yuvj420p 642x358:yuvj420p \
         640x360:yuvj422p 640x360:yuvj444p; do
  size="${c%%:*}"; pf="${c##*:}"; tag="${size}-${pf}"
  in="${T}-${tag}.mkv"; enc="${T}-${tag}-enc.mkv"
  gen-sample-mjpeg-fmt "$in" "$size" "$pf" 1
  # 4:2:2 / 4:4:4 only where the NVJPG decoder accepts the format at all
  # (the legacy software-frame path is the probe); otherwise skip the case.
  if [ "$pf" != yuvj420p ] && ! run_ff -T 30 -loglevel error \
       -c:v mjpeg_nvmpi -i "$in" -frames:v 1 -f null -; then
    echo "   ${tag}: SKIP (NVJPG decoder rejects this JPEG format even with software frames)"
    continue
  fi
  run_ff_must "${tag}: hw decode+encode" -T 60 -loglevel error \
    "${DEC_HW[@]}" -i "$in" -c:v h264_nvmpi -b:v 20M "$enc"
  run_ff_must "${tag}: baseline encode" -T 60 -loglevel error \
    -c:v mjpeg_nvmpi -i "$in" -vf "scale=in_range=pc:out_range=pc,format=yuv420p" \
    -c:v h264_nvmpi -b:v 20M "${enc}.base.mkv"
  psnr_ok "$(plane_psnr "$in" "$enc" "${size%x*}" "${size#*x}")" \
          "$(plane_psnr "$in" "${enc}.base.mkv" "${size%x*}" "${size#*x}")" "$tag"
done

# ── 3. Odd sizes (decode only; H.264 cannot encode them) ─────────────────────
echo "== 3. odd sizes decode =="
for size in 33x17 65x33; do
  in="${T}-odd-${size}.mkv"
  gen-sample-mjpeg-fmt "$in" "$size" yuvj420p 1
  run_ff_must "odd ${size}" -T 60 -loglevel warning \
    "${DEC_HW[@]}" -i "$in" -f null -
  if printf '%s\n' "$FF_OUT" | grep -qi "inconsistent DMA-BUF JPEG layout"; then
    ff_fail "odd ${size}: layout rejected."
  fi
  echo "   odd ${size}: decoded"
done

# ── 4. Close before unref: early exit with frames still queued ───────────────
# ffmpeg 8.1+ closes the codec before queued frames are unref'd; the dup'd fd
# (kept by the frame) and libnvmpi's deferred destroy must make that safe.
# -frames:v 1 stops the run while the decoder thread still holds frames.
# Two sequential runs, no rapid cycling.
echo "== 4. decoder closed with queued frames =="
for run in 1 2; do
  run_ff_must "early-exit run ${run}" -T 60 -loglevel error \
    "${DEC_HW[@]}" -frame_pool_size 8 -i "${SLONG}" -frames:v 1 \
    -vf scale_vic=640:360 -c:v h264_nvmpi "${T}-early.mkv"
  sleep 1
done
echo "   early exit x2: no crash"

# ── 5. fd leak: dup'd fds must be closed when frames are released ────────────
echo "== 5. fd count stable over a paced run =="
ffmpeg -hide_banner -loglevel error -y "${DEC_HW[@]}" -re -i "${SLONG}" \
  -vf scale_vic=640:360 -c:v h264_nvmpi "${T}-leak.mkv" &
pid=$!
sleep 3
fd1=$(find "/proc/${pid}/fd" -mindepth 1 2>/dev/null | wc -l)
sleep 5
fd2=$(find "/proc/${pid}/fd" -mindepth 1 2>/dev/null | wc -l)
wait "$pid" || { echo "FAIL(fd-leak): paced run failed."; exit 1; }
# ~150 frames pass between the samples; a per-frame leak would add ~150.
if [ "$fd1" -eq 0 ] || [ $((fd2 - fd1)) -gt 4 ]; then
  echo "FAIL(fd-leak): fds ${fd1} -> ${fd2} during the run."; exit 1
fi
echo "   fds: ${fd1} -> ${fd2}"

# ── 6. Range tag: full range on every frame ──────────────────────────────────
echo "== 6. color range =="
# The filter-graph input line logged at verbose level carries the frame's
# range (FFmpeg >= 6.1 prints "range:"); older versions do not log it.
run_ff_must "range" -T 60 -loglevel verbose "${DEC_HW[@]}" -i "${S720}" \
  -frames:v 2 -vf scale_vic=640:360 -c:v h264_nvmpi "${T}-range.mkv"
line=$(printf '%s\n' "$FF_OUT" | grep -m1 'pixfmt:drm_prime' || true)
case "$line" in
  *range:pc*) echo "   range: pc (full)" ;;
  *range:*)   echo "FAIL(range): drm_prime frames not tagged full range: ${line}"; exit 1 ;;
  *)          echo "   range: not logged by this FFmpeg version, skipped" ;;
esac

# ── 7. Fallback: libnvmpi without the fd API (symbols forced off) ────────────
echo "== 7. fallback to software frames =="
NVMPI_FORCE_NO_JPEG_FD=1 run_ff_must "fallback" -T 60 -loglevel info \
  -c:v mjpeg_nvmpi -output_format drm_prime -i "${S720}" -f null -
printf '%s\n' "$FF_OUT" | grep -qi "falling back to software" \
  || ff_fail "fallback: no warning logged."
assert_frames 10 "fallback decode"

# ── 8. Default path unchanged: no drm_prime without a request ────────────────
echo "== 8. default path stays software =="
run_ff_must "default" -T 60 -loglevel verbose \
  -c:v mjpeg_nvmpi -i "${S720}" -f null -
if printf '%s\n' "$FF_OUT" | grep -qi "drm_prime\|falling back"; then
  ff_fail "default: unexpected drm_prime / fallback activity."
fi
echo "   default: software frames"

echo "OK: hw-decoder-mjpeg-drmprime passed on ${variant}."
