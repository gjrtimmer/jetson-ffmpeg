#!/usr/bin/env bash
# Encoder GOP/keyframe suite: only IDR packets may carry AV_PKT_FLAG_KEY.
# Regression guard for the all-packets-marked-keyframe bug (upstream
# Keylost/jetson-ffmpeg#26): the flag must come from V4L2 KeyFrame metadata,
# at the configured GOP cadence — never on every packet.
set -eu

variant="${JETSON_VARIANT:-unknown}"
echo "=== hw-encoder-gop on variant: ${variant} ==="

echo "== keyframe cadence: 90 frames at g=30 =="
ffmpeg -y -hide_banner -loglevel error \
  -f lavfi -i testsrc2=s=1280x720:r=30 -t 3 \
  -c:v h264_nvmpi -g 30 -b:v 3M /tmp/out_gop.mp4
total=$(ffprobe -v error -select_streams v:0 -show_entries packet=flags \
  -of csv=p=0 /tmp/out_gop.mp4 | wc -l)
keys=$(ffprobe -v error -select_streams v:0 -show_entries packet=flags \
  -of csv=p=0 /tmp/out_gop.mp4 | grep -c '^K' || true)
echo "   ${keys} keyframes / ${total} packets"
dump_flags() {
  echo "--- first 40 packet flags (K = keyframe; expect K every ~30) ---"
  ffprobe -v error -select_streams v:0 -show_entries packet=flags \
    -of csv=p=0 /tmp/out_gop.mp4 | head -40 | tr '\n' ' '; echo ""
  echo "Flag source: src/nvmpi_enc.cpp (enc_metadata.KeyFrame via getMetadata)"
  echo "-> ffmpeg/dev/common/libavcodec/nvmpi_enc.c (AV_PKT_FLAG_KEY mapping)."
}
[ "$keys" -ge 2 ] || { echo "FAIL: too few keyframes (${keys}/${total}) — K flag not set on IDR?"; dump_flags; exit 1; }
[ "$keys" -le 10 ] || { echo "FAIL: ${keys}/${total} packets flagged K — all-keyframe bug is back."; dump_flags; exit 1; }

echo "OK: hw-encoder-gop passed on ${variant}."
