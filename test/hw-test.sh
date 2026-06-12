#!/usr/bin/env bash
# HW encode/decode smoke test — runs on any Jetson runner variant.
# Invoked per hardware variant via the CI matrix (JETSON_VARIANT).
# h264_nvmpi/hevc_nvmpi have no software fallback, so success proves
# NVDEC+NVENC actually engaged on the target hardware.
set -eu

variant="${JETSON_VARIANT:-unknown}"
echo "=== Running HW test on variant: ${variant} ==="

echo "== 1. nvmpi codecs present? =="
ffmpeg -hide_banner -encoders 2>/dev/null | grep -E 'h264_nvmpi|hevc_nvmpi' || { echo "FAIL: nvmpi encoders missing"; exit 1; }
ffmpeg -hide_banner -decoders 2>/dev/null | grep -E 'h264_nvmpi|hevc_nvmpi' || { echo "FAIL: nvmpi decoders missing"; exit 1; }

echo "== 2. generate short H.264 test sample (software) =="
ffmpeg -y -hide_banner -loglevel error \
  -f lavfi -i testsrc2=s=1280x720:r=30 -t 3 \
  -c:v libx264 /tmp/in.mp4

echo "== 3. HW transcode: h264_nvmpi decode -> hevc_nvmpi encode =="
ffmpeg -y -hide_banner -loglevel error \
  -c:v h264_nvmpi -i /tmp/in.mp4 \
  -c:v hevc_nvmpi -b:v 3M /tmp/out.mkv

codec=$(ffprobe -v error -select_streams v:0 \
  -show_entries stream=codec_name -of default=nokey=1:nw=1 /tmp/out.mkv)
echo "   output codec = ${codec}"
[ "$codec" = "hevc" ] || { echo "FAIL: expected hevc, got ${codec}"; exit 1; }

echo "== 4. HW transcode: h264_nvmpi decode -> h264_nvmpi encode =="
ffmpeg -y -hide_banner -loglevel error \
  -c:v h264_nvmpi -i /tmp/in.mp4 \
  -c:v h264_nvmpi -b:v 3M /tmp/out_h264.mkv

codec=$(ffprobe -v error -select_streams v:0 \
  -show_entries stream=codec_name -of default=nokey=1:nw=1 /tmp/out_h264.mkv)
echo "   output codec = ${codec}"
[ "$codec" = "h264" ] || { echo "FAIL: expected h264, got ${codec}"; exit 1; }

# --- RTP/SDP decode tests ----------------------------------------------------
# Streams whose SPS/PPS arrive only out-of-band (RTSP/RTP: SDP
# sprop-parameter-sets -> avctx->extradata, never in the packet stream) used
# to decode zero frames (upstream Keylost/jetson-ffmpeg#14): the hardware
# parser needs an in-band SPS before it configures the capture plane. The
# decoder wrapper now primes the hardware with Annex-B extradata at init.
# RTP-over-loopback with an SDP file exercises the exact same demuxer code
# path (rtpdec sprop parsing) as RTSP, without needing an RTSP server.
#
# rtp_case NAME DECODER SDP_FILE PUBLISHER_ARGS...
#   Starts the publisher in the background, waits for the SDP file, then
#   decodes 25 frames with DECODER. A decoder that never produces frames
#   hits the 60 s timeout (exit 124) and fails the case.
# The publishers loop forever, so never leave one behind: kill the active
# one even when the script itself is terminated (CI job cancel, ^C).
pub=""
trap '[ -n "${pub:-}" ] && kill "${pub}" 2>/dev/null || true' EXIT
rtp_case() {
  name="$1"; dec="$2"; sdp="$3"; shift 3
  rm -f "$sdp"
  "$@" >"/tmp/rtp_pub_${name}.log" 2>&1 &
  pub=$!
  ok=0
  for _ in $(seq 1 50); do [ -s "$sdp" ] && { ok=1; break; }; sleep 0.2; done
  if [ "$ok" -ne 1 ]; then
    kill "$pub" 2>/dev/null || true
    echo "FAIL(${name}): publisher wrote no SDP (see /tmp/rtp_pub_${name}.log)"
    return 1
  fi
  rc=0
  # -k 5: SIGKILL a receiver that survives SIGTERM (e.g. wedged in decoder
  # close), so a regression can never stall the whole job/runner.
  timeout -k 5 60 ffmpeg -y -hide_banner -loglevel error \
    -protocol_whitelist file,udp,rtp -c:v "$dec" -i "$sdp" \
    -frames:v 25 -f null - || rc=$?
  kill "$pub" 2>/dev/null || true
  wait "$pub" 2>/dev/null || true
  pub=""
  if [ "$rc" -ne 0 ]; then
    echo "FAIL(${name}): ${dec} decoded <25 frames from RTP (rc=${rc}, publisher log: /tmp/rtp_pub_${name}.log)"
    return 1
  fi
  echo "   ${name}: 25 frames decoded"
}

echo "== 5. RTP decode, in-band SPS/PPS (control) =="
# The explicit h264_mp4toannexb BSF injects SPS/PPS in-band at every IDR
# (the RTP muxer alone payloads avcC natively and never would) — this case
# decoded fine even before the priming fix and proves the RTP harness works.
rtp_case inband_h264 h264_nvmpi /tmp/rtp_inband.sdp \
  ffmpeg -re -hide_banner -stream_loop -1 -i /tmp/in.mp4 -an -c:v copy \
    -bsf:v h264_mp4toannexb \
    -f rtp rtp://127.0.0.1:15004 -sdp_file /tmp/rtp_inband.sdp || exit 1

echo "== 6. RTP decode, out-of-band SPS/PPS (upstream Keylost#14 regression) =="
# filter_units strips SPS(7)/PPS(8) after Annex-B conversion, so parameter
# sets reach the decoder ONLY via the SDP — the exact failing RTSP scenario.
rtp_case oob_h264 h264_nvmpi /tmp/rtp_oob_h264.sdp \
  ffmpeg -re -hide_banner -stream_loop -1 -i /tmp/in.mp4 -an -c:v copy \
    -bsf:v 'h264_mp4toannexb,filter_units=remove_types=7|8' \
    -f rtp rtp://127.0.0.1:15006 -sdp_file /tmp/rtp_oob_h264.sdp || exit 1

echo "== 7. RTP decode, out-of-band VPS/SPS/PPS (hevc) =="
# hevc_nvmpi with +global_header puts the parameter sets in extradata (-> SDP
# sprop) and disables in-band insertion; filter_units strips VPS(32)/SPS(33)/
# PPS(34) as a guarantee. Also exercises HW encode+decode concurrently.
rtp_case oob_hevc hevc_nvmpi /tmp/rtp_oob_hevc.sdp \
  ffmpeg -re -hide_banner -f lavfi -i testsrc2=s=1280x720:r=30 \
    -c:v hevc_nvmpi -flags +global_header -g 30 -b:v 3M \
    -bsf:v 'filter_units=remove_types=32|33|34' \
    -f rtp rtp://127.0.0.1:15008 -sdp_file /tmp/rtp_oob_hevc.sdp || exit 1

echo ""
echo "OK: nvmpi hardware decode+encode works on ${variant}."
