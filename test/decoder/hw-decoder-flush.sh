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
# shellcheck source-path=SCRIPTDIR/..
# shellcheck source=lib/common.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../lib/common.sh"

variant="${JETSON_VARIANT:-unknown}"
echo "=== hw-decoder-flush on variant: ${variant} ==="

# Shared 5 s sample with keyframes every 1 second (GOP=30 at 30fps) so seeks
# have IDR targets to land on.
ensure_sample SAMPLE_H264_SEEK gen-sample-h264-seek 5

# seek_decode LABEL CODEC INPUT SEEK_TO EXPECTED_MIN_FRAMES
# Seek to SEEK_TO seconds, decode from there, verify at least
# EXPECTED_MIN_FRAMES are produced (proves the decoder pipeline restarted).
seek_decode() {
  local label="$1" codec="$2" input="$3" seek_to="$4" min_frames="$5"
  run_ff -T 30 -ss "$seek_to" -c:v "$codec" -i "$input" -f null - || true
  if [ "$FF_RC" -eq 124 ]; then
    echo "      Possible deadlock in nvmpi_decoder_flush() (src/nvmpi_dec.cpp) or the"
    echo "      capture thread failing to restart after STREAMOFF/STREAMON."
    ff_fail "(${label}): seek+decode timed out (30 s)."
  fi
  if is_signal_rc "$FF_RC"; then
    ff_fail "(${label}): confirmed signal $((FF_RC - 128)) — crash in flush path."
  fi
  # Flush may not be re-priming extradata (SPS/PPS) or the capture thread is
  # not restarting properly when this fails.
  assert_frames "$min_frames" "${label} after seek to ${seek_to}s"
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
run_ff -T 15 -loglevel error \
  -ss 4.5 -c:v h264_nvmpi -i "${SAMPLE_H264_SEEK}" -f null - || true
if [ "$FF_RC" -eq 124 ]; then
  ff_fail "near-end seek timed out — possible deadlock on short stream."
fi
if is_signal_rc "$FF_RC"; then
  ff_fail "near-end seek confirmed crash (signal $((FF_RC - 128)))."
fi
echo "   near-end seek OK (rc=${FF_RC})"

echo "== 4. HEVC seek mid-stream (seek to 3s) =="
if ! has_codec enc libx265; then
  echo "   skipped (libx265 not available in this FFmpeg build)"
elif [ ! -s "${SAMPLE_HEVC_720P}" ]; then
  echo "   skipped (HEVC sample unavailable)"
else
  seek_decode "hevc-seek" hevc_nvmpi "${SAMPLE_HEVC_720P}" 3 20
fi

echo "OK: hw-decoder-flush passed on ${variant}."
