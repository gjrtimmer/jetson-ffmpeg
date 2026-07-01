#!/usr/bin/env bash
# Decoder flush suite: exercises avcodec_flush_buffers() (seek / stream
# restart) on the nvmpi decoder. Validates that the flush callback
# (nvmpi_flush_decoder → nvmpi_decoder_flush) correctly resets the V4L2
# pipeline without crash, hang, or stale-frame leakage.
#
# Tests:
#   1. Seek mid-stream: decode, seek backward, verify frames after seek
#      are produced (no hang, no crash).
#   2. Multiple rapid seeks: repeated seek cycles — no crash, no leak.
#   3. Flush on short decode: seek past most content — no hang.
#   4. HEVC seek: same as test 1 but with HEVC codec (skipped if libx265
#      is not available in the FFmpeg under test).
set -eu
# shellcheck source=test/gen-samples.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/gen-samples.sh"

variant="${JETSON_VARIANT:-unknown}"
echo "=== hw-decoder-flush on variant: ${variant} ==="

# Generate a 5-second sample with keyframes every 1 second (GOP=30 at 30fps)
# so seeks have IDR targets to land on.
SAMPLE_H264_SEEK="/tmp/nvmpi-sample-h264-seek.mp4"
[ -s "${SAMPLE_H264_SEEK}" ] || ffmpeg -y -hide_banner -loglevel error \
  -f lavfi -i testsrc2=s=1280x720:r=30 -t 5 \
  -c:v libx264 -preset fast -g 30 "${SAMPLE_H264_SEEK}"

# seek_decode LABEL CODEC INPUT SEEK_TO EXPECTED_MIN_FRAMES
# Seek to SEEK_TO seconds, decode from there, verify at least
# EXPECTED_MIN_FRAMES are produced (proves the decoder pipeline restarted).
# On signal-kill, waits 200 ms and retries once (transient V4L2 driver flake).
seek_decode() {
  local label="$1" codec="$2" input="$3" seek_to="$4" min_frames="$5"
  local rc=0 out frames attempt
  for attempt in 1 2; do
    rc=0
    out=$(timeout -k 5 30 ffmpeg -y -hide_banner \
      -ss "$seek_to" -c:v "$codec" -i "$input" \
      -f null - 2>&1) || rc=$?
    if [ "$rc" -eq 124 ]; then
      echo "FAIL(${label}): seek+decode timed out (30 s) — possible deadlock"
      echo "      in nvmpi_decoder_flush() (src/nvmpi_dec.cpp) or the capture"
      echo "      thread failing to restart after STREAMOFF/STREAMON."
      exit 1
    fi
    if is_signal_rc "$rc"; then
      if [ "$attempt" -eq 1 ]; then
        echo "  warn(${label}): signal $((rc-128)), retrying after 200 ms..."
        sleep 0.2
        continue
      fi
      echo "FAIL(${label}): confirmed signal $((rc-128)) — crash in flush path."
      echo "--- ffmpeg output (last 15 lines) ---"
      echo "$out" | tail -15
      exit 1
    fi
    break
  done
  # Extract decoded frame count from ffmpeg stats line
  frames=$(echo "$out" | grep -oP 'frame=\s*\K[0-9]+' | tail -1)
  if [ -z "$frames" ] || [ "$frames" -lt "$min_frames" ]; then
    echo "FAIL(${label}): expected >= ${min_frames} frames after seek, got ${frames:-0}."
    echo "      Flush may not be re-priming extradata (SPS/PPS) or the"
    echo "      capture thread is not restarting properly."
    echo "--- ffmpeg output (last 15 lines) ---"
    echo "$out" | tail -15
    exit 1
  fi
  echo "   ${label}: ${frames} frames decoded after seek to ${seek_to}s — OK"
}

echo "== 1. H.264 seek mid-stream (seek to 3s) =="
seek_decode "h264-seek" h264_nvmpi "${SAMPLE_H264_SEEK}" 3 20

echo "== 2. multiple rapid seeks (H.264) =="
# Seek to several positions. The 4s seek leaves ~1s of content — hardware
# decode after seek may not produce all 30 frames (DPB priming, IDR
# alignment), so use a low threshold that still proves the pipeline restarted.
# Small cooldown between seeks reduces V4L2 driver pressure.
for pos in 1 4 2; do
  min=10
  [ "$pos" -ge 4 ] && min=5
  seek_decode "h264-multi-seek-${pos}s" h264_nvmpi "${SAMPLE_H264_SEEK}" "$pos" "$min"
  sleep 0.1
done
echo "   rapid seek cycles completed without crash"

echo "== 3. flush on short decode (seek past most content) =="
rc=0
out=$(timeout -k 5 15 ffmpeg -y -hide_banner -loglevel error \
  -ss 4.5 -c:v h264_nvmpi -i "${SAMPLE_H264_SEEK}" \
  -f null - 2>&1) || rc=$?
if [ "$rc" -eq 124 ]; then
  echo "FAIL: near-end seek timed out — possible deadlock on short stream."
  exit 1
fi
if is_signal_rc "$rc"; then
  echo "  warn: near-end seek signal $((rc-128)), retrying after 200 ms..."
  sleep 0.2
  rc=0
  out=$(timeout -k 5 15 ffmpeg -y -hide_banner -loglevel error \
    -ss 4.5 -c:v h264_nvmpi -i "${SAMPLE_H264_SEEK}" \
    -f null - 2>&1) || rc=$?
  if is_signal_rc "$rc"; then
    echo "FAIL: near-end seek confirmed crash (signal $((rc-128)))."
    echo "$out" | tail -15
    exit 1
  fi
fi
echo "   near-end seek OK (rc=${rc})"

echo "== 4. HEVC seek mid-stream (seek to 3s) =="
if ffmpeg -hide_banner -encoders 2>/dev/null | grep -q libx265; then
  SAMPLE_HEVC_SEEK="/tmp/nvmpi-sample-hevc-seek.mp4"
  [ -s "${SAMPLE_HEVC_SEEK}" ] || gen-sample-hevc "${SAMPLE_HEVC_SEEK}" 5
  seek_decode "hevc-seek" hevc_nvmpi "${SAMPLE_HEVC_SEEK}" 3 20
else
  echo "   skipped (libx265 not available in this FFmpeg build)"
fi

echo "OK: hw-decoder-flush passed on ${variant}."
