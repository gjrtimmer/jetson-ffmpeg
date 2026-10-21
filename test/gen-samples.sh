# shellcheck shell=bash
# Input-sample paths and generators for the hw-* suites. Normally sourced via
# test/lib/common.sh (which also provides run_ff and friends); `gen-all`
# generates every sample once per job (called by hw-all.sh) so suites never
# regenerate shared inputs. Source, don't execute.
#
# Conventions:
#   - one specific sample path per case: /tmp/nvmpi-sample-{type}.mp4
#     (never reuse a generic name for a different kind of content)
#   - generators are named gen-sample-{name-or-type}

# shellcheck disable=SC2034  # consumed by the sourcing hw-* suites
SAMPLE_H264_720P="/tmp/nvmpi-sample-h264-720p.mp4"
# shellcheck disable=SC2034
SAMPLE_H264_OVERSIZED="/tmp/nvmpi-sample-h264-oversized-qp0.mp4"

# Short software-encoded H.264 sample (transcode/RTP-suite input).
#   gen-sample-h264 FILE [SECONDS]
gen-sample-h264() {
  ffmpeg -y -hide_banner -loglevel error \
    -f lavfi -i testsrc2=s=1280x720:r=30 -t "${2:-3}" \
    -c:v libx264 "$1"
}

# Oversized-packet H.264 sample: all-intra LOSSLESS (-qp 0) encode of noisy
# 1080p frames. Noise is incompressible, so every access unit is hundreds of
# KiB — far above any small chunk_size, for exercising the decoder input
# bounds check. (Plain testsrc2 compresses too well: even at 20 Mb/s
# all-intra its packets stay *under* 64 KiB.)
#   gen-sample-oversized FILE [SECONDS]
gen-sample-oversized() {
  ffmpeg -y -hide_banner -loglevel error \
    -f lavfi -i "testsrc2=s=1920x1080:r=30,noise=alls=40:allf=t+u" -t "${2:-1}" \
    -c:v libx264 -g 1 -qp 0 \
    "$1"
}

# shellcheck disable=SC2034  # consumed by hw-decoder-codecs.sh
SAMPLE_MPEG2_720P="/tmp/nvmpi-sample-mpeg2-720p.mkv"
# shellcheck disable=SC2034
SAMPLE_MPEG4_720P="/tmp/nvmpi-sample-mpeg4-720p.mkv"
# shellcheck disable=SC2034
SAMPLE_VP8_720P="/tmp/nvmpi-sample-vp8-720p.mkv"
# shellcheck disable=SC2034
SAMPLE_VP9_720P="/tmp/nvmpi-sample-vp9-720p.mkv"

# shellcheck disable=SC2034
SAMPLE_H264_SEEK="/tmp/nvmpi-sample-h264-seek.mp4"       # 5 s, g=30 (seek/flush/lifecycle)
# shellcheck disable=SC2034
SAMPLE_H264_SHORT="/tmp/nvmpi-sample-h264-short.mp4"     # 0.3 s (EOS drain)
# shellcheck disable=SC2034
SAMPLE_H264_720P_LONG="/tmp/nvmpi-sample-h264-720p-long.mp4"
# shellcheck disable=SC2034
SAMPLE_HEVC_720P="/tmp/nvmpi-sample-hevc-720p.mp4"
# shellcheck disable=SC2034  # consumed by hw-format-pixfmt.sh
SAMPLE_HEVC10_720P="/tmp/nvmpi-sample-hevc10-720p.mp4"

# Longer H.264 sample for benchmarking (enough frames to measure steady-state fps).
#   gen-sample-h264-long FILE [SECONDS]
gen-sample-h264-long() {
  ffmpeg -y -hide_banner -loglevel error \
    -f lavfi -i testsrc2=s=1280x720:r=30 -t "${2:-10}" \
    -c:v libx264 -preset fast -g 30 "$1"
}

# Short software-encoded HEVC sample.
#   gen-sample-hevc FILE [SECONDS]
gen-sample-hevc() {
  ffmpeg -y -hide_banner -loglevel error \
    -f lavfi -i testsrc2=s=1280x720:r=30 -t "${2:-3}" \
    -c:v libx265 -preset ultrafast "$1"
}

