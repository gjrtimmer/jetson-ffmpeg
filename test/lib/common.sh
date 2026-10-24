# shellcheck shell=bash
# Shared helpers for the hw-* suites. Source, don't execute:
#
#   # shellcheck source-path=SCRIPTDIR/..
#   . "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../lib/common.sh"
#
# Provides (see test/README.md "Common library"):
#   run_ff [-T SECS] ARGS...   run ffmpeg under a per-call timeout with one
#                              signal retry; sets FF_OUT / FF_RC; returns FF_RC
#   run_ff_must LABEL ARGS...  run_ff + FAIL (tail-15 of output) on rc != 0
#   ff_fail MSG                print FAIL + tail-15 of FF_OUT, exit 1
#   assert_frames MIN [LABEL]  parse frame= from FF_OUT, FAIL below MIN
#   packet_count FILE          demuxed packet count of the first video stream
#   plane_psnr REF DEC W H [FIRST] [SCALE_OPTS] [REF_FIRST]
#                              "Y U V" minimum PSNR over 5 frames, paired by index
#   has_codec enc|dec NAME     is the codec compiled into ffmpeg?
#   device_health [LABEL]      decode a known-good sample; FAIL if the V4L2
#                              device is unusable (call after stress loops)
#   is_signal_rc RC            rc in the POSIX standard signal-kill range
#   is_mock                    true when run against the jetson-nvmpi mock
#                              (NVMPI_MOCK=1, CI test:stub); see mock_skip
#   mock_skip WHY              print the standard "skipped on mock" line
# plus everything from test/gen-samples.sh (sample paths, gen-*, gen-all).
#
# The Tegra V4L2 driver has stochastic segfaults on rapid device open/close
# (especially HEVC). One signal-kill does NOT prove a code bug; a repeat after
# a cooldown does, so run_ff retries a signal-killed call once.
# POSIX: death by signal N gives rc = 128+N. Standard signals 1-31 -> 129-159;
# real-time signals (162-192) are excluded: the Tegra driver uses them
# internally and they leak out of error paths without indicating a code bug.

_COMMON_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source-path=SCRIPTDIR/..
# shellcheck source=gen-samples.sh
. "${_COMMON_DIR}/../gen-samples.sh"

# No terminal on stdin (#136): ffmpeg calls tcsetattr on a tty stdin, and a
# `timeout` child (own process group, hence a background group) gets SIGTTOU and
# stops, so every call "hung" when a suite ran from an interactive shell. CI has
# no tty and never saw it. Suites read no stdin, so drop it for all children.
exec </dev/null

# Per-call ffmpeg wall-clock timeout (seconds) when -T is not given. A hung
# call is hard-killed (timeout -k) instead of eating the whole suite budget.
# Every ffmpeg `timeout` uses --foreground: ffmpeg stays in the suite's process
# group, so Ctrl-C / a group kill reaches it (no orphaned V4L2 users).
FF_TIMEOUT="${FF_TIMEOUT:-60}"
FF_KILL_GRACE=5

# shellcheck disable=SC2034  # read by callers after run_ff
FF_OUT="" FF_RC=0

is_signal_rc() { [ "$1" -ge 129 ] && [ "$1" -le 159 ]; }

# NVMPI_MOCK=1: the codecs run against the behavioural mock (no Jetson). Its
# bitstreams are canned (HEVC parameter sets are not muxable, slice data is not
# decodable) and it models no timing or driver quirks, so assertions that
# need real hardware output are guarded with is_mock. Every guarded assertion
# still runs on the Orin; the mock run is additive.
is_mock() { [ "${NVMPI_MOCK:-0}" = "1" ]; }
mock_skip() { echo "   SKIP on mock: $1 (asserted on real hardware only)"; }

# run_ff [-T SECS] ARGS... : ffmpeg -y -hide_banner ARGS under timeout -k.
# stdout+stderr are captured in FF_OUT. rc 124/137 = timeout / hard kill.
# A signal-killed run is retried once after 200 ms; FF_RC is the last rc.
run_ff() {
  local secs="$FF_TIMEOUT"
  if [ "${1:-}" = "-T" ]; then secs="$2"; shift 2; fi
  local attempt
  for attempt in 1 2; do
    FF_RC=0
    FF_OUT=$(timeout --foreground -k "$FF_KILL_GRACE" "$secs" ffmpeg -y -hide_banner "$@" 2>&1) || FF_RC=$?
    # 137 = hard-killed after a timeout (hang): do not retry, that would double the wait.
    [ "$FF_RC" -eq 137 ] && break
    is_signal_rc "$FF_RC" || break
    [ "$attempt" -eq 1 ] && { echo "  warn: signal $((FF_RC - 128)), retrying after 200 ms..."; sleep 0.2; }
  done
  return "$FF_RC"
}

