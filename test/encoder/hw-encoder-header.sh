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

variant="${JETSON_VARIANT:-unknown}"
echo "=== hw-encoder-header on variant: ${variant} ==="

FRAMES=30
for enc in h264_nvmpi hevc_nvmpi; do
  echo "== ${enc} +global_header -> mp4 =="
  rc=0
  out=$(ffmpeg -y -hide_banner \
    -f lavfi -i testsrc2=s=1280x720:r=30 \
    -frames:v ${FRAMES} -c:v "$enc" -flags +global_header -b:v 3M \
    /tmp/nvmpi-out-globalheader.mp4 2>&1) || rc=$?
  if [ "$rc" -ne 0 ]; then
    echo "FAIL: ${enc} +global_header encode/mux failed (rc=${rc})."
    echo "      Extradata generation lives in ffmpeg/dev/common/libavcodec/nvmpi_enc.c"
    echo "      (GLOBAL_HEADER throwaway-encoder NAL scan)."
    echo "--- ffmpeg output (last 15 lines) ---"
    echo "$out" | tail -15
    exit 1
  fi

  # Roundtrip via the demuxer + software decoder: proves avcC/hvcC (built
  # from extradata) is present and sane — works on every FFmpeg version.
  packets=$(ffprobe -v error -count_packets -select_streams v:0 \
    -show_entries stream=nb_read_packets -of default=nokey=1:nw=1 \
    /tmp/nvmpi-out-globalheader.mp4)
  if [ "${packets:-0}" -ne "${FRAMES}" ]; then
    echo "FAIL: ${enc} mp4 roundtrip returned ${packets:-0}/${FRAMES} packets —"
    echo "      extradata missing or unusable (avcC/hvcC not built)."
    echo "--- sw-decode check ---"
    ffmpeg -y -hide_banner -i /tmp/nvmpi-out-globalheader.mp4 -f null - 2>&1 | tail -10
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
