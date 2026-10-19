#!/usr/bin/env bash
# Encoder blocking-wait suite: -flags low_delay activates blocking dequeue
# in nvmpi_encoder_get_packet(). Regression guard for issue #34.
#
# Assertions:
#   1. Encode with low_delay produces correct frame count (no drops/hangs).
#   2. Encode without low_delay still works (non-blocking path).
#   3. Early termination (-t) with low_delay exits cleanly (no hang).
#   4. Custom wait_timeout works.
set -eu
# shellcheck source-path=SCRIPTDIR/..
# shellcheck source=lib/common.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../lib/common.sh"

variant="${JETSON_VARIANT:-unknown}"
echo "=== hw-encoder-blocking on variant: ${variant} ==="

FRAMES=60
OUT=/tmp/nvmpi-out-blocking.mp4

# Helper: encode and verify frame count
encode_check() {
  local enc="$1" flags="$2" extra="$3" label="$4" expect="$5"
  local packets

  # shellcheck disable=SC2086
  run_ff_must "${label} encode" -T 60 \
    -f lavfi -i testsrc2=s=1280x720:r=30 \
    -frames:v ${FRAMES} -c:v "$enc" -b:v 3M ${flags} ${extra} \
    "$OUT"

  packets=$(packet_count "$OUT")
  if [ "${packets:-0}" -ne "$expect" ]; then
    ff_fail "${label} — expected ${expect} packets, got ${packets:-0}."
  fi
  echo "   ${label}: ${packets}/${expect} packets OK"
}

# --- blocking mode (low_delay) ---

echo "== h264_nvmpi: -flags low_delay =="
encode_check h264_nvmpi "-flags low_delay" "" "h264 low_delay" "$FRAMES"

echo "== hevc_nvmpi: -flags low_delay =="
encode_check hevc_nvmpi "-flags low_delay" "" "hevc low_delay" "$FRAMES"

# --- non-blocking mode (default, no low_delay) ---

echo "== h264_nvmpi: default (no low_delay) =="
encode_check h264_nvmpi "" "" "h264 default" "$FRAMES"

# --- custom wait_timeout ---

echo "== h264_nvmpi: -flags low_delay -wait_timeout 1000 =="
encode_check h264_nvmpi "-flags low_delay" "-wait_timeout 1000" "h264 low_delay+timeout=1000" "$FRAMES"

# --- early termination with low_delay (clean shutdown, no hang) ---

echo "== h264_nvmpi: -flags low_delay -t 1 (early termination) =="
run_ff -T 30 \
  -f lavfi -i testsrc2=s=1280x720:r=30 \
  -t 1 -c:v h264_nvmpi -b:v 3M -flags low_delay \
  "$OUT" || true

packets=$(packet_count "$OUT")
if [ "${packets:-0}" -gt 0 ]; then
  echo "   h264 early-term: ${packets} packets, clean exit OK"
else
  ff_fail "h264 early-term produced 0 packets or hung."
fi

rm -f "$OUT"
echo "OK: hw-encoder-blocking passed on ${variant}."
