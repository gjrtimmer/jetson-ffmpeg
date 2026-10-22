#!/usr/bin/env bash
# MJPEG hardware decoder test suite: exercises the mjpeg_nvmpi decoder path
# which uses NvJPEGDecoder (synchronous per-frame decode) rather than the V4L2
# M2M pipeline used by other codecs. Tests: basic decode, frame count accuracy,
# PSNR correctness vs software decode, multi-stream concurrency,
# resolution-change handling and a growing image size (#137).
set -eu
# shellcheck source-path=SCRIPTDIR/..
# shellcheck source=lib/common.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../lib/common.sh"

variant="${JETSON_VARIANT:-unknown}"
echo "=== hw-decoder-mjpeg on variant: ${variant} ==="

# ── 1. Basic decode ──────────────────────────────────────────────────────────
echo "== 1. Basic MJPEG decode (mjpeg_nvmpi) =="
ensure_sample SAMPLE_MJPEG_720P gen-sample-mjpeg 3

run_ff_must "basic-decode: mjpeg_nvmpi" -T 60 -loglevel error \
  -c:v mjpeg_nvmpi -i "${SAMPLE_MJPEG_720P}" -f null -
echo "   basic-decode: OK"

# ── 2. Frame count accuracy ─────────────────────────────────────────────────
echo "== 2. Frame count accuracy =="
tmpout="/tmp/nvmpi-dec-mjpeg-fc.mkv"
expected=$(packet_count "${SAMPLE_MJPEG_720P}")

run_ff_must "frame-count: mjpeg_nvmpi decode+transcode" -T 60 -loglevel error \
  -c:v mjpeg_nvmpi -i "${SAMPLE_MJPEG_720P}" -c:v ffv1 "$tmpout"

actual=$(packet_count "$tmpout")
# MJPEG decode is synchronous — no pipeline buffering, so every frame should
# be emitted. Allow 1-frame tolerance for container overhead.
min_expected=$((expected - 1))
if [ "${actual:-0}" -lt "$min_expected" ]; then
  echo "FAIL(frame-count): decoded ${actual:-0}/${expected} frames (min ${min_expected})."
  exit 1
fi
echo "   frame-count: ${actual}/${expected} frames decoded"

# ── 3. PSNR correctness vs software decode ───────────────────────────────────
echo "== 3. PSNR correctness (hw vs sw) =="
hw_raw="/tmp/nvmpi-mjpeg-hw.yuv"
sw_raw="/tmp/nvmpi-mjpeg-sw.yuv"

# Decode with hw and sw decoders to raw YUV for comparison
run_ff_must "psnr: hw decode to raw" -T 60 -loglevel error \
  -c:v mjpeg_nvmpi -i "${SAMPLE_MJPEG_720P}" \
  -pix_fmt yuv420p -f rawvideo "$hw_raw"

run_ff_must "psnr: sw decode to raw" -T 60 -loglevel error \
  -c:v mjpeg -i "${SAMPLE_MJPEG_720P}" \
  -pix_fmt yuv420p -f rawvideo "$sw_raw"

# Compare sizes first — must match exactly for PSNR to be valid
hw_size=$(stat -c%s "$hw_raw")
sw_size=$(stat -c%s "$sw_raw")
if [ "$hw_size" -ne "$sw_size" ]; then
  echo "FAIL(psnr): raw file size mismatch: hw=${hw_size} sw=${sw_size}."
  exit 1
fi

# PSNR via ffmpeg's psnr filter — extract average PSNR from log
psnr_log="/tmp/nvmpi-mjpeg-psnr.log"
run_ff_must "psnr: psnr filter" -T 60 -loglevel error \
  -f rawvideo -pix_fmt yuv420p -s 1280x720 -i "$hw_raw" \
  -f rawvideo -pix_fmt yuv420p -s 1280x720 -i "$sw_raw" \
  -lavfi "psnr=stats_file=${psnr_log}" -f null -

# Extract average PSNR from last line (summary)
avg_psnr=$(tail -1 "$psnr_log" | grep -oP 'psnr_avg:\K[0-9.]+' || echo "0")
# HW decode goes through VIC (block-linear → pitch-linear) which introduces
# color-space conversion rounding vs pure SW libjpeg. 25 dB proves visual
# correctness; values below ~20 dB indicate a real decode error.
psnr_int=${avg_psnr%%.*}
if [ "${psnr_int:-0}" -lt 25 ]; then
  echo "FAIL(psnr): avg PSNR ${avg_psnr} dB < 25 dB threshold."
  exit 1
fi
echo "   psnr: avg ${avg_psnr} dB (>= 25 dB threshold)"

# ── 4. Multi-stream concurrency ─────────────────────────────────────────────
echo "== 4. Multi-stream concurrency (3 parallel) =="
pids=()
rc_all=0

for _ in 1 2 3; do
  ( run_ff -T 60 -loglevel error \
      -c:v mjpeg_nvmpi -i "${SAMPLE_MJPEG_720P}" -f null - ) &
  pids+=($!)
done

for pid in "${pids[@]}"; do
  wait "$pid" || rc_all=$((rc_all + 1))
done

