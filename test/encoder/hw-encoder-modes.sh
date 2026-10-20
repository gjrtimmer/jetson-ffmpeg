#!/usr/bin/env bash
# Encoder wait-mode suite: {blocking, nonblocking} x {default, low_delay}.
# Merges the former hw-encoder-blocking (issue #34, low_delay blocking dequeue
# in nvmpi_encoder_get_packet()) and hw-encoder-nonblock (-nonblocking 1:
# put_frame returns EAGAIN instead of blocking).
#
# Assertions (each is a hard failure):
#   1. Every mode produces exactly FRAMES packets (no drops, no hangs):
#      blocking/default, blocking/low_delay, nonblocking/default,
#      nonblocking/low_delay for H.264; blocking/low_delay + nonblocking for HEVC.
#      Back-pressure (jetson-nvmpi#125, jetson-ffmpeg#125): nonblocking never
#      drops an AU, also with a tiny -packet_pool_size (3), where send and
#      receive can both see EAGAIN (receive must then wait; no stall).
#   2. Custom -wait_timeout with low_delay works.
#   3. Early termination (-t 1) exits cleanly with low_delay and with
#      -nonblocking 1 (no hang, >0 packets).
# Output goes to mp4 (packet count read back with ffprobe), as the replaced
# suites did. mkv is NOT equivalent: the matroska muxer refuses the header
# ("Could not write header") for h264 with -flags low_delay on Orin NX, so
# mkv would turn this suite into an extradata test; hw-encoder-header owns that.
set -eu
# shellcheck source-path=SCRIPTDIR/..
# shellcheck source=lib/common.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../lib/common.sh"

variant="${JETSON_VARIANT:-unknown}"
echo "=== hw-encoder-modes on variant: ${variant} ==="

FRAMES=15
OUT=/tmp/nvmpi-out-modes.mp4
trap 'rm -f "$OUT"' EXIT

# encode_check ENC FLAGS EXTRA LABEL [SECS]: encode FRAMES frames, assert packet count.
encode_check() {
  local enc="$1" flags="$2" extra="$3" label="$4" secs="${5:-60}"
  local packets

  # shellcheck disable=SC2086
  run_ff_must "${label} encode" -T "$secs" \
    -f lavfi -i testsrc2=s=1280x720:r=30 \
    -frames:v ${FRAMES} -c:v "$enc" -b:v 3M ${flags} ${extra} \
    "$OUT"

  packets=$(packet_count "$OUT")
  if [ "${packets:-0}" -ne "$FRAMES" ]; then
    ff_fail "${label} — expected ${FRAMES} packets, got ${packets:-0}."
  fi
  echo "   ${label}: ${packets}/${FRAMES} packets OK"
}

# early_term FLAGS LABEL: -t 1 must exit cleanly and leave >0 packets.
early_term() {
  local flags="$1" label="$2" packets
  rm -f "$OUT"   # a stale file from an earlier call must not satisfy the check
  # shellcheck disable=SC2086
  run_ff -T 30 \
    -f lavfi -i testsrc2=s=1280x720:r=30 \
    -t 1 -c:v h264_nvmpi -b:v 3M ${flags} \
    "$OUT" || true
  packets=$(packet_count "$OUT")
  if [ "${packets:-0}" -gt 0 ]; then
    echo "   ${label} early-term: ${packets} packets, clean exit OK"
  else
    ff_fail "${label} early-term produced 0 packets or hung."
  fi
}

echo "== h264: blocking x {default, low_delay} =="
encode_check h264_nvmpi "" "" "h264 blocking/default"
encode_check h264_nvmpi "-flags low_delay" "" "h264 blocking/low_delay"

echo "== h264: nonblocking x {default, low_delay} =="
encode_check h264_nvmpi "-nonblocking 1" "" "h264 nonblocking/default"
encode_check h264_nvmpi "-nonblocking 1 -flags low_delay" "" "h264 nonblocking/low_delay"

echo "== h264: nonblocking + small packet_pool_size (back-pressure, exact count) =="
encode_check h264_nvmpi "-nonblocking 1" "-packet_pool_size 3" "h264 nonblocking/pool=3"
encode_check h264_nvmpi "-nonblocking 1 -flags low_delay" "-packet_pool_size 3" "h264 nonblocking/low_delay/pool=3"

echo "== hevc: blocking/low_delay, nonblocking =="
if is_mock; then
  # The mp4 muxer cannot take the mock's canned HEVC parameter sets.
  mock_skip "mp4 packet count needs muxable HEVC extradata"
else
  encode_check hevc_nvmpi "-flags low_delay" "" "hevc blocking/low_delay"
  encode_check hevc_nvmpi "-nonblocking 1" "" "hevc nonblocking/default"
fi

echo "== h264: low_delay + -wait_timeout 1000 =="
# low_delay blocks up to wait_timeout per frame while the encoder is idle
# (~1 s/frame, see jetson-nvmpi#103), so this call is legitimately slow but
# bounded: FRAMES s + margin.
encode_check h264_nvmpi "-flags low_delay" "-wait_timeout 1000" "h264 low_delay+timeout=1000" 120

echo "== early termination (-t 1) =="
early_term "-flags low_delay" "h264 low_delay"
early_term "-nonblocking 1" "h264 nonblocking"

echo "OK: hw-encoder-modes passed on ${variant}."
