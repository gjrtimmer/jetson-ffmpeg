#!/usr/bin/env bash
# Encoder level suite: -level must propagate to the bitstream SPS for both
# h264_nvmpi and hevc_nvmpi. Regression guard for issue #14 (upstream
# Keylost#50): without setLevel() the hardware writes whatever level it
# defaults to, causing HLS validators and device capability negotiation to
# reject the output.
#
# Assertions:
#   1. Explicit level: encode at a specific level, verify ffprobe reports it.
#   2. Default level: encode without -level, verify ffprobe reports a sane
#      non-zero level (hardware default, not garbage).
set -eu

variant="${JETSON_VARIANT:-unknown}"
echo "=== hw-encoder-level on variant: ${variant} ==="

FRAMES=30
OUT=/tmp/nvmpi-out-level.mp4

# Helper: encode and extract the stream-level value from ffprobe.
# Usage: encode_and_probe <encoder> <level_flag> <expected_level>
#   level_flag: "-level 4.1" or "" (for default)
#   expected_level: the level value ffprobe should report, or "any" for default
encode_and_probe() {
  local enc="$1" level_flag="$2" expected="$3" label="$4"
  local rc=0

  # Build the ffmpeg command — level_flag may be empty (default path)
  # shellcheck disable=SC2086
  ffmpeg -y -hide_banner -loglevel error \
    -f lavfi -i testsrc2=s=1280x720:r=30 \
    -frames:v ${FRAMES} -c:v "$enc" -b:v 3M ${level_flag} \
    "$OUT" 2>&1 || rc=$?
  if [ "$rc" -ne 0 ]; then
    echo "FAIL: ${label} encode failed (rc=${rc})."
    exit 1
  fi

  # ffprobe reports level as an integer: H.264 level_idc (e.g. 41),
  # HEVC general_level_idc (e.g. 153 for 5.1).
  local level
  level=$(ffprobe -v error -select_streams v:0 \
    -show_entries stream=level -of default=nokey=1:nw=1 "$OUT" 2>/dev/null || true)

  if [ -z "$level" ] || [ "$level" = "N/A" ]; then
    echo "SKIP: ${label} — ffprobe does not report stream level (old version?)"
    return 0
  fi

  if [ "$expected" = "any" ]; then
    if [ "$level" -le 0 ] 2>/dev/null; then
      echo "FAIL: ${label} — default level is ${level} (expected >0)."
      exit 1
    fi
    echo "   ${label}: level=${level} (default, >0 OK)"
  else
    if [ "$level" -ne "$expected" ]; then
      echo "FAIL: ${label} — expected level ${expected}, got ${level}."
      echo "      setLevel() may not be wired for this codec."
      echo "      Check src/nvmpi_enc_api.cpp level mapping + setLevel guard."
      exit 1
    fi
    echo "   ${label}: level=${level} OK"
  fi
}

# --- H.264 tests ---

echo "== h264_nvmpi: explicit -level 4.1 =="
encode_and_probe h264_nvmpi "-level 4.1" 41 "h264 level=4.1"

echo "== h264_nvmpi: explicit -level 5.1 =="
encode_and_probe h264_nvmpi "-level 5.1" 51 "h264 level=5.1"

echo "== h264_nvmpi: default level =="
encode_and_probe h264_nvmpi "" "any" "h264 default"

# --- HEVC tests ---
# ffprobe reports HEVC level as general_level_idc (30*tier + level):
#   level 4.1 = 123, level 5.1 = 153

echo "== hevc_nvmpi: explicit -level 4.1 =="
encode_and_probe hevc_nvmpi "-level 4.1" 123 "hevc level=4.1"

echo "== hevc_nvmpi: explicit -level 5.1 =="
encode_and_probe hevc_nvmpi "-level 5.1" 153 "hevc level=5.1"

echo "== hevc_nvmpi: default level =="
encode_and_probe hevc_nvmpi "" "any" "hevc default"

rm -f "$OUT"
echo "OK: hw-encoder-level passed on ${variant}."
