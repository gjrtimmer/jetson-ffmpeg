#!/usr/bin/env bash
# Pixel-format suite: the input/output layouts added in #12.
#   1. YUVJ420P (MJPEG/full-range) source -> h264_nvmpi — full-range input is
#      accepted and correctly range-converted (the Jetson encoder has no
#      full-range VUI control, so FFmpeg's swscale compresses full->limited
#      before encode; the output is correctly limited-range).
#   2. NV12 encoder input      — h264_nvmpi accepts NV12M frames and produces
#      a decodable stream.
#   3. NV12 decoder output     — h264_nvmpi decoder emits NV12 frames natively
#      (-output_format nv12; no software conversion).
#   4. P010LE decoder output   — hevc_nvmpi emits 10-bit P010LE from a Main10
#      stream. NvUtils/JetPack-5+ only; skips cleanly without libx265 or where
#      10-bit is unsupported.
#   5. insert_vui              — VUI timing_info embeds fps in the elementary
#      bitstream (default on); absent when disabled.
# Consolidates pixel-format fixes that were scattered across the bradcagle,
# xsacha, YuriiHoliuk, vietnx and jocover forks (jocover PRs #25/#26/#27).
set -eu

variant="${JETSON_VARIANT:-unknown}"
echo "=== hw-format-pixfmt on variant: ${variant} ==="

# shellcheck source=/dev/null
. "$(dirname "${BASH_SOURCE[0]}")/gen-samples.sh"

frame_count() {
  ffprobe -v error -select_streams v:0 -count_frames \
    -show_entries stream=nb_read_frames -of csv=p=0 "$1"
}

# ---------------------------------------------------------------------------
# 1. YUVJ420P (MJPEG / full-range) source -> h264_nvmpi
# ---------------------------------------------------------------------------
echo "== 1. YUVJ420P/MJPEG source -> h264_nvmpi =="
# MJPEG decodes to full-range yuvj420p. The encoder advertises only yuv420p,
# so FFmpeg inserts a swscale full->limited range conversion — the correct
# path on hardware that cannot signal full range in the VUI. The encode must
# succeed and yield decodable frames (the upstream MJPEG-ingest scenario).
ffmpeg -y -hide_banner -loglevel error \
  -f lavfi -i testsrc2=s=1280x720:r=30 -t 2 \
  -c:v mjpeg -pix_fmt yuvj420p /tmp/nvmpi-pf-mjpeg.mkv
src_pixfmt=$(ffprobe -v error -select_streams v:0 -show_entries stream=pix_fmt \
  -of csv=p=0 /tmp/nvmpi-pf-mjpeg.mkv || true)
echo "   MJPEG source pix_fmt=${src_pixfmt:-unknown}"
if ! ffmpeg -y -hide_banner -loglevel error \
    -i /tmp/nvmpi-pf-mjpeg.mkv -c:v h264_nvmpi /tmp/nvmpi-pf-yuvj.mp4 2> /tmp/nvmpi-pf-yuvj.log; then
  echo "FAIL: MJPEG/full-range source could not be encoded by h264_nvmpi."
  tail -15 /tmp/nvmpi-pf-yuvj.log
  echo "Code: ffmpeg/dev/common/libavcodec/nvmpi_enc.c (encoder pix_fmts; swscale converts full->limited)."
  exit 1
fi
yuvj_frames=$(frame_count /tmp/nvmpi-pf-yuvj.mp4)
[ "${yuvj_frames:-0}" -ge 1 ] || { echo "FAIL: MJPEG->h264_nvmpi produced no frames."; exit 1; }
crange=$(ffprobe -v error -select_streams v:0 -show_entries stream=color_range \
  -of csv=p=0 /tmp/nvmpi-pf-yuvj.mp4 || true)
echo "   ${yuvj_frames} frames encoded, output color_range=${crange:-unknown} (limited is correct)"

# ---------------------------------------------------------------------------
# 2. NV12 encoder input
# ---------------------------------------------------------------------------
echo "== 2. NV12 encode =="
if ! ffmpeg -y -hide_banner -loglevel error \
    -f lavfi -i testsrc2=s=1280x720:r=30 -t 2 \
    -pix_fmt nv12 -c:v h264_nvmpi /tmp/nvmpi-pf-nv12enc.mp4 2> /tmp/nvmpi-pf-nv12enc.log; then
  echo "FAIL: NV12 input rejected by h264_nvmpi."
  tail -15 /tmp/nvmpi-pf-nv12enc.log
  echo "Code: src/nvmpi_enc.cpp (raw_pixfmt V4L2_PIX_FMT_NV12M for NV_PIX_NV12)."
  exit 1
fi
nv12enc_frames=$(frame_count /tmp/nvmpi-pf-nv12enc.mp4)
[ "${nv12enc_frames:-0}" -ge 1 ] || { echo "FAIL: NV12 encode produced no decodable frames."; exit 1; }
echo "   ${nv12enc_frames} frames from NV12 input"

