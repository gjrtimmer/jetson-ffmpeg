#!/usr/bin/env bash
# Decoder input-buffer suite: the chunk_size AVOption and the oversized-packet
# bounds check (packets larger than the compressed-input V4L2 buffers must be
# rejected with a clean error — no crash, no silent truncation).
set -eu
# shellcheck source-path=SCRIPTDIR/..
# shellcheck source=lib/common.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../lib/common.sh"

variant="${JETSON_VARIANT:-unknown}"
echo "=== hw-decoder-chunk on variant: ${variant} ==="

ensure_sample SAMPLE_H264_720P gen-sample-h264 3
ensure_sample SAMPLE_H264_OVERSIZED gen-sample-oversized 1

echo "== 1. explicit chunk_size accepted (16 MiB) =="
run_ff_must "decode with -chunk_size 16MiB" -T 60 -loglevel error \
  -chunk_size:v 16777216 -c:v h264_nvmpi -i "${SAMPLE_H264_720P}" -f null -
echo "   decode with chunk_size=16MiB OK"

echo "== 2. oversized packets rejected cleanly (chunk_size=64KiB) =="
# All-intra lossless noise: every access unit is far above 64 KiB, so every
# packet must be rejected. Required outcome is a *clean* failure: the
# libnvmpi bounds-check message appears and the process is not killed by a
# signal (no segfault/abort).

# Guard against an unfit sample (the reason this case once silently passed
# nothing): the largest packet must actually exceed the chunk_size under test.
max_pkt=$(ffprobe -v error -select_streams v:0 -show_entries packet=size \
  -of csv=p=0 "${SAMPLE_H264_OVERSIZED}" | sort -n | tail -1)
echo "   largest sample packet: ${max_pkt} bytes (chunk_size under test: 65536)"
if [ "${max_pkt:-0}" -le 65536 ]; then
  echo "FAIL: sample unfit for this case — largest packet (${max_pkt}) does not exceed 65536."
  echo "      Fix gen-sample-oversized in test/gen-samples.sh (content too compressible?)."
  exit 1
fi

run_ff -T 60 \
  -chunk_size:v 65536 -c:v h264_nvmpi -i "${SAMPLE_H264_OVERSIZED}" \
  -f null - || true
if is_signal_rc "$FF_RC"; then
  ff_fail "decoder confirmed crash (signal $((FF_RC - 128))) on oversized packet."
fi
if ! printf '%s\n' "$FF_OUT" | grep -q "exceeds chunk_size"; then
  echo "      Expected the libnvmpi '[libnvmpi][E]: ... exceeds chunk_size ...' message"
  echo "      (src/nvmpi_dec.cpp, nvmpi_decoder_put_packet bounds check). ffmpeg rc=${FF_RC}."
  ff_fail "oversized packet was not rejected."
fi
echo "   oversized packets rejected without crash"

echo "OK: hw-decoder-chunk passed on ${variant}."
