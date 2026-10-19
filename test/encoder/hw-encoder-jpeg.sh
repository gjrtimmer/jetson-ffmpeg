#!/usr/bin/env bash
# MJPEG hardware encoder test suite: exercises the mjpeg_nvmpi encoder path
# which uses NvJPEGEncoder::encodeFromFd (synchronous per-frame encode) rather
# than the V4L2 M2M pipeline used by H.264/HEVC encoders. Tests: basic encode,
# quality mapping, frame count accuracy, PSNR correctness vs software encode,
# multi-stream concurrency, and resolution handling.
set -eu
# shellcheck source-path=SCRIPTDIR/..
# shellcheck source=lib/common.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../lib/common.sh"

variant="${JETSON_VARIANT:-unknown}"
echo "=== hw-encoder-jpeg on variant: ${variant} ==="

# file_size FILE — bytes
file_size() {
  stat -c%s "$1"
}

# ── 1. Basic encode ──────────────────────────────────────────────────────────
echo "== 1. Basic MJPEG encode (mjpeg_nvmpi) =="
run_ff_must "basic-encode: mjpeg_nvmpi" -T 60 -loglevel error \
  -f lavfi -i testsrc2=s=1280x720:r=30 -t 2 \
  -c:v mjpeg_nvmpi -q:v 5 /tmp/nvmpi-enc-mjpeg-basic.mkv

actual=$(packet_count /tmp/nvmpi-enc-mjpeg-basic.mkv)
if [ "${actual:-0}" -lt 50 ]; then
  ff_fail "basic-encode: only ${actual} frames in output (expected ~60)."
fi
echo "   basic-encode: OK (${actual} frames)"

# ── 2. Quality mapping ──────────────────────────────────────────────────────
# Lower -q:v = higher quality = larger file. Verify q:v 2 > q:v 20 in size.
echo "== 2. Quality mapping (q:v 2 vs q:v 20) =="
run_ff_must "quality: q:v 2 encode" -T 60 -loglevel error \
  -f lavfi -i testsrc2=s=1280x720:r=30 -t 1 \
  -c:v mjpeg_nvmpi -q:v 2 /tmp/nvmpi-enc-mjpeg-q2.mkv
run_ff_must "quality: q:v 20 encode" -T 60 -loglevel error \
  -f lavfi -i testsrc2=s=1280x720:r=30 -t 1 \
  -c:v mjpeg_nvmpi -q:v 20 /tmp/nvmpi-enc-mjpeg-q20.mkv

size_q2=$(file_size /tmp/nvmpi-enc-mjpeg-q2.mkv)
size_q20=$(file_size /tmp/nvmpi-enc-mjpeg-q20.mkv)
if [ "$size_q2" -le "$size_q20" ]; then
  echo "FAIL(quality): q:v 2 (${size_q2}B) should be larger than q:v 20 (${size_q20}B)."
  exit 1
fi
echo "   quality: q:v 2 = ${size_q2}B > q:v 20 = ${size_q20}B"

# ── 3. Frame count accuracy ─────────────────────────────────────────────────
# Synchronous encoder: every input frame should produce exactly one output packet.
echo "== 3. Frame count accuracy (60 frames in) =="
run_ff_must "frame-count: encode" -T 60 -loglevel error \
  -f lavfi -i testsrc2=s=1280x720:r=30 -t 2 \
  -c:v mjpeg_nvmpi -q:v 5 /tmp/nvmpi-enc-mjpeg-fc.mkv

actual=$(packet_count /tmp/nvmpi-enc-mjpeg-fc.mkv)
# testsrc2 at 30fps for 2s = 60 frames. Synchronous encoder should emit all.
if [ "${actual:-0}" -lt 59 ] || [ "${actual:-0}" -gt 61 ]; then
  echo "FAIL(frame-count): ${actual} frames (expected 59-61)."
  exit 1
fi
echo "   frame-count: ${actual}/60 frames encoded"

# ── 4. PSNR correctness (hw encode → sw decode vs original) ────────────────
# Encode with hw, decode back to raw, compare against original raw.
echo "== 4. PSNR correctness (encode roundtrip) =="
orig_raw="/tmp/nvmpi-enc-mjpeg-orig.yuv"
rt_raw="/tmp/nvmpi-enc-mjpeg-rt.yuv"

