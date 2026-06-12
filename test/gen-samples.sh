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