# 10-bit (Main10, yuv420p10le) software-encoded HEVC sample — input for the
# P010 hardware-decode path. NVDEC produces 10-bit output only from a genuinely
# 10-bit stream, so the sample must be encoded at 10 bpc.
#   gen-sample-hevc10 FILE [SECONDS]
gen-sample-hevc10() {
  ffmpeg -y -hide_banner -loglevel error \
    -f lavfi -i testsrc2=s=1280x720:r=30 -t "${2:-3}" \
    -c:v libx265 -preset ultrafast -pix_fmt yuv420p10le "$1"
}

# Short software-encoded MPEG-2 sample (B-frames disabled for exact frame count).
#   gen-sample-mpeg2 FILE [SECONDS]
gen-sample-mpeg2() {
  ffmpeg -y -hide_banner -loglevel error \
    -f lavfi -i testsrc2=s=1280x720:r=30 -t "${2:-3}" \
    -c:v mpeg2video -g 15 -bf 0 -b:v 5M "$1"
}

# Short software-encoded MPEG-4 Part 2 sample.
#   gen-sample-mpeg4 FILE [SECONDS]
gen-sample-mpeg4() {
  ffmpeg -y -hide_banner -loglevel error \
    -f lavfi -i testsrc2=s=1280x720:r=30 -t "${2:-3}" \
    -c:v mpeg4 -g 15 -bf 0 -b:v 5M "$1"
}

# Short software-encoded VP8 sample (requires --enable-libvpx).
#   gen-sample-vp8 FILE [SECONDS]
gen-sample-vp8() {
  ffmpeg -y -hide_banner -loglevel error \
    -f lavfi -i testsrc2=s=1280x720:r=30 -t "${2:-3}" \
    -c:v libvpx -b:v 3M "$1"
}

# Short software-encoded VP9 sample (requires --enable-libvpx).
#   gen-sample-vp9 FILE [SECONDS]
gen-sample-vp9() {
  ffmpeg -y -hide_banner -loglevel error \
    -f lavfi -i testsrc2=s=1280x720:r=30 -t "${2:-3}" \
    -c:v libvpx-vp9 -b:v 3M "$1"
}

# shellcheck disable=SC2034
SAMPLE_MJPEG_720P="/tmp/nvmpi-sample-mjpeg-720p.mkv"
# shellcheck disable=SC2034
SAMPLE_MJPEG_480P="/tmp/nvmpi-sample-mjpeg-480p.mkv"

# Baseline JPEG sequence in MKV container (hw-decodable).
#   gen-sample-mjpeg FILE [SECONDS]
gen-sample-mjpeg() {
  ffmpeg -y -hide_banner -loglevel error \
    -f lavfi -i testsrc2=s=1280x720:r=30 -t "${2:-3}" \
    -c:v mjpeg -q:v 5 "$1"
}

# Lower-resolution MJPEG sample for resolution-change testing.
#   gen-sample-mjpeg-480p FILE [SECONDS]
gen-sample-mjpeg-480p() {
  ffmpeg -y -hide_banner -loglevel error \
    -f lavfi -i testsrc2=s=854x480:r=30 -t "${2:-2}" \
    -c:v mjpeg -q:v 5 "$1"
}

# Baseline MJPEG sample with a chosen size and chroma format (drm_prime suite).
# PIX_FMT is a JPEG-range format: yuvj420p, yuvj422p or yuvj444p. Every frame
# is the same still picture, so frame pairing in PSNR comparisons cannot drift.
#   gen-sample-mjpeg-fmt FILE WxH PIX_FMT [SECONDS]
gen-sample-mjpeg-fmt() {
  local w="${2%x*}" h="${2#*x}" still="$1.still.png"
  ffmpeg -y -hide_banner -loglevel error \
    -f lavfi -i testsrc2=s=320x240 -frames:v 1 "$still" || return 1
  ffmpeg -y -hide_banner -loglevel error \
    -loop 1 -framerate 30 -i "$still" -t "${4:-1}" \
    -vf "scale=${w}:${h}" -c:v mjpeg -pix_fmt "$3" -q:v 3 "$1"
  local rc=$?
  rm -f "$still"
  return "$rc"
}

