# shellcheck shell=bash
# Input-sample paths and generators for the hw-* test suites.
# Source, don't execute:
#   . "$(dirname "${BASH_SOURCE[0]}")/gen-samples.sh"
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
    -c:v libx265 "$1"
}

# 10-bit (Main10, yuv420p10le) software-encoded HEVC sample — input for the
# P010 hardware-decode path. NVDEC produces 10-bit output only from a genuinely
# 10-bit stream, so the sample must be encoded at 10 bpc.
#   gen-sample-hevc10 FILE [SECONDS]
gen-sample-hevc10() {
  ffmpeg -y -hide_banner -loglevel error \
    -f lavfi -i testsrc2=s=1280x720:r=30 -t "${2:-3}" \
    -c:v libx265 -pix_fmt yuv420p10le "$1"
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
