# Hardware test suites

Per-feature hardware tests for the nvmpi codecs. Everything in this directory
runs against the `ffmpeg` found in `PATH` and **requires a real Jetson** — the
nvmpi codecs have no software fallback, so a passing suite proves the hardware
engines (NVDEC/NVENC/VIC) actually engaged.

## Layout

| File | Kind | What it checks |
|------|------|----------------|
| `hw-all.sh` | runner | Runs every suite below (auto-discovered), `hw-smoke` first |
| `hw-smoke.sh` | suite | nvmpi codecs registered; h264→hevc and h264→h264 HW transcodes produce the expected codec |
| `hw-rtp-sdp.sh` | suite | RTP/SDP streams with out-of-band SPS/PPS decode frames (upstream [Keylost#14](https://github.com/Keylost/jetson-ffmpeg/issues/14) regression — extradata priming at decoder init) |
| `hw-decoder-chunk.sh` | suite | `chunk_size` AVOption works; packets larger than the input buffers are rejected with a clean error, never a crash or silent truncation |
| `hw-decoder-codecs.sh` | suite | MPEG-2, MPEG-4, VP8, VP9 decode paths (codec-type mapping and V4L2 decode pipeline per codec); VP8/VP9 skip when libvpx absent |
| `hw-decoder-downscale.sh` | suite | `-resize WxH` hardware downscale (VIC) produces the requested output dimensions |
| `hw-decoder-pool.sh` | suite | `frame_pool_size` AVOption boundary values (min=1, max=32); no deadlock or crash at extremes |
| `hw-encoder-header.sh` | suite | `-flags +global_header` yields non-empty extradata for h264_nvmpi **and** hevc_nvmpi (bounded NAL scan incl. the H.265 VPS/SPS/PPS branch) |
| `hw-encoder-gop.sh` | suite | `AV_PKT_FLAG_KEY` only on IDR packets at the configured GOP cadence (upstream [Keylost#26](https://github.com/Keylost/jetson-ffmpeg/issues/26) all-keyframe bug guard) |
| `hw-decoder-blocking-wait.sh` | suite | Blocking wait (`wait=true`) in `get_frame`: low-delay frames arrive, EOS no hang, latency comparison, `wait_timeout` AVOption, HEVC blocking, rapid open/close |
| `hw-decoder-lifecycle.sh` | suite | Decoder lifecycle correctness: normal decode, flush+reuse (seek), short file EOS |
| `hw-perf-blocking-wait.sh` | suite | Non-fatal performance metrics: first-frame latency, CPU usage, decode throughput (fps) |
| `gen-samples.sh` | helpers | Input-sample paths + generators (sourced by suites, never executed) |
| `smoke-all.sh` | harness | Full cross-version matrix: builds libnvmpi, then patch→configure→build→`hw-all.sh` for **every** supported FFmpeg version |
| `unit/test_bufpool.cpp` | unit test | NVMPI_bufPool blocking dequeue, shutdown, reset, concurrent stress (no Jetson required; build with `-DBUILD_TESTING=ON`) |

Planned expansion (formats, JPEG, libnvmpi API harness): see
[#27](https://github.com/gjrtimmer/jetson-ffmpeg/issues/27). Lifecycle, flush,
and perf suites landed with [#10](https://github.com/gjrtimmer/jetson-ffmpeg/issues/10).

## Running

```bash
# everything, on the ffmpeg in PATH
JETSON_VARIANT=orin-nx test/hw-all.sh

# a subset of suites (names = filename minus the hw- prefix and .sh)
HW_SUITES="decoder-chunk encoder-gop" test/hw-all.sh

# one suite directly
test/hw-decoder-chunk.sh

# the full cross-version matrix (heaviest test in the repo)
test/smoke-all.sh                 # all versions
test/smoke-all.sh -v "6.1 8.0"    # subset
```

`JETSON_VARIANT` is a display label for logs/CI matrices — it does not change
behavior. The dev container exposes the alias `hw-all` (there is deliberately
no `test` alias; it would shadow the shell builtin).

## How the runner works

`hw-all.sh` globs `test/hw-*.sh` (excluding itself), always runs `hw-smoke`
first — when basic transcode is broken, nothing else is worth reading — then
the rest in alphabetical order. It prints the binary under test (`which
ffmpeg` + version line) so a CI log proves which build was exercised. Passing
suites are collapsed into GitLab CI log sections; a failing suite's full
output stays uncollapsed, followed by a one-suite rerun hint. The run ends
with a per-suite result block:

```
suite results (orin-nx):
  smoke              PASS
  decoder-chunk      PASS
  encoder-gop        FAIL
  encoder-header     PASS
  rtp-sdp            PASS
```

The runner exits non-zero if any suite failed. Adding a new suite requires no
runner changes: drop an executable `hw-{area}-{case}.sh` into this directory.

## Conventions

- **Suite naming**: `hw-{area}-{case}.sh` — area is the subsystem under test
  (`decoder`, `encoder`, `rtp`, later `jpeg`, `flush`, …), case is the feature
  or regression it guards. One concern per file, so a red CI job names the
  broken layer directly.
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