# Generate raw reference
run_ff_must "psnr: raw reference generation" -T 60 -loglevel error \
  -f lavfi -i testsrc2=s=1280x720:r=30 -t 1 \
  -pix_fmt yuv420p -f rawvideo "$orig_raw"

# Encode with hw, decode back to raw
run_ff_must "psnr: hw encode" -T 60 -loglevel error \
  -f lavfi -i testsrc2=s=1280x720:r=30 -t 1 \
  -c:v mjpeg_nvmpi -q:v 2 /tmp/nvmpi-enc-mjpeg-psnr.mkv
run_ff_must "psnr: decode of hw-encoded output" -T 60 -loglevel error \
  -i /tmp/nvmpi-enc-mjpeg-psnr.mkv \
  -pix_fmt yuv420p -f rawvideo "$rt_raw"

# Verify sizes match
orig_size=$(file_size "$orig_raw")
rt_size=$(file_size "$rt_raw")
if [ "$orig_size" -ne "$rt_size" ]; then
  echo "FAIL(psnr): raw size mismatch: orig=${orig_size} rt=${rt_size}."
  exit 1
fi

# PSNR comparison
psnr_log="/tmp/nvmpi-enc-mjpeg-psnr.log"
run_ff_must "psnr: psnr filter" -T 60 -loglevel error \
  -f rawvideo -pix_fmt yuv420p -s 1280x720 -i "$orig_raw" \
  -f rawvideo -pix_fmt yuv420p -s 1280x720 -i "$rt_raw" \
  -lavfi "psnr=stats_file=${psnr_log}" -f null -

avg_psnr=$(tail -1 "$psnr_log" | grep -oP 'psnr_avg:\K[0-9.]+' || echo "0")
# JPEG is lossy; at q:v 2 (high quality) we expect >30 dB. Below 25 dB
# indicates encode corruption or wrong pixel format.
psnr_int=${avg_psnr%%.*}
if [ "${psnr_int:-0}" -lt 25 ]; then
  echo "FAIL(psnr): avg PSNR ${avg_psnr} dB < 25 dB threshold."
  exit 1
fi
echo "   psnr: avg ${avg_psnr} dB (>= 25 dB threshold)"

# ── 5. Multi-stream concurrency ─────────────────────────────────────────────
echo "== 5. Multi-stream concurrency (3 parallel encodes) =="
pids=()
rc_all=0

for i in 1 2 3; do
  ( run_ff -T 60 -loglevel error \
      -f lavfi -i testsrc2=s=1280x720:r=30 -t 1 \
      -c:v mjpeg_nvmpi -q:v 5 "/tmp/nvmpi-enc-mjpeg-para-${i}.mkv" ) &
  pids+=($!)
done

for pid in "${pids[@]}"; do
  wait "$pid" || rc_all=$((rc_all + 1))
done

if [ "$rc_all" -gt 0 ]; then
  echo "FAIL(multi-stream): ${rc_all}/3 concurrent encode sessions failed."
  exit 1
fi
echo "   multi-stream: 3/3 concurrent sessions OK"

# ── 6. Multiple resolutions ─────────────────────────────────────────────────
# Verify encoder handles different input resolutions (DMA buffer realloc).
echo "== 6. Multiple resolutions (720p + 480p + 1080p) =="
for res in 1280x720 854x480 1920x1080; do
  tag="${res//x/-}"
  run_ff_must "resolution: encode at ${res}" -T 60 -loglevel error \
    -f lavfi -i "testsrc2=s=${res}:r=30" -t 1 \
    -c:v mjpeg_nvmpi -q:v 5 "/tmp/nvmpi-enc-mjpeg-res-${tag}.mkv"
  fc=$(packet_count "/tmp/nvmpi-enc-mjpeg-res-${tag}.mkv")
  echo "   ${res}: ${fc} frames"
done
echo "   resolution: all sizes OK"

# ── Cleanup ──────────────────────────────────────────────────────────────────
rm -f "$orig_raw" "$rt_raw" "$psnr_log" \
      /tmp/nvmpi-enc-mjpeg-*.mkv /tmp/nvmpi-enc-mjpeg-*.yuv 2>/dev/null || true

echo "OK: hw-encoder-jpeg passed on ${variant}."
