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
├── gen-samples.sh         # shared sample generators (sourced by suites)
├── smoke-all.sh           # full cross-version matrix harness
├── decoder/               # NVDEC decoder suites
│   ├── hw-decoder-blocking-wait.sh
│   ├── hw-decoder-chunk.sh
│   ├── hw-decoder-codecs.sh
│   ├── hw-decoder-downscale.sh
│   ├── hw-decoder-flush.sh
│   ├── hw-decoder-lifecycle.sh
│   ├── hw-decoder-mjpeg.sh
│   ├── hw-decoder-pool.sh
│   └── hw-decoder-teardown.sh
├── encoder/               # NVENC encoder suites
│   ├── hw-encoder-blocking.sh
│   ├── hw-encoder-gop.sh
│   ├── hw-encoder-header.sh
│   ├── hw-encoder-jpeg.sh
│   ├── hw-encoder-level.sh
│   ├── hw-encoder-lifecycle.sh
│   ├── hw-encoder-pools.sh
│   └── hw-encoder-thread-safety.sh
├── filter/                # VIC filter suites
│   └── hw-filter-scale-vic.sh
├── format/                # pixel format / container suites
│   └── hw-format-pixfmt.sh
├── integration/           # cross-subsystem and smoke suites
│   ├── hw-smoke.sh
│   ├── hw-pthread-guard.sh
│   ├── hw-rtp-sdp.sh
│   └── hw-soak-decode.sh
├── perf/                  # performance / latency suites
│   ├── hw-perf-bench.sh
│   ├── hw-perf-blocking-wait.sh
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
| `integration/` | Cross-subsystem smoke tests, RTP/SDP, soak tests, pthread guard |
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
else is worth reading — then the rest in alphabetical order. It prints the
binary under test (`which ffmpeg` + version line) so a CI log proves which
build was exercised. Passing suites are collapsed into GitLab CI log sections;
a failing suite's full output stays uncollapsed, followed by a one-suite rerun
hint. The run ends with a per-suite result block:

```
suite results (orin-nx):
  smoke              PASS
  decoder-chunk      PASS
  encoder-gop        FAIL
  encoder-header     PASS
  rtp-sdp            PASS
```

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
- **Samples**: generated inputs come from `gen-samples.sh`. Generators are
  named `gen-sample-{name-or-type}`; sample files use one specific path per
  case (`/tmp/nvmpi-sample-{type}.mp4`) — never reuse a generic name for a
  different kind of content. Add a new constant + generator next to the
  existing ones when a suite needs new input.
- **Failure style**: print `FAIL: <reason>` and exit non-zero; keep going only
  when later checks are independent. Negative tests must assert the *clean*
  failure mode (error message present, process not killed by a signal).
  **A FAIL must be fixable from the CI log alone**: include the observed
  values, the captured ffmpeg/tool output (last ~15 lines), and a pointer to
  the code under test. The runner prints failing suites uncollapsed and adds
  a one-suite rerun hint; passing suites are collapsed.
- **Suites ship with their feature**: a feature PR is not complete without the
  suite that guards it (tracked in #27).

## Where these run

- **GitLab CI** (`.gitlab-ci.yml`): one `test:hw-ffmpeg-<ver>` job per
  supported FFmpeg version (artifact from its patch job), each executing
  `test/hw-all.sh` on the Jetson runner matrix.
- **GitHub Actions** (`.github/workflows/ci.yml`): same shape, manual-only.
- **`smoke-all.sh`**: local pre-push gate — nothing gets pushed without a
  green cross-version matrix.