# 5 s all-keyframe-every-30 H.264 sample for seek tests.
gen-sample-h264-seek() {
  ffmpeg -y -hide_banner -loglevel error \
    -f lavfi -i testsrc2=s=1280x720:r=30 -t "${2:-5}" \
    -c:v libx264 -preset fast -g 30 "$1"
}

# 0.3 s H.264 sample: exercises EOS drain with minimal data.
gen-sample-h264-short() {
  ffmpeg -y -hide_banner -loglevel error \
    -f lavfi -i testsrc2=s=1280x720:r=30 -t "${2:-0.3}" \
    -c:v libx264 "$1"
}

# Raw Annex B H.264 of a STILL (frame 0 of testsrc2 repeated, no B-frames).
# Still content makes plane_psnr independent of frame pairing: the hw chain may
# shift, drop or duplicate a frame, and on moving content that alone costs
# ~25 dB.
#   gen-sample-h264-still FILE WxH SECONDS
gen-sample-h264-still() {
  ffmpeg -y -hide_banner -loglevel error \
    -f lavfi -i "testsrc2=s=$2:r=30" \
    -vf "select='eq(n\,0)',loop=loop=-1:size=1:start=0" -t "$3" -pix_fmt yuv420p \
    -c:v libx264 -g 15 -bf 0 -f h264 "$1"
}

# Mid-stream resolution change: 1 s 1280x720 still, then 1 s 642x358 still
# (new SPS at an IDR), concatenated as one Annex B stream. The parts are kept
# as FILE-a.h264 / FILE-b.h264 for per-part reference checks.
#   gen-sample-h264-reschange FILE
gen-sample-h264-reschange() {
  gen-sample-h264-still "$1-a.h264" 1280x720 1 &&
    gen-sample-h264-still "$1-b.h264" 642x358 1 &&
    cat "$1-a.h264" "$1-b.h264" > "$1"
}

# has_sw_encoder NAME: true when the software encoder is compiled into ffmpeg
# (needed to generate samples; the nvmpi codecs under test never fall back).
has_sw_encoder() {
  ffmpeg -hide_banner -encoders 2>/dev/null | awk '{print $2}' | grep -qx "$1"
}

# gen-all: generate every shared sample once (forced, so stale files from an
# earlier job on the same host never leak in). Samples needing an optional sw
# encoder (libx265, libvpx*) are skipped when it is missing; suites that use
# them already SKIP in that case. Prints one line per sample for the job log.
gen-all() {
  local t0 n=0
  t0=$(date +%s)
  _gen() { local fn="gen-$1"; shift; "$fn" "$@" && n=$((n + 1)) || echo "[W] ${fn} failed"; }
  _gen sample-h264 "${SAMPLE_H264_720P}" 3
  _gen sample-h264-long "${SAMPLE_H264_720P_LONG}" 10
  _gen sample-h264-seek "${SAMPLE_H264_SEEK}" 5
  _gen sample-h264-short "${SAMPLE_H264_SHORT}" 0.3
  _gen sample-oversized "${SAMPLE_H264_OVERSIZED}" 1
  _gen sample-mpeg2 "${SAMPLE_MPEG2_720P}" 3
  _gen sample-mpeg4 "${SAMPLE_MPEG4_720P}" 3
  _gen sample-mjpeg "${SAMPLE_MJPEG_720P}" 3
  _gen sample-mjpeg-480p "${SAMPLE_MJPEG_480P}" 2
  if has_sw_encoder libx265; then
    _gen sample-hevc "${SAMPLE_HEVC_720P}" 5
    _gen sample-hevc10 "${SAMPLE_HEVC10_720P}" 3
  fi
  has_sw_encoder libvpx && _gen sample-vp8 "${SAMPLE_VP8_720P}" 3
  has_sw_encoder libvpx-vp9 && _gen sample-vp9 "${SAMPLE_VP9_720P}" 3
  unset -f _gen
  echo "gen-all: ${n} samples in $(( $(date +%s) - t0 ))s"
}
