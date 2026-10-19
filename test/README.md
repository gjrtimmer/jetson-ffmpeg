# Hardware test suites

Per-feature hardware tests for the nvmpi codecs. Everything in this directory
runs against the `ffmpeg` found in `PATH` and **requires a real Jetson** — the
nvmpi codecs have no software fallback, so a passing suite proves the hardware
engines (NVDEC/NVENC/VIC) actually engaged.

## Layout

Suites are organized by subsystem in subdirectories:

```
test/
├── hw-all.sh              # runner: auto-discovers + runs all suites
├── gen-samples.sh         # shared sample paths + generators, gen-all
├── lib/common.sh          # shared helpers: run_ff, assert_frames, has_codec, ...
├── smoke-all.sh           # full cross-version matrix harness
├── decoder/               # NVDEC decoder suites
│   ├── hw-decoder-blocking-wait.sh
│   ├── hw-decoder-chunk.sh
│   ├── hw-decoder-codecs.sh
│   ├── hw-decoder-downscale.sh
│   ├── hw-decoder-flush.sh
│   ├── hw-decoder-lifecycle.sh
│   ├── hw-decoder-mjpeg.sh
│   └── hw-decoder-pool.sh
├── encoder/               # NVENC encoder suites
│   ├── hw-encoder-gop.sh
│   ├── hw-encoder-header.sh
│   ├── hw-encoder-jpeg.sh
│   ├── hw-encoder-level.sh
│   ├── hw-encoder-modes.sh
│   ├── hw-encoder-pools.sh
│   └── hw-encoder-thread-safety.sh
├── filter/                # VIC filter suites
│   └── hw-filter-scale-vic.sh
├── format/                # pixel format / container suites
│   └── hw-format-pixfmt.sh
├── integration/           # cross-subsystem and smoke suites
│   ├── hw-smoke.sh
│   ├── hw-lifecycle-stress.sh
│   ├── hw-rtp-sdp.sh
│   └── hw-soak-decode.sh
├── perf/                  # performance / latency suites
│   ├── hw-perf-bench.sh
│   └── hw-perf-mode.sh
├── unit/                  # off-device unit tests (no Jetson required)
│   └── test_bufpool.cpp
└── api/                   # libnvmpi C API tests (no FFmpeg required)
    └── test_api.c
```

| Directory | What it covers |
|-----------|---------------|
| `decoder/` | NVDEC decode paths: lifecycle, codecs, chunking, blocking wait, downscale, MJPEG, pools, flush, teardown |
| `encoder/` | NVENC encode paths: lifecycle, GOP, headers, levels, pools, blocking, JPEG, thread safety |
| `filter/` | VIC hardware scale/CSC filter |
| `format/` | Pixel format and container support |
| `integration/` | Cross-subsystem smoke tests, RTP/SDP, soak tests, lifecycle stress (open/close + pthread guard) |
| `perf/` | Performance benchmarks and latency measurements (non-fatal) |

## Running

```bash
# everything, on the ffmpeg in PATH
JETSON_VARIANT=orin-nx test/hw-all.sh

# a subset of suites (names = filename minus the hw- prefix and .sh)
HW_SUITES="decoder-chunk encoder-gop" test/hw-all.sh

# one suite directly
test/decoder/hw-decoder-chunk.sh

# the full cross-version matrix (heaviest test in the repo)
test/smoke-all.sh                 # all versions
test/smoke-all.sh -v "6.1 8.0"    # subset
```

`JETSON_VARIANT` is a display label for logs/CI matrices — it does not change
behavior. The dev container exposes the alias `hw-all` (there is deliberately
no `test` alias; it would shadow the shell builtin).

## How the runner works

`hw-all.sh` recursively discovers `hw-*.sh` files in all subdirectories using
`find`, always runs `hw-smoke` first — when basic transcode is broken, nothing
else is worth reading — then the rest in sorted order. Before the first suite
it runs `gen-all` once, generating every shared input sample, so suites never
regenerate them. It prints the
binary under test (`which ffmpeg` + version line) so a CI log proves which
build was exercised. Passing suites are collapsed into GitLab CI log sections;
a failing suite's full output stays uncollapsed, followed by a one-suite rerun
hint. The run ends with a per-suite result block:

```
suite results (orin-nx):
  smoke                      PASS        4s
  decoder-chunk              PASS        9s
  encoder-gop                FAIL        3s
  encoder-header             PASS        6s
  rtp-sdp                    PASS       12s
total: 34s
```

A suite that exceeds `HW_SUITE_TIMEOUT` (default 600 s) is hard-killed with
`timeout -k` (grace `HW_SUITE_KILL_GRACE`, default 10 s). Processes left in
D-state (uninterruptible, stuck in the Tegra driver) are listed right after a
failed or timed-out suite and again at the end, so a driver hang is visible
instead of silently consuming the job timeout.

The runner exits non-zero if any suite failed. Adding a new suite requires no
runner changes: drop an executable `hw-{area}-{case}.sh` into the appropriate
subdirectory.

## Conventions

- **Suite naming**: `hw-{area}-{case}.sh` — area is the subsystem under test
  (`decoder`, `encoder`, `filter`, `perf`, …), case is the feature or
  regression it guards. One concern per file, so a red CI job names the broken
  layer directly.