# ff_fail MSG : standard failure exit with evidence.
ff_fail() {
  echo "FAIL: $1"
  echo "--- ffmpeg output (last 15 lines) ---"
  printf '%s\n' "$FF_OUT" | tail -15
  exit 1
}

# run_ff_must LABEL [-T SECS] ARGS... : run_ff, FAIL on timeout/crash/error.
run_ff_must() {
  local label="$1"; shift
  run_ff "$@" || true
  case "$FF_RC" in
    0) ;;
    124|137) ff_fail "${label} timed out (hung; hard-killed)." ;;
    *) if is_signal_rc "$FF_RC"; then
         ff_fail "${label} confirmed crash (signal $((FF_RC - 128)))."
       else
         ff_fail "${label} failed (rc=${FF_RC})."
       fi ;;
  esac
}

# assert_frames MIN [LABEL] : last frame= counter of FF_OUT must be >= MIN.
# Needs ffmpeg stats in the output (don't pass -loglevel error). Echoes count.
assert_frames() {
  local min="$1" label="${2:-decode}" frames
  frames=$(printf '%s\n' "$FF_OUT" | grep -oP 'frame=\s*\K[0-9]+' | tail -1 || true)
  if [ -z "$frames" ] || [ "$frames" -lt "$min" ]; then
    ff_fail "${label}: expected >= ${min} frames, got ${frames:-0}."
  fi
  echo "   ${label}: ${frames} frames - OK"
}

# packet_count FILE : packets demuxed from the first video stream (0 on error).
packet_count() {
  ffprobe -v error -count_packets -select_streams v:0 \
    -show_entries stream=nb_read_packets -of default=nokey=1:nw=1 "$1" 2>/dev/null || echo 0
}

# plane_psnr REF DEC W H [FIRST] [SCALE_OPTS] [REF_FIRST] : prints "Y U V", the
# minimum PSNR per plane over 5 frames starting at frame index FIRST (default 0)
# of DEC and REF_FIRST (default FIRST) of REF, both each scaled to WxH yuv420p (SCALE_OPTS is appended to the scale filter,
# e.g. in_range=pc:out_range=pc; a resolution change in REF resets ffmpeg's
# frame counter, so compare against a clip of the post-change part). Frames are paired by index (setpts=N/TB on
# both sides), not by timestamp: a raw Annex B reference has none and the
# decoded stream carries the decoder's. Never compare moving content across a
# decode+encode chain with this: a one-frame shift costs ~25 dB; use stills.
plane_psnr() {
  local ref="$1" dec="$2" w="$3" h="$4" first="${5:-0}" opts="${6:-}" rfirst="${7:-${5:-0}}"
  local log="${PSNR_LOG:-/tmp/nvmpi-psnr-$$.log}"
  local tail="setpts=N/TB,scale=${w}:${h}${opts:+:${opts}},format=yuv420p"
  local sa="select='between(n\,${rfirst}\,$((rfirst + 4)))',${tail}"
  local sb="select='between(n\,${first}\,$((first + 4)))',${tail}"
  run_ff_must "psnr compare" -T 60 -loglevel error \
    -i "$ref" -i "$dec" -frames:v 5 \
    -lavfi "[0:v]${sa}[a];[1:v]${sb}[b];[a][b]psnr=stats_file=${log}" \
    -f null -
  # stats_file lines: n:1 mse_avg:.. psnr_y:48.3 psnr_u:.. psnr_v:..
  awk '{ for (i = 1; i <= NF; i++) if ($i ~ /^psnr_[yuv]:/) { split($i, a, ":"); k = substr(a[1], 6, 1); v = (a[2] == "inf") ? 99 : a[2]; if (!(k in m) || v + 0 < m[k] + 0) m[k] = v } } END { printf "%s %s %s\n", m["y"], m["u"], m["v"] }' "$log"
  rm -f "$log"
}

# has_codec enc|dec NAME : is NAME in `ffmpeg -encoders` / `-decoders`?
has_codec() {
  local kind="$1" name="$2" flag=-encoders
  [ "$kind" = dec ] && flag=-decoders
  ffmpeg -hide_banner "$flag" 2>/dev/null | awk '{print $2}' | grep -qx "$name"
}

# device_health [LABEL] : after stress loops, the V4L2 decoder must still work.
device_health() {
  local label="${1:-device health}"
  run_ff_must "$label" -T 30 -c:v h264_nvmpi -i "${SAMPLE_H264_720P}" -frames:v 10 -f null -
  assert_frames 5 "$label"
}

# ensure_sample VAR GEN [ARGS...] : generate "${!VAR}" with GEN when missing
# (standalone suite run; hw-all.sh pre-generates everything via gen-all).
ensure_sample() {
  local var="$1" gen="$2"; shift 2
  [ -s "${!var}" ] || "$gen" "${!var}" "$@"
}
