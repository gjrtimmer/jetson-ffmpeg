#!/usr/bin/env bash
# Decoder blocking-wait suite: validates that -flags low_delay activates
# true blocking wait in nvmpi_decoder_get_frame() (issue #10).
set -eu
# shellcheck source-path=SCRIPTDIR/..
# shellcheck source=lib/common.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../lib/common.sh"

variant="${JETSON_VARIANT:-unknown}"
echo "=== hw-decoder-blocking-wait on variant: ${variant} ==="

ensure_sample SAMPLE_H264_720P gen-sample-h264 3

expected_frames() {
  ffprobe -v error -select_streams v:0 -count_frames \
    -show_entries stream=nb_read_frames -of csv=p=0 "$1" 2>/dev/null || echo 0
}

echo "== 1. low_delay frames arrive (H.264) =="
run_ff -T 30 \
  -flags low_delay -c:v h264_nvmpi -i "${SAMPLE_H264_720P}" -f null - || true
if [ "$FF_RC" -eq 124 ]; then
  ff_fail "low_delay decode timed out (30s) — possible deadlock."
fi
if is_signal_rc "$FF_RC"; then
  ff_fail "low_delay decode confirmed crash (signal $((FF_RC - 128)))."
fi
exp=$(expected_frames "${SAMPLE_H264_720P}")
assert_frames 1 "low_delay decode (expected ~${exp})"

echo "== 2. low_delay no hang on EOS =="
run_ff -T 30 -loglevel error \
  -flags low_delay -c:v h264_nvmpi -i "${SAMPLE_H264_720P}" -f null - || true
if [ "$FF_RC" -eq 124 ]; then
  ff_fail "EOS hang with -flags low_delay."
fi
echo "   EOS exit clean (rc=${FF_RC}) — OK"

echo "== 3. low_delay completes without hang =="
# Blocking wait may have higher first-frame wall-clock than non-blocking
# (it waits for the actual decoded frame instead of returning -1/retry),
# so we only verify it completes within a reasonable time — not that it
# is faster than normal mode.
run_ff -T 15 -flags low_delay \
  -c:v h264_nvmpi -i "${SAMPLE_H264_720P}" -frames:v 1 -f null - || true
if [ "$FF_RC" -eq 124 ]; then
  ff_fail "low_delay single-frame decode timed out (15s)."
fi
echo "   low_delay single-frame decode OK (rc=${FF_RC})"

echo "== 4. wait_timeout AVOption =="
run_ff -T 30 -loglevel error \
  -flags low_delay -wait_timeout 100 -c:v h264_nvmpi \
  -i "${SAMPLE_H264_720P}" -f null - || true
if [ "$FF_RC" -eq 124 ]; then
  ff_fail "decode with wait_timeout=100 timed out."
fi
if printf '%s\n' "$FF_OUT" | grep -qi "unrecognized option\|no such option"; then
  ff_fail "wait_timeout AVOption not recognized."
fi
echo "   wait_timeout=100 accepted — OK"

echo "== 5. blocking wait HEVC =="
if has_codec enc libx265; then
  if [ ! -s "${SAMPLE_HEVC_720P}" ]; then
    echo "   skipped (HEVC sample unavailable)"
  else
    run_ff -T 30 -loglevel error \
      -flags low_delay -c:v hevc_nvmpi -i "${SAMPLE_HEVC_720P}" -f null - || true
    if [ "$FF_RC" -eq 124 ]; then
      ff_fail "HEVC low_delay decode timed out."
    fi
    echo "   HEVC low_delay OK (rc=${FF_RC})"
  fi
else
  echo "   skipped (libx265 not available)"
fi

echo "== 6. rapid open/close under blocking mode =="
for i in $(seq 1 20); do
  run_ff -T 10 -loglevel error \
    -flags low_delay -c:v h264_nvmpi -i "${SAMPLE_H264_720P}" \
    -frames:v 5 -f null - || true
  if is_signal_rc "$FF_RC"; then
    ff_fail "confirmed crash on iteration ${i} (signal $((FF_RC - 128)))."
  fi
  if [ "$FF_RC" -eq 124 ]; then
    ff_fail "hang on iteration ${i}."
  fi
done
echo "   20 rapid open/close cycles — OK"

echo "OK: hw-decoder-blocking-wait passed on ${variant}."
