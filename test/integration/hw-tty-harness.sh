#!/usr/bin/env bash
# Harness regression guard for #136 — needs NO nvmpi hardware (software ffmpeg).
# Suites used to hang when run from an interactive terminal (ffmpeg stopped by
# SIGTTOU behind GNU `timeout`) and Ctrl-C left suite + ffmpeg orphaned. This
# runs a throwaway copy of the harness under a pseudo-terminal and asserts:
#   1. a software run_ff suite completes (rc 0), directly and via hw-all.sh;
#   2. Ctrl-C during hw-all.sh (serial and parallel mock batch) exits 130 and
#      leaves no ffmpeg / suite process behind.
# Needs python3 (pty module), ffmpeg and GNU timeout; SKIPs cleanly otherwise.
set -eu
# shellcheck source-path=SCRIPTDIR/..
# shellcheck source=lib/common.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../lib/common.sh"
TEST_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo "=== hw-tty-harness: terminal run + Ctrl-C (#136) ==="
skip() { echo "SKIP: $1"; echo "PASS: hw-tty-harness (skipped)"; exit 0; }
command -v python3 >/dev/null 2>&1 || skip "python3 not available (pty needed)"
python3 -I -c 'import pty' 2>/dev/null || skip "python3 has no pty module"
command -v ffmpeg >/dev/null 2>&1 || skip "ffmpeg not in PATH"
timeout --foreground 1 true 2>/dev/null || skip "timeout lacks --foreground (not GNU coreutils)"

T="$(mktemp -d "${TMPDIR:-/tmp}/tty-harness.XXXXXX")"
trap 'rm -rf "$T"' EXIT
TAG="nvmpi-136-$$"   # unique ffmpeg argument: leftover processes are found by it
mkdir -p "$T/test" "$T/bin" "$T/marks"

# Throwaway copy of the harness: own lock file (the real one is held by the
# hw-all.sh that runs THIS suite), real lib/ and samples.
cp -R "${TEST_DIR}/lib" "${TEST_DIR}/gen-samples.sh" "$T/test/"
sed "s#^LOCK_FILE=.*#LOCK_FILE=\"${T}/lock\"#" "${TEST_DIR}/hw-all.sh" >"$T/test/hw-all.sh"
# No flock on this host (macOS dev box): the nested run only needs it to succeed.
command -v flock >/dev/null 2>&1 || { printf '#!/bin/sh\nexit 0\n' >"$T/bin/flock"; chmod +x "$T/bin/flock"; }

# mk_suite NAME LONG(0|1): software-only suite built on run_ff, like the real ones.
mk_suite() {
  cat >"$T/test/hw-$1.sh" <<EOS
#!/usr/bin/env bash
set -eu
. "${TEST_DIR}/lib/common.sh"
if [ "$2" = 1 ]; then
  : >"${T}/marks/$1"
  run_ff -T 120 -f lavfi -re -i testsrc=d=300 -metadata comment=${TAG} -f null -
  echo "long suite ended rc=\${FF_RC}"
else
  run_ff_must sw -f lavfi -i testsrc=d=1 -metadata comment=${TAG} -f null -
  echo "tty ok"
fi
EOS
}
mk_suite ttyprobe 0; mk_suite ttylong1 1; mk_suite ttylong2 1

# pty_run SECS NMARKS CMD...: run CMD on a pseudo-terminal (it is the controlling
# tty and stdin). With NMARKS > 0 send ^C once that many files exist in
# $T/marks. Prints the output, then "pty-rc=N" (130-style: 128+signal).
cat >"$T/pty.py" <<'EOP'
import os, pty, select, sys, time
limit, nmarks, marks, cmd = float(sys.argv[1]), int(sys.argv[2]), sys.argv[3], sys.argv[4:]
pid, fd = pty.fork()
if pid == 0:
    os.execvp(cmd[0], cmd)
out, t0, sent = b"", time.time(), False
while True:
    if select.select([fd], [], [], 0.2)[0]:
        try:
            d = os.read(fd, 4096)
        except OSError:
            break
        if not d:
            break
        out += d
    if nmarks and not sent and len(os.listdir(marks)) >= nmarks:
        time.sleep(1.5)  # let ffmpeg start
        os.write(fd, b"\x03"); sent = True
    if time.time() - t0 > limit:
        os.kill(pid, 9); out += b"\n[pty] TIMEOUT: killed\n"; break
_, st = os.waitpid(pid, 0)
sys.stdout.write(out.decode(errors="replace"))
# os.WEXITSTATUS, not waitstatus_to_exitcode (3.9+): JetPack 5 ships Python 3.8.
print("pty-rc=%d" % (os.WEXITSTATUS(st) if os.WIFEXITED(st) else 128 + os.WTERMSIG(st)))
EOP
pty_run() { python3 -I "$T/pty.py" "$1" "$2" "$T/marks" "${@:3}"; }

# shellcheck disable=SC2009  # pgrep is not guaranteed on the minimal runner image
leftovers() { ps -eo args 2>/dev/null | grep -F -- "$TAG" | grep -v grep || true; }
fail() { echo "FAIL: $1"; printf '%s\n' "${OUT:-}" | tail -25; exit 1; }
hwall() { env PATH="$T/bin:$PATH" NVMPI_SAMPLE_DIR="${NVMPI_SAMPLE_DIR:-/tmp}" "$@" bash "$T/test/hw-all.sh"; }

echo "== 1. suite directly on a tty completes =="
OUT="$(pty_run 60 0 bash "$T/test/hw-ttyprobe.sh" 2>&1)" || true
case "$OUT" in *"tty ok"*"pty-rc=0"*) echo "   OK" ;; *) fail "suite did not complete on a tty (hang: stdin still the terminal?)" ;; esac

echo "== 2. hw-all.sh on a tty completes =="
OUT="$(pty_run 120 0 env PATH="$T/bin:$PATH" NVMPI_SAMPLE_DIR="${NVMPI_SAMPLE_DIR:-/tmp}" HW_SUITES=ttyprobe bash "$T/test/hw-all.sh" 2>&1)" || true
case "$OUT" in *"OK: all hw suites passed"*"pty-rc=0"*) echo "   OK" ;; *) fail "hw-all.sh did not finish cleanly on a tty" ;; esac

# ^C: needs the suite(s) to be inside ffmpeg (marks), then everything must stop.
ctrlc_case() {
  local label="$1" n="$2"; shift 2
  rm -f "$T"/marks/*
  OUT="$(pty_run 120 "$n" env PATH="$T/bin:$PATH" NVMPI_SAMPLE_DIR="${NVMPI_SAMPLE_DIR:-/tmp}" "$@" bash "$T/test/hw-all.sh" 2>&1)" || true
  case "$OUT" in *"pty-rc=130"*) ;; *) fail "${label}: Ctrl-C did not end hw-all.sh with rc 130" ;; esac
  sleep 1
  local left; left="$(leftovers)"
  if [ -n "$left" ]; then
    echo "$left" | awk '{print "   leftover: " $0}'
    pkill -f -- "$TAG" 2>/dev/null || true
    fail "${label}: processes survived Ctrl-C (orphans)"
  fi
  echo "   OK"
}
echo "== 3. Ctrl-C, serial run leaves no orphans =="
ctrlc_case serial 1 HW_SUITES=ttylong1
echo "== 4. Ctrl-C, parallel mock batch leaves no orphans =="
ctrlc_case parallel 2 NVMPI_MOCK=1 HW_JOBS=2 HW_SUITES="ttylong1 ttylong2"

echo "PASS: hw-tty-harness"
