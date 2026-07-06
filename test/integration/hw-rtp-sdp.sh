#!/usr/bin/env bash
# RTP/SDP streaming suite: streams whose SPS/PPS arrive only out-of-band
# (RTSP/RTP: SDP sprop-parameter-sets -> avctx->extradata, never in the
# packet stream) used to decode zero frames (upstream
# Keylost/jetson-ffmpeg#14): the hardware parser needs an in-band SPS before
# it configures the capture plane. The decoder wrapper now primes the
# hardware with Annex-B extradata at init. RTP-over-loopback with an SDP
# file exercises the exact same demuxer code path (rtpdec sprop parsing) as
# RTSP, without needing an RTSP server.
set -eu
# shellcheck source=test/gen-samples.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../gen-samples.sh"

variant="${JETSON_VARIANT:-unknown}"
echo "=== hw-rtp-sdp on variant: ${variant} ==="

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
    echo "FAIL(${name}): publisher wrote no SDP."
    echo "--- publisher log (last 10 lines) ---"
    tail -10 "/tmp/rtp_pub_${name}.log" 2>/dev/null || echo "(no publisher log)"
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
    echo "FAIL(${name}): ${dec} decoded <25 frames from RTP (rc=${rc}; 124 = 60 s timeout,"
    echo "      i.e. the decoder produced no frames — the Keylost#14 symptom)."
    echo "--- publisher log (last 10 lines) ---"
    tail -10 "/tmp/rtp_pub_${name}.log" 2>/dev/null || echo "(no publisher log)"
    return 1
  fi
  echo "   ${name}: 25 frames decoded"
}

[ -s "${SAMPLE_H264_720P}" ] || gen-sample-h264 "${SAMPLE_H264_720P}" 3

echo "== 1. RTP decode, in-band SPS/PPS (control) =="
# The explicit h264_mp4toannexb BSF injects SPS/PPS in-band at every IDR
# (the RTP muxer alone payloads avcC natively and never would) — this case
# decoded fine even before the priming fix and proves the RTP harness works.
rtp_case inband_h264 h264_nvmpi /tmp/rtp_inband.sdp \
  ffmpeg -re -hide_banner -stream_loop -1 -i "${SAMPLE_H264_720P}" -an -c:v copy \
    -bsf:v h264_mp4toannexb \
    -f rtp rtp://127.0.0.1:15004 -sdp_file /tmp/rtp_inband.sdp || exit 1

echo "== 2. RTP decode, out-of-band SPS/PPS (upstream Keylost#14 regression) =="
# filter_units strips SPS(7)/PPS(8) after Annex-B conversion, so parameter
# sets reach the decoder ONLY via the SDP — the exact failing RTSP scenario.
rtp_case oob_h264 h264_nvmpi /tmp/rtp_oob_h264.sdp \
  ffmpeg -re -hide_banner -stream_loop -1 -i "${SAMPLE_H264_720P}" -an -c:v copy \
    -bsf:v 'h264_mp4toannexb,filter_units=remove_types=7|8' \
    -f rtp rtp://127.0.0.1:15006 -sdp_file /tmp/rtp_oob_h264.sdp || exit 1

echo "== 3. RTP decode, out-of-band VPS/SPS/PPS (hevc) =="
# hevc_nvmpi with +global_header puts the parameter sets in extradata (-> SDP
# sprop) and disables in-band insertion; filter_units strips VPS(32)/SPS(33)/
# PPS(34) as a guarantee. Also exercises HW encode+decode concurrently.
rtp_case oob_hevc hevc_nvmpi /tmp/rtp_oob_hevc.sdp \
  ffmpeg -re -hide_banner -f lavfi -i testsrc2=s=1280x720:r=30 \
    -c:v hevc_nvmpi -flags +global_header -g 30 -b:v 3M \
    -bsf:v 'filter_units=remove_types=32|33|34' \
    -f rtp rtp://127.0.0.1:15008 -sdp_file /tmp/rtp_oob_hevc.sdp || exit 1

echo "OK: hw-rtp-sdp passed on ${variant}."