- **Directory placement**: suites go in the subdirectory matching their area.
  Cross-subsystem tests go in `integration/`.
- **Suite header**: every file starts with a comment block explaining what it
  checks, why the behavior matters, and which upstream issue/fix it guards.
- **Common library**: suites start with `set -eu` and source
  `lib/common.sh` (`# shellcheck source-path=SCRIPTDIR/..` +
  `# shellcheck source=lib/common.sh`). Never open-code retries, `frame=`
  greps or codec probes; use the helpers below.
- **Samples**: generated inputs come from `gen-samples.sh`. Generators are
  named `gen-sample-{name-or-type}`; sample files use one specific path per
  case (`/tmp/nvmpi-sample-{type}.mp4`) — never reuse a generic name for a
  different kind of content. Add a new constant + generator next to the
  existing ones when a suite needs new input.
- **Shared samples are generated once** (`gen-all` in `hw-all.sh`). A suite
  never regenerates a shared sample; for standalone runs it calls
  `ensure_sample VAR GEN [secs]`, which generates only when the file is missing.
- **Failure style**: print `FAIL: <reason>` and exit non-zero; keep going only
  when later checks are independent. Negative tests must assert the *clean*
  failure mode (error message present, process not killed by a signal).
  **A FAIL must be fixable from the CI log alone**: include the observed
  values, the captured ffmpeg/tool output (last ~15 lines), and a pointer to
  the code under test. The runner prints failing suites uncollapsed and adds
  a one-suite rerun hint; passing suites are collapsed.
- **Suites ship with their feature**: a feature PR is not complete without the
  suite that guards it (tracked in #27).

## Common library (`lib/common.sh`)

| Helper | Purpose |
|--------|---------|
| `run_ff [-T secs] ARGS` | `ffmpeg -y -hide_banner ARGS` under `timeout -k` (default 60 s, `FF_TIMEOUT`); one retry after a signal-kill (200 ms cooldown); stdout+stderr in `FF_OUT`, rc in `FF_RC` (124/137 = timeout) |
| `run_ff_must LABEL [-T secs] ARGS` | `run_ff` plus `FAIL` with the last 15 output lines on timeout, crash or error |
| `ff_fail MSG` | print `FAIL: MSG` + tail-15 of `FF_OUT`, exit 1 |
| `assert_frames MIN [label]` | last `frame=` counter in `FF_OUT` must be >= MIN (needs ffmpeg stats, no `-loglevel error`) |
| `packet_count FILE` | demuxed packets of the first video stream |
| `has_codec enc\|dec NAME` | is the codec compiled into ffmpeg? |
| `device_health [label]` | known-good decode after a stress loop; FAIL if the V4L2 device is stuck |
| `is_signal_rc RC` | rc in the POSIX signal-kill range 129-159 (real-time signals excluded) |
| `ensure_sample VAR GEN [secs]` | generate a sample only when missing |

A signal-kill alone is not a bug (the Tegra V4L2 driver has stochastic
segfaults on rapid open/close); only a repeat after the cooldown is, which is
what `run_ff` encodes. All scripts are shellcheck-clean (`.shellcheckrc`
enables `external-sources`).

## Mock (stub) runs

`NVMPI_MOCK=1 test/hw-all.sh` runs the mockable suites against the
behavioural Jetson mock from jetson-nvmpi (`mock/`: drop-in `libnvv4l2`,
`libnvbufsurface`, `libnvbufsurftransform` with the real sonames, first on
`LD_LIBRARY_PATH`) instead of a Jetson. CI runs it as `test:stub:ffmpeg-<ver>`
on plain K3s runners (about 2 min per version), in parallel with the hw jobs.
Prepare a host with the mock libs on `LD_LIBRARY_PATH` and
`jetson-nvmpi/scripts/mock-dev-nodes.sh` (codec device nodes).

The mock is **additive**: every suite also runs on the Orin in the hw tiers, a
green mock run never replaces a hardware run, and the mock never runs on release
tags (the release gate is real hardware). It catches logic, lifecycle and
wrapper bugs and, via `auto_cancel`, keeps such failures off the shared Orin.

The mockable list is `MOCK_SUITES` in `hw-all.sh` (opt-in; new suites are not
mocked until listed). Not mockable: `decoder-mjpeg`, `encoder-jpeg` (NvJPEG not
mocked), `encoder-level` (canned SPS), `soak-decode`, `perf-bench`, `perf-mode`
(timing needs the engines). Inside mockable suites, assertions that need real
hardware output are guarded with `is_mock` / `mock_skip` (canned HEVC
parameter sets cannot be muxed; canned slices are not software-decodable; no
VUI; no perf numbers). Never guard a check to make the mock pass without it
still running on hardware.

## Where these run

- **GitLab CI** (`.gitlab-ci.yml`): `test:hw-fast:ffmpeg-<ver>` (only the soak
  suite excluded) and `test:hw-full:ffmpeg-<ver>` (all suites; 7.1 and 8.1 in
  every MR, all versions in the weekly full run), each executing
  `test/hw-all.sh` on the Jetson runner matrix. Jobs `needs:` their build and
  start right after it; a failed job cancels the rest of the pipeline.
- **GitHub Actions** (`.github/workflows/ci.yml`): same shape, manual-only.
- **`smoke-all.sh`**: local pre-push gate — nothing gets pushed without a
  green cross-version matrix.
