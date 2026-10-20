#!/usr/bin/env bash
# AV1 decoder suite: av1_nvmpi (libnvmpi >= 4.2.0, NVDEC on Orin). Decodes a
# committed 320x240 AV1 IVF fixture (5 key frames, no AV1 encoder is needed in
# the test image) and checks frame count, dimensions and raw output size, then
# a seek and a short decode. 64x64 would be rejected by NVDEC
# (InvalidResolution), hence the fixture size.
# AV1 decode is JetPack 6 only: V4L2_PIX_FMT_AV1 is absent on JetPack 5, where
# libnvmpi cannot create the decoder, so the suite SKIPs there. Encode is not
# supported (#69). No rapid open/close loops (Tegra driver segfaults).
set -eu
# shellcheck source-path=SCRIPTDIR/..
# shellcheck source=lib/common.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../lib/common.sh"

variant="${JETSON_VARIANT:-unknown}"
echo "=== hw-decoder-av1 on variant: ${variant} ==="

# The v4.2.x mock decoder returns no frames for the AV1 fourcc (0 frames from
# the fixture through the released libnvmpi), so this suite is hardware only.
if is_mock; then
  mock_skip "AV1 decode is not modelled by the mock"
  exit 0
fi
has_codec dec av1_nvmpi || { echo "SKIP: av1_nvmpi not compiled in."; exit 0; }

# JetPack 5 (L4T R35) lacks the V4L2 AV1 pixel format.
if grep -q 'R35' /etc/nv_tegra_release 2>/dev/null; then
  echo "SKIP: AV1 decode needs JetPack 6 (L4T R36), this is JetPack 5."
  exit 0
fi

FIX="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../fixtures/av1-320x240.ivf"
[ -s "$FIX" ] || ff_fail "fixture missing: ${FIX}"
W=320 H=240 N=5
T=/tmp/nvmpi-av1
trap 'rm -f "${T}"-* 2>/dev/null || true' EXIT

echo "== 1. decode: frame count and dimensions =="
# Stream geometry comes from the IVF header; the decoder itself is forced with
# ffmpeg (ffprobe on PATH may not be the patched build). 5 packets -> 5 frames.
dims=$(ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=p=0 "$FIX")
[ "$dims" = "${W},${H}" ] || ff_fail "fixture dimensions '${dims}', expected ${W},${H}."
run_ff_must "decode" -T 60 -c:v av1_nvmpi -i "$FIX" -f null -
assert_frames "$N" "av1 decode"
frames=$(printf '%s\n' "$FF_OUT" | grep -oP 'frame=\s*\K[0-9]+' | tail -1)
[ "$frames" -eq "$N" ] || ff_fail "expected exactly ${N} frames, got ${frames}."
printf '%s\n' "$FF_OUT" | grep -q "Video: av1.*${W}x${H}" || ff_fail "input stream not reported as av1 ${W}x${H}."

echo "== 2. raw output size (non-empty, correctly sized frames) =="
run_ff_must "rawvideo" -T 60 -loglevel warning -c:v av1_nvmpi -i "$FIX" \
  -pix_fmt yuv420p -f rawvideo "${T}-out.yuv"
size=$(wc -c < "${T}-out.yuv" | tr -d ' ')
exp=$((W * H * 3 * N / 2))
[ "$size" -eq "$exp" ] || ff_fail "raw output ${size} bytes, expected ${exp} (${N} frames of ${W}x${H} yuv420p)."
echo "   ${size} bytes - OK"

echo "== 3. seek: decode from the 3rd frame (every frame is a key frame) =="
run_ff_must "seek" -T 60 -ss 0.06 -c:v av1_nvmpi -i "$FIX" -f null -
assert_frames 2 "av1 after seek"

echo "== 4. short decode: -frames:v 1 exits cleanly =="
run_ff_must "short decode" -T 60 -c:v av1_nvmpi -i "$FIX" -frames:v 1 -f null -
assert_frames 1 "av1 first frame"

echo "PASS: hw-decoder-av1"