if [ "$rc_all" -gt 0 ]; then
  echo "FAIL(multi-stream): ${rc_all}/3 concurrent decode sessions failed."
  exit 1
fi
echo "   multi-stream: 3/3 concurrent sessions OK"

# ── 5. Resolution change ────────────────────────────────────────────────────
echo "== 5. Resolution change (720p → 480p concat) =="
ensure_sample SAMPLE_MJPEG_480P gen-sample-mjpeg-480p 2
concat_file="/tmp/nvmpi-mjpeg-concat.txt"
concat_out="/tmp/nvmpi-mjpeg-concat-out.mkv"

printf "file '%s'\nfile '%s'\n" "${SAMPLE_MJPEG_720P}" "${SAMPLE_MJPEG_480P}" \
  > "$concat_file"

# Concat demuxer feeds both resolutions into a single decode session.
# The JPEG decoder must handle frame pool reallocation on size change.
run_ff_must "resolution-change: decode of concatenated resolutions" -T 60 -loglevel error \
  -c:v mjpeg_nvmpi -f concat -safe 0 -i "$concat_file" \
  -c:v ffv1 "$concat_out"

# Verify some frames came through from both segments
actual=$(packet_count "$concat_out")
exp_720=$(packet_count "${SAMPLE_MJPEG_720P}")
exp_480=$(packet_count "${SAMPLE_MJPEG_480P}")
total_expected=$((exp_720 + exp_480))
min_total=$((total_expected * 75 / 100))
if [ "${actual:-0}" -lt "$min_total" ]; then
  echo "FAIL(resolution-change): ${actual}/${total_expected} frames (min ${min_total})."
  exit 1
fi
echo "   resolution-change: ${actual}/${total_expected} frames decoded"

# ── 6. Image size grows mid-stream (#137) ───────────────────────────────────
# A larger JPEG must not be copied into the smaller software frame: libnvmpi
# >= 4.6.0 refuses it (NVMPI_ERR_FRAME_SIZE, image size reported), the
# decoder re-allocates and decodes the packet again. JPEG decode is
# synchronous, so every frame must come out, each at its own size: the raw
# output (-noautoscale) is exactly n_a small frames plus n_b large ones.
echo "== 6. Image size grows mid-stream (640x360 -> 1280x720) =="
grow="/tmp/nvmpi-mjpeg-grow.mjpeg"
grow_raw="/tmp/nvmpi-mjpeg-grow.yuv"
gen-sample-mjpeg-grow "$grow"
n_a=$(packet_count "${grow}-a.mkv")
n_b=$(packet_count "${grow}-b.mkv")
sz_a=$((640 * 360 * 3 / 2))
sz_b=$((1280 * 720 * 3 / 2))
run_ff_must "grow: decode, every frame at its own size" -T 60 -loglevel error \
  -c:v mjpeg_nvmpi -f mjpeg -i "$grow" -noautoscale -fps_mode passthrough \
  -pix_fmt yuv420p -f rawvideo "$grow_raw"
bytes=$(stat -c%s "$grow_raw")
echo "   grow: raw ${bytes} bytes, expected ${n_a} x 640x360 + ${n_b} x 1280x720 = $((n_a * sz_a + n_b * sz_b))"
if [ "$bytes" -ne $((n_a * sz_a + n_b * sz_b)) ]; then
  echo "FAIL(grow): raw output is not ${n_a} x 640x360 + ${n_b} x 1280x720 frames."
  exit 1
fi
# The 1280x720 frames (tail) against the software decode of the same images.
tail -c $((n_b * sz_b)) "$grow_raw" > "${grow_raw}.b"
run_ff_must "grow: sw decode of the 1280x720 part" -T 60 -loglevel error \
  -c:v mjpeg -i "${grow}-b.mkv" -pix_fmt yuv420p -f rawvideo "$sw_raw"
run_ff_must "grow: psnr filter" -T 60 -loglevel error \
  -f rawvideo -pix_fmt yuv420p -s 1280x720 -i "${grow_raw}.b" \
  -f rawvideo -pix_fmt yuv420p -s 1280x720 -i "$sw_raw" \
  -lavfi "psnr=stats_file=${psnr_log}" -f null -
avg_psnr=$(tail -1 "$psnr_log" | grep -oP 'psnr_avg:\K[0-9.]+' || echo "0")
psnr_int=${avg_psnr%%.*}
if [ "${psnr_int:-0}" -lt 25 ]; then
  echo "FAIL(grow): 1280x720 frames avg PSNR ${avg_psnr} dB < 25 dB threshold."
  exit 1
fi
echo "   grow: 1280x720 frames avg PSNR ${avg_psnr} dB (>= 25 dB threshold)"

# ── Cleanup ──────────────────────────────────────────────────────────────────
rm -f "$hw_raw" "$sw_raw" "$psnr_log" "$tmpout" "$concat_file" "$concat_out" \
      "$grow" "${grow}"-?.mkv "$grow_raw" "${grow_raw}.b" \
      /tmp/nvmpi-dec-mjpeg-*.mkv 2>/dev/null || true

echo "OK: hw-decoder-mjpeg passed on ${variant}."