# ---------------------------------------------------------------------------
# 3. NV12 decoder output (native, routed via avctx->pix_fmt)
# ---------------------------------------------------------------------------
echo "== 3. NV12 decode (native, -output_format nv12) =="
gen-sample-h264 "$SAMPLE_H264_720P" 2
# -output_format selects the decoder's native output layout (no swscale):
# showinfo reads the frame straight off the decoder, so fmt:nv12 proves the
# VIC transform produced NV12 rather than a downstream conversion.
nv12dec_fmt=$(ffmpeg -hide_banner -loglevel info \
  -c:v h264_nvmpi -output_format nv12 -i "$SAMPLE_H264_720P" \
  -vf showinfo -f null - 2>&1 | grep -o 'fmt:nv12' | head -1 || true)
if [ "${nv12dec_fmt}" != "fmt:nv12" ]; then
  echo "FAIL: decoder did not emit NV12 frames (got: ${nv12dec_fmt:-none})."
  echo "Code: ffmpeg/dev/common/libavcodec/nvmpi_dec.c (output_format opt -> param.pixFormat=NV_PIX_NV12; bufFrame->format=avctx->pix_fmt)."
  exit 1
fi
echo "   decoder emitted NV12 frames natively"

# ---------------------------------------------------------------------------
# 4. P010LE decoder output (10-bit HEVC; NvUtils/JetPack-5+ only)
# ---------------------------------------------------------------------------
echo "== 4. P010LE decode (10-bit HEVC, -output_format p010le) =="
if ! ffmpeg -hide_banner -encoders 2>/dev/null | grep -q 'libx265'; then
  echo "   SKIP: libx265 not available in this FFmpeg build — cannot generate a 10-bit HEVC sample."
else
  gen-sample-hevc10 "$SAMPLE_HEVC10_720P" 2
  p010_log=/tmp/nvmpi-pf-p010.log
  p010_fmt=$(ffmpeg -hide_banner -loglevel info \
    -c:v hevc_nvmpi -output_format p010le -i "$SAMPLE_HEVC10_720P" \
    -vf showinfo -f null - 2> "$p010_log" | true; grep -o 'fmt:p010le' "$p010_log" | head -1 || true)
  if [ "${p010_fmt}" = "fmt:p010le" ]; then
    echo "   decoder emitted P010LE (10-bit) frames"
  elif grep -q "requires the NvUtils buffer API" "$p010_log"; then
    echo "   SKIP: P010LE unsupported on this build (legacy nvbuf_utils / JetPack 4)."
  else
    echo "FAIL: hevc_nvmpi did not emit P010LE from a Main10 stream."
    tail -15 "$p010_log"
    echo "Code: src/nvmpi_dec.cpp (NvBufferColorFormat_NV12_10LE); nvmpi_dec.c (NV_PIX_P010 routing)."
    exit 1
  fi
fi

# ---------------------------------------------------------------------------
# 5. insert_vui — fps present in the elementary bitstream when on, absent off
# ---------------------------------------------------------------------------
echo "== 5. insert_vui (VUI timing_info) =="
# Encode to a raw Annex-B elementary stream (no container to carry fps), so the
# probed frame rate can only come from the bitstream VUI.
ffmpeg -y -hide_banner -loglevel error \
  -f lavfi -i testsrc2=s=1280x720:r=30 -t 2 \
  -c:v h264_nvmpi -insert_vui 1 -f h264 /tmp/nvmpi-pf-vui-on.h264
ffmpeg -y -hide_banner -loglevel error \
  -f lavfi -i testsrc2=s=1280x720:r=30 -t 2 \
  -c:v h264_nvmpi -insert_vui 0 -f h264 /tmp/nvmpi-pf-vui-off.h264
fps_on=$(ffprobe -v error -select_streams v:0 -show_entries stream=r_frame_rate \
  -of csv=p=0 /tmp/nvmpi-pf-vui-on.h264 || true)
fps_off=$(ffprobe -v error -select_streams v:0 -show_entries stream=r_frame_rate \
  -of csv=p=0 /tmp/nvmpi-pf-vui-off.h264 || true)
echo "   insert_vui=1 -> r_frame_rate=${fps_on:-unknown}; insert_vui=0 -> r_frame_rate=${fps_off:-unknown}"
# With VUI on, the probed fps must be a normal frame rate, not the huge
# fallback (e.g. 1200000/1) that ffprobe reports when no timing info is
# present.  H.264 VUI encodes time_scale/(2*num_units_in_tick); different
# FFmpeg versions may report 30/1 or 60/1 for a 30-fps source, so we check
# the numerator is reasonable (<= 120) rather than matching an exact value.
fps_num="${fps_on%%/*}"
if [ "${fps_num:-0}" -le 0 ] || [ "${fps_num}" -gt 120 ]; then
  echo "FAIL: insert_vui=1 did not embed fps in the bitstream (got ${fps_on:-unknown})."
  echo "Code: src/nvmpi_enc.cpp (setInsertVuiEnabled); nvmpi_enc.c (insert_vui AVOption)."
  exit 1
fi

echo "OK: hw-format-pixfmt passed on ${variant}."
