#!/usr/bin/env bash
# Decoder lifecycle suite: guards that blocking-wait changes did not break
# normal (non-low-delay) decode paths.
set -eu
# shellcheck source-path=SCRIPTDIR/..
# shellcheck source=lib/common.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../lib/common.sh"

variant="${JETSON_VARIANT:-unknown}"
echo "=== hw-decoder-lifecycle on variant: ${variant} ==="

ensure_sample SAMPLE_H264_720P gen-sample-h264 3
ensure_sample SAMPLE_H264_SEEK gen-sample-h264-seek 5
ensure_sample SAMPLE_H264_SHORT gen-sample-h264-short 0.3

echo "== 1. normal decode still works (no low_delay) =="
run_ff -T 30 -c:v h264_nvmpi -i "${SAMPLE_H264_720P}" -f null - || true
if [ "$FF_RC" -eq 124 ]; then
  ff_fail "normal decode timed out."
fi
if is_signal_rc "$FF_RC"; then
  ff_fail "normal decode confirmed crash (signal $((FF_RC - 128)))."
fi
assert_frames 10 "normal decode"

echo "== 2. flush and reuse (seek mid-stream) =="
run_ff -T 30 -ss 2 -c:v h264_nvmpi -i "${SAMPLE_H264_SEEK}" -f null - || true
if [ "$FF_RC" -eq 124 ]; then
  ff_fail "seek+decode timed out — flush may be broken."
fi
assert_frames 10 "flush+reuse after seek"

echo "== 3. EOS no hang (short file) =="
run_ff -T 15 -loglevel error -c:v h264_nvmpi -i "${SAMPLE_H264_SHORT}" -f null - || true
if [ "$FF_RC" -eq 124 ]; then
  ff_fail "short-file decode timed out — EOS not signalled."
fi
echo "   short-file EOS OK (rc=${FF_RC})"

echo "OK: hw-decoder-lifecycle passed on ${variant}."
