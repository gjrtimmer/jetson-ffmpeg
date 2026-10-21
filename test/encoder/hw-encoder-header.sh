#!/usr/bin/env bash
# Encoder GLOBAL_HEADER suite: -flags +global_header must yield working
# extradata for BOTH h264_nvmpi and hevc_nvmpi (mp4 mux depends on it: the
# muxer builds avcC/hvcC from extradata). Exercises the extradata generation
# path including the bounded NAL start-code scan and its H.265 (VPS/SPS/PPS)
# branch.
#
# Assertions (version-proof):
#   1. encode+mux with +global_header succeeds;
#   2. the produced mp4 demuxes back to exactly N packets — this fails when
#      extradata is missing/garbage (avcC/hvcC not built), on every FFmpeg
#      version;
#   3. ffprobe's stream extradata_size is asserted non-zero ONLY when the
#      ffprobe in PATH supports that entry (FFmpeg >= 6.0; 4.2/4.4 print
#      nothing for it).
set -eu
# shellcheck source-path=SCRIPTDIR/..
# shellcheck source=lib/common.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../lib/common.sh"

variant="${JETSON_VARIANT:-unknown}"
echo "=== hw-encoder-header on variant: ${variant} ==="

FRAMES=30
for enc in h264_nvmpi hevc_nvmpi; do
  if is_mock && [ "$enc" = hevc_nvmpi ]; then
    echo "== ${enc} +global_header -> mp4 =="
    mock_skip "mock HEVC parameter sets are canned, hvcC cannot be built from them"
    continue
  fi
  echo "== ${enc} +global_header -> mp4 =="
  # Extradata generation lives in ffmpeg/dev/common/libavcodec/nvmpi_enc.c
  # (GLOBAL_HEADER throwaway-encoder NAL scan).
  run_ff_must "${enc} +global_header encode/mux" -T 60 \
    -f lavfi -i testsrc2=s=1280x720:r=30 \
    -frames:v ${FRAMES} -c:v "$enc" -flags +global_header -b:v 3M \
    /tmp/nvmpi-out-globalheader.mp4
  # Keep the encoder's stderr: the sw-decode check below overwrites FF_OUT.
  enc_out="$FF_OUT"

  # Roundtrip via the demuxer + software decoder: proves avcC/hvcC (built
  # from extradata) is present and sane — works on every FFmpeg version.
  packets=$(packet_count /tmp/nvmpi-out-globalheader.mp4)
  if [ "${packets:-0}" -ne "${FRAMES}" ]; then
    # Encoder-side count: libnvmpi logs one "packet pool empty; encoded
    # frame dropped" line per AU it discards (jetson-ffmpeg#133). If
    # FRAMES - drops == muxed packets, the loss is in the encoder, not in
    # mux/demux or extradata.
    drops=$(printf '%s\n' "$enc_out" | grep -c "encoded frame dropped" || true)
    echo "FAIL: ${enc} mp4 roundtrip returned ${packets:-0}/${FRAMES} packets —"
    echo "      encoder side: $((FRAMES - drops))/${FRAMES} AUs (${drops} dropped by libnvmpi);"
    echo "      muxed: ${packets:-0}. If both match, the encoder lost AUs;"
    echo "      otherwise extradata is missing or unusable (avcC/hvcC not built)."
    echo "--- encoder stderr (libnvmpi + last lines) ---"
    printf '%s\n' "$enc_out" | grep -v '^\[mock\]' | grep -E "libnvmpi|nvmpi" | head -20 || true
    printf '%s\n' "$enc_out" | grep -v '^\[mock\]' | tail -10
    echo "--- sw-decode check ---"
    run_ff -T 30 -i /tmp/nvmpi-out-globalheader.mp4 -f null - || true
    printf '%s\n' "$FF_OUT" | tail -10
    echo "--- stream info ---"
    ffprobe -v error -select_streams v:0 -show_streams /tmp/nvmpi-out-globalheader.mp4 2>&1 | head -20
    exit 1
  fi

  # Direct extradata_size assert where ffprobe supports it (>= 6.0).
  ed=$(ffprobe -v error -select_streams v:0 \
    -show_entries stream=extradata_size -of default=nokey=1:nw=1 \
    /tmp/nvmpi-out-globalheader.mp4 2>/dev/null || true)
  if [ -n "$ed" ] && [ "$ed" != "N/A" ]; then
    if [ "$ed" -eq 0 ]; then
      echo "FAIL: ${enc} stream has extradata_size=0 despite +global_header."
      echo "      Check the NAL scan in ffmpeg/dev/common/libavcodec/nvmpi_enc.c."
      exit 1
    fi
    echo "   ${enc}: ${packets}/${FRAMES} packets roundtrip, extradata_size=${ed}"
  else
    echo "   ${enc}: ${packets}/${FRAMES} packets roundtrip (ffprobe too old for extradata_size — roundtrip assertion only)"
  fi
done

echo "OK: hw-encoder-header passed on ${variant}."
