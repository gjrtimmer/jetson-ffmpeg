#!/usr/bin/env bash
# Decoder blocking-wait suite: validates that -flags low_delay activates
# true blocking wait in nvmpi_decoder_get_frame() (issue #10).
# Section 7 (absorbed from the former hw-perf-blocking-wait) reports first-frame
# latency, CPU and throughput normal vs low_delay. It is informational only:
# metrics never gate pass/fail.
set -eu
# shellcheck source-path=SCRIPTDIR/..
# shellcheck source=lib/common.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../lib/common.sh"

variant="${JETSON_VARIANT:-unknown}"
echo "=== hw-decoder-blocking-wait on variant: ${variant} ==="

ensure_sample SAMPLE_H264_720P gen-sample-h264 3

expected_frames() {
  ffprobe -v error -select_streams v:0 -count_frames \
    -show_entries stream=nb_read_frames -of csv=p=0 "$1" 2>/dev/null || echo 0
}

echo "== 1. low_delay frames arrive (H.264) =="
run_ff -T 30 \
  -flags low_delay -c:v h264_nvmpi -i "${SAMPLE_H264_720P}" -f null - || true
if [ "$FF_RC" -eq 124 ]; then
  ff_fail "low_delay decode timed out (30s) — possible deadlock."
fi
if is_signal_rc "$FF_RC"; then
  ff_fail "low_delay decode confirmed crash (signal $((FF_RC - 128)))."
fi
exp=$(expected_frames "${SAMPLE_H264_720P}")
assert_frames 1 "low_delay decode (expected ~${exp})"

echo "== 2. low_delay no hang on EOS =="
run_ff -T 30 -loglevel error \
  -flags low_delay -c:v h264_nvmpi -i "${SAMPLE_H264_720P}" -f null - || true
if [ "$FF_RC" -eq 124 ]; then
  ff_fail "EOS hang with -flags low_delay."
fi
echo "   EOS exit clean (rc=${FF_RC}) — OK"

echo "== 3. low_delay completes without hang =="
# Blocking wait may have higher first-frame wall-clock than non-blocking
# (it waits for the actual decoded frame instead of returning -1/retry),
# so we only verify it completes within a reasonable time — not that it
# is faster than normal mode.
run_ff -T 15 -flags low_delay \
  -c:v h264_nvmpi -i "${SAMPLE_H264_720P}" -frames:v 1 -f null - || true
if [ "$FF_RC" -eq 124 ]; then
  ff_fail "low_delay single-frame decode timed out (15s)."
fi
echo "   low_delay single-frame decode OK (rc=${FF_RC})"

echo "== 4. wait_timeout AVOption =="
run_ff -T 30 -loglevel error \
  -flags low_delay -wait_timeout 100 -c:v h264_nvmpi \
  -i "${SAMPLE_H264_720P}" -f null - || true
if [ "$FF_RC" -eq 124 ]; then
  ff_fail "decode with wait_timeout=100 timed out."
fi
if printf '%s\n' "$FF_OUT" | grep -qi "unrecognized option\|no such option"; then
  ff_fail "wait_timeout AVOption not recognized."
fi
echo "   wait_timeout=100 accepted — OK"

echo "== 5. blocking wait HEVC =="
if has_codec enc libx265; then
  if [ ! -s "${SAMPLE_HEVC_720P}" ]; then
    echo "   skipped (HEVC sample unavailable)"
  else
    run_ff -T 30 -loglevel error \
      -flags low_delay -c:v hevc_nvmpi -i "${SAMPLE_HEVC_720P}" -f null - || true
    if [ "$FF_RC" -eq 124 ]; then
      ff_fail "HEVC low_delay decode timed out."
    fi
    echo "   HEVC low_delay OK (rc=${FF_RC})"
  fi
else
  echo "   skipped (libx265 not available)"
fi

echo "== 6. rapid open/close under blocking mode =="
for i in $(seq 1 5); do
  run_ff -T 10 -loglevel error \
    -flags low_delay -c:v h264_nvmpi -i "${SAMPLE_H264_720P}" \
    -frames:v 5 -f null - || true
  if is_signal_rc "$FF_RC"; then
    ff_fail "confirmed crash on iteration ${i} (signal $((FF_RC - 128)))."
  fi
  if [ "$FF_RC" -eq 124 ]; then
    ff_fail "hang on iteration ${i}."
  fi
done
echo "   5 rapid open/close cycles — OK"

if is_mock; then
  mock_skip "section 7 (timing numbers are meaningless without the hardware engines)"
  echo "OK: hw-decoder-blocking-wait passed on ${variant} (mock)."
  exit 0
fi
echo "== 7. perf (informational): normal vs low_delay =="
ensure_sample SAMPLE_H264_720P_LONG gen-sample-h264-long 10

# first_frame_ms [FLAGS]: wall-clock ms to decode one frame (result in M_MS).
# Results go to globals, not stdout: command substitution would also capture
# any stdout the driver prints ("Opening in BLOCKING MODE ...").
first_frame_ms() {
  local t0 t1
  t0=$(date +%s%N)
  run_ff -T 15 -loglevel error "$@" \
    -c:v h264_nvmpi -i "${SAMPLE_H264_720P_LONG}" -frames:v 1 -f null - || true
  t1=$(date +%s%N)
  M_MS=$(( (t1 - t0) / 1000000 ))
}

# full_decode [FLAGS]: full decode; sets M_FPS (frames/wall) and M_CPU (% of one
# core). A fresh bash runs timeout+ffmpeg then prints its children's cumulative
# user/sys via `times` (no /usr/bin/time on the runner image).
full_decode() {
  local t0 t1 out frames cpu_s
  t0=$(date +%s%N)
  out=$(bash -c 'timeout -k 5 60 ffmpeg -y -hide_banner -stats "$@" \
      -c:v h264_nvmpi -i "$0" -f null - 2>&1; times' \
      "${SAMPLE_H264_720P_LONG}" "$@" 2>&1) || true
  t1=$(date +%s%N)
  frames=$(printf '%s\n' "$out" | grep -oP 'frame=\s*\K[0-9]+' | tail -1 || true)
  # last line of `times` = children: "<u_min>m<u_s>s <s_min>m<s_s>s"
  cpu_s=$(printf '%s\n' "$out" | tail -1 | awk '{
    split($1,u,/[ms]/); split($2,s,/[ms]/); printf "%.3f", u[1]*60+u[2]+s[1]*60+s[2] }')
  M_FPS=$(awk -v f="${frames:-0}" -v a="$t0" -v b="$t1" 'BEGIN{ w=(b-a)/1e9; printf "%.1f", (w>0)?f/w:0 }')
  M_CPU=$(awk -v c="${cpu_s:-0}" -v a="$t0" -v b="$t1" 'BEGIN{ w=(b-a)/1e9; printf "%.0f", (w>0)?c*100/w:0 }')
}

sum_n=0; sum_l=0
for _ in 1 2; do
  first_frame_ms;                  sum_n=$((sum_n + M_MS))
  first_frame_ms -flags low_delay; sum_l=$((sum_l + M_MS))
done
full_decode;                  fps_n=$M_FPS; cpu_n=$M_CPU
full_decode -flags low_delay; fps_l=$M_FPS; cpu_l=$M_CPU
echo "PERF: first_frame_ms normal=$((sum_n / 2)) low_delay=$((sum_l / 2))"
echo "PERF: cpu_percent normal=${cpu_n} low_delay=${cpu_l}"
echo "PERF: fps normal=${fps_n} low_delay=${fps_l}"

echo "OK: hw-decoder-blocking-wait passed on ${variant}."
