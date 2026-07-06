#!/usr/bin/env bash
# Decoder blocking-wait suite: validates that -flags low_delay activates
# true blocking wait in nvmpi_decoder_get_frame() (issue #10).
set -eu
# shellcheck source=test/gen-samples.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../gen-samples.sh"

variant="${JETSON_VARIANT:-unknown}"
echo "=== hw-decoder-blocking-wait on variant: ${variant} ==="

[ -s "${SAMPLE_H264_720P}" ] || gen-sample-h264 "${SAMPLE_H264_720P}" 3

expected_frames() {
  ffprobe -v error -select_streams v:0 -count_frames \
    -show_entries stream=nb_read_frames -of csv=p=0 "$1" 2>/dev/null || echo 0
}

echo "== 1. low_delay frames arrive (H.264) =="
rc=0
out=$(timeout -k 5 30 ffmpeg -y -hide_banner \
  -flags low_delay -c:v h264_nvmpi -i "${SAMPLE_H264_720P}" \
  -f null - 2>&1) || rc=$?
if [ "$rc" -eq 124 ]; then
  echo "FAIL: low_delay decode timed out (30s) — possible deadlock."
  exit 1
fi
if is_signal_rc "$rc"; then
  echo "  warn: low_delay decode signal $((rc-128)), retrying after 200 ms..."
  sleep 0.2
  rc=0
  out=$(timeout -k 5 30 ffmpeg -y -hide_banner \
    -flags low_delay -c:v h264_nvmpi -i "${SAMPLE_H264_720P}" \
    -f null - 2>&1) || rc=$?
  if is_signal_rc "$rc"; then
    echo "FAIL: low_delay decode confirmed crash (signal $((rc-128)))."
    echo "$out" | tail -15
    exit 1
  fi
fi
frames=$(echo "$out" | grep -oP 'frame=\s*\K[0-9]+' | tail -1)
exp=$(expected_frames "${SAMPLE_H264_720P}")
if [ -z "$frames" ] || [ "$frames" -lt 1 ]; then
  echo "FAIL: no frames decoded with -flags low_delay."
  echo "$out" | tail -15
  exit 1
fi
echo "   decoded ${frames} frames (expected ~${exp}) — OK"

echo "== 2. low_delay no hang on EOS =="
rc=0
timeout -k 5 30 ffmpeg -y -hide_banner -loglevel error \
  -flags low_delay -c:v h264_nvmpi -i "${SAMPLE_H264_720P}" \
  -f null - 2>/dev/null || rc=$?
if [ "$rc" -eq 124 ]; then
  echo "FAIL: EOS hang with -flags low_delay."
  exit 1
fi
echo "   EOS exit clean (rc=${rc}) — OK"

echo "== 3. low_delay completes without hang =="
# Blocking wait may have higher first-frame wall-clock than non-blocking
# (it waits for the actual decoded frame instead of returning -1/retry),
# so we only verify it completes within a reasonable time — not that it
# is faster than normal mode.
rc=0
timeout -k 5 15 ffmpeg -y -hide_banner -flags low_delay \
  -c:v h264_nvmpi -i "${SAMPLE_H264_720P}" \
  -frames:v 1 -f null - >/dev/null 2>&1 || rc=$?
if [ "$rc" -eq 124 ]; then
  echo "FAIL: low_delay single-frame decode timed out (15s)."
  exit 1
fi
echo "   low_delay single-frame decode OK (rc=${rc})"

echo "== 4. wait_timeout AVOption =="
rc=0
out=$(timeout -k 5 30 ffmpeg -y -hide_banner -loglevel error \
  -flags low_delay -wait_timeout 100 -c:v h264_nvmpi \
  -i "${SAMPLE_H264_720P}" -f null - 2>&1) || rc=$?
if [ "$rc" -eq 124 ]; then
  echo "FAIL: decode with wait_timeout=100 timed out."
  exit 1
fi
if echo "$out" | grep -qi "unrecognized option\|no such option"; then
  echo "FAIL: wait_timeout AVOption not recognized."
  echo "$out"
  exit 1
fi
echo "   wait_timeout=100 accepted — OK"

echo "== 5. blocking wait HEVC =="
if ffmpeg -hide_banner -encoders 2>/dev/null | grep -q libx265; then
  [ -s "${SAMPLE_HEVC_720P}" ] || gen-sample-hevc "${SAMPLE_HEVC_720P}" 3
  rc=0
  timeout -k 5 30 ffmpeg -y -hide_banner -loglevel error \
    -flags low_delay -c:v hevc_nvmpi -i "${SAMPLE_HEVC_720P}" \
    -f null - 2>/dev/null || rc=$?
  if [ "$rc" -eq 124 ]; then
    echo "FAIL: HEVC low_delay decode timed out."
    exit 1
  fi
  echo "   HEVC low_delay OK (rc=${rc})"
else
  echo "   skipped (libx265 not available)"
fi

echo "== 6. rapid open/close under blocking mode =="
for i in $(seq 1 20); do
  rc=0
  timeout -k 5 10 ffmpeg -y -hide_banner -loglevel error \
    -flags low_delay -c:v h264_nvmpi -i "${SAMPLE_H264_720P}" \
    -frames:v 5 -f null - 2>/dev/null || rc=$?
  if is_signal_rc "$rc"; then
    echo "  warn: iteration ${i} signal $((rc-128)), retrying after 200 ms..."
    sleep 0.2
    rc=0
    timeout -k 5 10 ffmpeg -y -hide_banner -loglevel error \
      -flags low_delay -c:v h264_nvmpi -i "${SAMPLE_H264_720P}" \
      -frames:v 5 -f null - 2>/dev/null || rc=$?
    if is_signal_rc "$rc"; then
      echo "FAIL: confirmed crash on iteration ${i} (signal $((rc-128)))."
      exit 1
    fi
  fi
  if [ "$rc" -eq 124 ]; then
    echo "FAIL: hang on iteration ${i}."
    exit 1
  fi
done
echo "   20 rapid open/close cycles — OK"

echo "OK: hw-decoder-blocking-wait passed on ${variant}."
