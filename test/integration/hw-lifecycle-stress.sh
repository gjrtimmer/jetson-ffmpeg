#!/usr/bin/env bash
# Lifecycle stress suite: decoder/encoder open-close guards on real hardware.
# Replaces hw-encoder-lifecycle, hw-decoder-teardown and hw-pthread-guard
# (200+200+50, 200+20, 100+100 iterations) with ~10 iterations per variant.
# CLAUDE.md forbids rapid V4L2 open/close cycling: the Tegra driver segfaults
# stochastically, so volume adds noise, not coverage. Every driver-pitfall
# guard keeps at least one hardware assertion:
#
#   1. Decoder full open/close      close drains planes before DMA destroy (#8)
#   2. Decoder close-during-decode  early teardown with capture thread live (#8)
#   3. Decoder error-path close     truncated stream, isInError() exit path (#8)
#   4. Decoder pthread guard        glibc>=2.34 pthread_join on unstarted DQ
#                                   thread (#6, Keylost#21): -frames:v 1
#   5. Encoder full open/close      pkt pool drain + DQ thread stop (#27)
#   6. Encoder pthread guard        encoder torn down before DQ thread runs (#6)
#   7. Encoder close-mid-encode     kill before EOS; a close hang = rc 137 (#27)
#   8. Encoder short-input EOS      drain with a nearly empty packet pool (#27)
#   9. Device health                decoder and encoder usable after the stress
#
# Code under test: nvmpi_decoder_close (src/nvmpi_dec*.cpp),
# nvmpi_encode_close (ffmpeg/dev/common/libavcodec/nvmpi_enc.c),
# nvmpi_encoder_close (src/nvmpi_enc*.cpp),
# vendor/mmapi/NvV4l2ElementPlane.cpp (pthread_join guard).
# A signal-kill is retried once by run_ff; a repeat is a deterministic bug.

set -eu
# shellcheck source-path=SCRIPTDIR/..
# shellcheck source=lib/common.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../lib/common.sh"

variant="${JETSON_VARIANT:-unknown}"
echo "=== hw-lifecycle-stress on variant: ${variant} ==="

ITER=10
MID_ITER=5

ensure_sample SAMPLE_H264_720P gen-sample-h264 3
ensure_sample SAMPLE_H264_720P_LONG gen-sample-h264-long 10
ensure_sample SAMPLE_H264_SHORT gen-sample-h264-short 0.3

# loop LABEL ARGS... : ITER runs of run_ff_must, no sleeps.
loop() {
  local label="$1" i; shift
  for i in $(seq 1 "$ITER"); do
    run_ff_must "${label} iter ${i}/${ITER}" -T 30 "$@"
  done
  echo "   ${label}: ${ITER} iterations - OK"
}

echo "== 1. decoder full open/close x${ITER} =="
# On failure: nvmpi_decoder_close must drain V4L2 planes before DMA destroy.
loop "decoder full close" -c:v h264_nvmpi -i "$SAMPLE_H264_720P" -frames:v 10 -f null -

echo "== 2. decoder close-during-decode x${ITER} =="
# On failure: early close while the capture thread still holds V4L2 buffers.
loop "decoder early close" -c:v h264_nvmpi -i "$SAMPLE_H264_720P" -frames:v 1 -f null -

echo "== 3. decoder error-path close x${ITER} (truncated stream) =="
TRUNCATED="$(mktemp "${TMPDIR:-/tmp}/nvmpi-truncated.XXXXXX.mp4")"
trap 'rm -f "$TRUNCATED"' EXIT
# First 8 KB: decoder initializes, then hits premature EOF mid-frame.
dd if="$SAMPLE_H264_720P" of="$TRUNCATED" bs=1024 count=8 2>/dev/null
for i in $(seq 1 "$ITER"); do
  run_ff -T 30 -c:v h264_nvmpi -i "$TRUNCATED" -f null - || true
  case "$FF_RC" in
    124|137) ff_fail "error-path close iter ${i}/${ITER} hung." ;;
  esac
  # On failure: truncated stream must error cleanly (isInError() exit path in dec_capture_loop_fcn).
  is_signal_rc "$FF_RC" && ff_fail "error-path close iter ${i}/${ITER} crashed (signal $((FF_RC - 128)))."
done
echo "   decoder error-path close: ${ITER} iterations - OK"

echo "== 4. decoder pthread_join guard x${ITER} =="
# DQ thread may never start before teardown; unguarded pthread_join segfaults on glibc>=2.34.
loop "decoder pthread guard" -c:v h264_nvmpi -i "$SAMPLE_H264_720P" -frames:v 1 -f null -

echo "== 5. encoder full open/close x${ITER} =="
# On failure: nvmpi_encode_close (pool drain) / nvmpi_encoder_close (DQ stop).
loop "encoder full close" -i "$SAMPLE_H264_720P" -c:v h264_nvmpi -frames:v 10 -f null -

echo "== 6. encoder pthread_join guard x${ITER} =="
loop "encoder pthread guard" -f lavfi -i "color=c=black:s=320x240:d=0.04:r=25" \
  -c:v h264_nvmpi -frames:v 1 -f null -

echo "== 7. encoder close-mid-encode x${MID_ITER} =="
for i in $(seq 1 "$MID_ITER"); do
  # SIGTERM before EOS (timeout rc 124 is the expected outcome). rc 137 means
  # the close path hung until timeout -k SIGKILLed it; a signal death is a crash.
  rc=0
  timeout --foreground -k 5 0.5 ffmpeg -y -hide_banner -i "$SAMPLE_H264_720P_LONG" \
    -c:v h264_nvmpi -f null - >/dev/null 2>&1 || rc=$?
  if [ "$rc" -eq 137 ]; then
    echo "FAIL: close-mid-encode iter ${i}/${MID_ITER}: close hung (needed SIGKILL)."
    exit 1
  fi
  if is_signal_rc "$rc"; then
    echo "FAIL: close-mid-encode iter ${i}/${MID_ITER}: crashed (signal $((rc - 128)))."
    exit 1
  fi
done
echo "   close-mid-encode: ${MID_ITER} iterations - OK"

echo "== 8. encoder short-input EOS =="
run_ff_must "short encode EOS" -T 15 -i "$SAMPLE_H264_SHORT" -c:v h264_nvmpi -f null -
echo "   short encode EOS OK"

echo "== 9. device health =="
device_health "decoder after lifecycle stress"
run_ff_must "encoder health" -T 30 -i "$SAMPLE_H264_720P" -c:v h264_nvmpi -frames:v 10 -f null -
assert_frames 10 "encoder after lifecycle stress"

echo "OK: hw-lifecycle-stress passed on ${variant}."
