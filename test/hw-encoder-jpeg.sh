#!/usr/bin/env bash
# MJPEG hardware encoder test suite: exercises the mjpeg_nvmpi encoder path
# which uses NvJPEGEncoder::encodeFromFd (synchronous per-frame encode) rather
# than the V4L2 M2M pipeline used by H.264/HEVC encoders. Tests: basic encode,
# quality mapping, frame count accuracy, PSNR correctness vs software encode,
# multi-stream concurrency, and resolution handling.
set -eu

variant="${JETSON_VARIANT:-unknown}"
echo "=== hw-encoder-jpeg on variant: ${variant} ==="

# input_frames FILE — count video packets in a container
input_frames() {
  ffprobe -v error -count_packets -select_streams v:0 \
    -show_entries stream=nb_read_packets -of default=nokey=1:nw=1 "$1"
}

# file_size FILE — bytes
file_size() {
  stat -c%s "$1"
}

# ── 1. Basic encode ──────────────────────────────────────────────────────────
echo "== 1. Basic MJPEG encode (mjpeg_nvmpi) =="
out=$(ffmpeg -y -hide_banner -loglevel error \
  -f lavfi -i testsrc2=s=1280x720:r=30 -t 2 \
  -c:v mjpeg_nvmpi -q:v 5 /tmp/nvmpi-enc-mjpeg-basic.mkv 2>&1) || {
  echo "FAIL(basic-encode): mjpeg_nvmpi encode failed."
  echo "--- ffmpeg output ---"
  echo "$out" | tail -15
  exit 1
}

actual=$(input_frames /tmp/nvmpi-enc-mjpeg-basic.mkv)
if [ "${actual:-0}" -lt 50 ]; then
  echo "FAIL(basic-encode): only ${actual} frames in output (expected ~60)."
  exit 1
fi
echo "   basic-encode: OK (${actual} frames)"

# ── 2. Quality mapping ──────────────────────────────────────────────────────
# Lower -q:v = higher quality = larger file. Verify q:v 2 > q:v 20 in size.
echo "== 2. Quality mapping (q:v 2 vs q:v 20) =="
ffmpeg -y -hide_banner -loglevel error \
  -f lavfi -i testsrc2=s=1280x720:r=30 -t 1 \
  -c:v mjpeg_nvmpi -q:v 2 /tmp/nvmpi-enc-mjpeg-q2.mkv 2>/dev/null || {
  echo "FAIL(quality): q:v 2 encode failed."
  exit 1
}
ffmpeg -y -hide_banner -loglevel error \
  -f lavfi -i testsrc2=s=1280x720:r=30 -t 1 \
  -c:v mjpeg_nvmpi -q:v 20 /tmp/nvmpi-enc-mjpeg-q20.mkv 2>/dev/null || {
  echo "FAIL(quality): q:v 20 encode failed."
  exit 1
}

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
ffmpeg -y -hide_banner -loglevel error \
  -f lavfi -i testsrc2=s=1280x720:r=30 -t 2 \
  -c:v mjpeg_nvmpi -q:v 5 /tmp/nvmpi-enc-mjpeg-fc.mkv 2>/dev/null || {
  echo "FAIL(frame-count): encode failed."
  exit 1
}

actual=$(input_frames /tmp/nvmpi-enc-mjpeg-fc.mkv)
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
ffmpeg -y -hide_banner -loglevel error \
  -f lavfi -i testsrc2=s=1280x720:r=30 -t 1 \
  -pix_fmt yuv420p -f rawvideo "$orig_raw" 2>/dev/null || {
  echo "FAIL(psnr): raw reference generation failed."
  exit 1
}

# Encode with hw, decode back to raw
ffmpeg -y -hide_banner -loglevel error \
  -f lavfi -i testsrc2=s=1280x720:r=30 -t 1 \
  -c:v mjpeg_nvmpi -q:v 2 /tmp/nvmpi-enc-mjpeg-psnr.mkv 2>/dev/null || {
  echo "FAIL(psnr): hw encode failed."
  exit 1
}
ffmpeg -y -hide_banner -loglevel error \
  -i /tmp/nvmpi-enc-mjpeg-psnr.mkv \
  -pix_fmt yuv420p -f rawvideo "$rt_raw" 2>/dev/null || {
  echo "FAIL(psnr): decode of hw-encoded output failed."
  exit 1
}

# Verify sizes match
orig_size=$(file_size "$orig_raw")
rt_size=$(file_size "$rt_raw")
if [ "$orig_size" -ne "$rt_size" ]; then
  echo "FAIL(psnr): raw size mismatch: orig=${orig_size} rt=${rt_size}."
  exit 1
fi

# PSNR comparison
psnr_log="/tmp/nvmpi-enc-mjpeg-psnr.log"
ffmpeg -y -hide_banner -loglevel error \
  -f rawvideo -pix_fmt yuv420p -s 1280x720 -i "$orig_raw" \
  -f rawvideo -pix_fmt yuv420p -s 1280x720 -i "$rt_raw" \
  -lavfi "psnr=stats_file=${psnr_log}" -f null - 2>/dev/null || {
  echo "FAIL(psnr): psnr filter failed."
  exit 1
}

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
  timeout -k 5 60 ffmpeg -y -hide_banner -loglevel error \
    -f lavfi -i testsrc2=s=1280x720:r=30 -t 1 \
    -c:v mjpeg_nvmpi -q:v 5 "/tmp/nvmpi-enc-mjpeg-para-${i}.mkv" 2>/dev/null &
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
  out=$(ffmpeg -y -hide_banner -loglevel error \
    -f lavfi -i "testsrc2=s=${res}:r=30" -t 1 \
    -c:v mjpeg_nvmpi -q:v 5 "/tmp/nvmpi-enc-mjpeg-res-${tag}.mkv" 2>&1) || {
    echo "FAIL(resolution): encode at ${res} failed."
    echo "--- ffmpeg output ---"
    echo "$out" | tail -15
    exit 1
  }
  fc=$(input_frames "/tmp/nvmpi-enc-mjpeg-res-${tag}.mkv")
  echo "   ${res}: ${fc} frames"
done
echo "   resolution: all sizes OK"

# ── Cleanup ──────────────────────────────────────────────────────────────────
rm -f "$orig_raw" "$rt_raw" "$psnr_log" \
      /tmp/nvmpi-enc-mjpeg-*.mkv /tmp/nvmpi-enc-mjpeg-*.yuv 2>/dev/null || true

echo "OK: hw-encoder-jpeg passed on ${variant}."
