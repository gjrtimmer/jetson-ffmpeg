# Issue #10: Decoder Blocking Wait — Design Specification

**Date:** 2026-06-15
**Issue:** [#10 — decoder: implement blocking wait in nvmpi_decoder_get_frame() for AV_CODEC_FLAG_LOW_DELAY](https://github.com/gjrtimmer/jetson-ffmpeg/issues/10)
**Approach:** A — Condition-variable in pool
**Status:** Approved, pending implementation

---

## Table of Contents

1. [Overview](#overview)
2. [Two-Branch Strategy](#two-branch-strategy)
3. [Branch 1: Modular Split (refactor/10-decoder-split)](#branch-1-modular-split)
4. [Branch 2: Blocking Wait (feat/10-decoder-blocking-wait)](#branch-2-blocking-wait)
5. [TDD Test Plan](#tdd-test-plan)
6. [Performance Measurement](#performance-measurement)
7. [Subagent Strategy](#subagent-strategy)
8. [Documentation Plan](#documentation-plan)
9. [Prior Art Analysis](#prior-art-analysis)
10. [Risks and Mitigations](#risks-and-mitigations)

---

## Overview

`nvmpi_decoder_get_frame(nvmpictx*, nvFrame*, bool wait)` ignores its `wait`
parameter — the body starts with `(void)wait;` and always returns `-1`
immediately when no frame is ready. The FFmpeg integration already passes
`avctx->flags & AV_CODEC_FLAG_LOW_DELAY`, so users running
`ffmpeg -flags low_delay` silently do not get low-delay semantics.

This design implements true blocking wait using condition variables in the
buffer pool, with a configurable tiered timeout, proper EOS/shutdown
signalling, and comprehensive thread-safety guarantees.

**Prerequisite:** Issue #2 (EOS/lifecycle fixes) — closed, merged to main.

---

## Two-Branch Strategy

```
main
 └─ refactor/10-decoder-split        ← merge first
     └─ feat/10-decoder-blocking-wait ← merge second (rebased on main after refactor merges)
```

The refactor branch isolates the mechanical file-split from the behavioral
change, keeping each MR reviewable and bisectable.

---

## Branch 1: Modular Split

**Branch name:** `refactor/10-decoder-split`

### Modular split convention

File naming pattern: `nvmpi_{codec}_{concern}.cpp`

This convention applies to both decoder and encoder. When the encoder grows
(issues #13, #15, #17), it follows the same pattern.

| Concern | Decoder file | Encoder file (future) | Contents |
|---------|-------------|----------------------|----------|
| Public API | `nvmpi_dec_api.cpp` | `nvmpi_enc_api.cpp` | `create`, `put_packet`/`put_frame`, `get_frame`/`get_packet`, `close`, `flush` |
| Capture/output loop | `nvmpi_dec_capture.cpp` | `nvmpi_enc_output.cpp` | V4L2 DQBUF loop thread, EOS handling, buffer transforms |
| Plane setup | `nvmpi_dec_planes.cpp` | `nvmpi_enc_planes.cpp` | V4L2 plane allocation, REQBUFS, STREAMON/OFF, resolution change |
| Internal header | `nvmpi_dec_internal.h` | `nvmpi_enc_internal.h` | Shared context struct, internal helpers, forward decls |

Shared infrastructure (unchanged naming):

| File | Contents |
|------|----------|
| `NVMPI_bufPool.hpp` | Template interface (header-only, gains CV + shutdown in feat branch) |
| `NVMPI_frameBuf.hpp/.cpp` | DMA buffer alloc/destroy |

### Split rules

1. `nvmpi_dec_internal.h` holds `struct nvmpictx` — single definition, included
   by all `nvmpi_dec_*.cpp` files. Located in `src/`, not installed.
2. Public API surface (`include/nvmpi.h`) unchanged — no ABI break.
3. Each `.cpp` file is independently compilable — no circular includes.
4. Capture thread function moves to `nvmpi_dec_capture.cpp` but references
   `nvmpictx` via internal header.
5. `CMakeLists.txt` updated with new source files (explicit list, not glob).
6. When to split further: file exceeds ~500 lines or gains a new logical concern.

### Commits (refactor branch)

1. `refactor(nvmpi): extract nvmpictx struct to nvmpi_dec_internal.h`
2. `refactor(nvmpi): extract capture loop to nvmpi_dec_capture.cpp`
3. `refactor(nvmpi): extract V4L2 plane setup to nvmpi_dec_planes.cpp`
4. `refactor(nvmpi): rename residual nvmpi_dec.cpp to nvmpi_dec_api.cpp`
5. `build(nvmpi): update CMakeLists.txt for split decoder sources`
6. `docs: add ARCHITECTURE.md with modular split convention`
7. `docs: update DEVELOPMENT.md with split-file workflow`

---

## Branch 2: Blocking Wait

**Branch name:** `feat/10-decoder-blocking-wait`

### NVMPI_bufPool changes

```
NVMPI_bufPool.hpp (template, stays header-only)
├── Existing: qFilledBuf(), dqFilledBuf(), qEmptyBuf(), dqEmptyBuf()
├── New: std::condition_variable cv_filledBuf
├── New: std::atomic<bool> m_shutdown{false}
├── Modified: qFilledBuf() → adds cv_filledBuf.notify_one() after push
├── New: dqFilledBuf(std::chrono::milliseconds timeout) → tiered blocking wait
│   └── Loop: cv.wait_for(100ms) → check shutdown → check !empty → re-wait until ceiling
├── New: shutdown() → set m_shutdown=true under lock, cv_filledBuf.notify_all()
└── New: reset() → clear m_shutdown (needed for flush/restart cycle)
```

Existing non-blocking `dqFilledBuf()` stays unchanged — encoder and
non-blocking decoder paths unaffected.

### Tiered wait loop design

The blocking `dqFilledBuf(timeout)` uses a tiered internal loop:

- AVOption `wait_timeout` sets ceiling (default 500ms, user-tunable, range 50-5000ms)
- Internally, `dqFilledBuf(timeout)` uses short `cv.wait_for(100ms)` iterations
  (100ms is a hardcoded internal granularity constant, not user-configurable)
- Each iteration re-checks `m_shutdown` flag before re-waiting
- Total wait bounded by the timeout parameter
- Benefits: responsive to cancellation (100ms granularity), user-tunable ceiling,
  clean EOS wakeup via `notify_all()`
- A pure 500ms `wait_for()` would block even after EOS signal arrives mid-wait
  (until timeout expires). Tiered loop with shutdown check catches it within 100ms.
  The `notify_all()` in `shutdown()` wakes immediately in most cases, but the
  loop is defense-in-depth.

### Decoder changes (post-refactor files)

**`nvmpi_dec_internal.h`:**
- `ctx->eos` → `std::atomic<bool>`
- Add `ctx->wait_timeout_ms` field (default 500, populated from AVOption)

**`nvmpi_dec_capture.cpp`:**
- All exit paths in `dec_capture_loop_fcn()` call `ctx->framePool->shutdown()`
  before return
- Covers: normal EOS (`V4L2_BUF_FLAG_LAST`), dqEvent error, dqBuffer error,
  `ctx->eos` check
- Replace 500µs spin-sleep backpressure (current line 668) with
  CV-based empty-buf wait — same pattern on empty-buf queue

**`nvmpi_dec_api.cpp`:**
- `nvmpi_decoder_get_frame()`: honor `wait` parameter
  - `wait == false`: existing non-blocking `dqFilledBuf()` path
  - `wait == true`: call `dqFilledBuf(ctx->wait_timeout_ms)`
- `nvmpi_decoder_flush()`: call `framePool->reset()` after drain + thread restart
- `nvmpi_decoder_close()`: `framePool->shutdown()` before thread join

### FFmpeg integration layer

**`ffmpeg/dev/common/libavcodec/nvmpi_dec.c`:**
- Add `wait_timeout` AVOption (int, default 500, range 50-5000,
  "Blocking wait timeout in milliseconds for low-delay mode")
- Pass value to decoder context at init
- Existing `AV_CODEC_FLAG_LOW_DELAY` → `wait=true` path unchanged
- Regenerate patches via `update_patch.sh`

### Thread-safety model

```
User thread                     Capture thread
─────────                       ──────────────
put_packet() ──────────────────→ V4L2 output plane
                                 dec_capture_loop_fcn()
get_frame(wait=true)             ├─ dqBuffer from capture plane
  │                              ├─ transform NvBuffer → nvFrame
  ├─ dqFilledBuf(timeout)        ├─ qFilledBuf() ← notify_one()
  │   └─ cv.wait_for(100ms) ◄───┘
  │   └─ check shutdown flag     │
  │   └─ re-wait or return       ├─ on EOS: shutdown() ← notify_all()
  │                              └─ return
close()
  ├─ ctx->eos = true (atomic)
  ├─ framePool->shutdown()
  └─ thread.join()
```

**Synchronization primitives:**

| Primitive | Location | Protects |
|-----------|----------|----------|
| `m_filledBuf` (mutex) | `NVMPI_bufPool` | filledBuf queue + cv_filledBuf |
| `m_emptyBuf` (mutex) | `NVMPI_bufPool` | emptyBuf queue |
| `cv_filledBuf` | `NVMPI_bufPool` | blocking wait on filled-buf availability |
| `m_shutdown` (atomic bool) | `NVMPI_bufPool` | shutdown signal (set under m_filledBuf lock) |
| `ctx->eos` (atomic bool) | `nvmpictx` | EOS signal between user + capture threads |

**Deadlock prevention:** Every capture-loop exit path calls `shutdown()`.
Tiered 100ms wait means worst-case wake latency is 100ms even if signal missed.
`shutdown()` acquires `m_filledBuf` lock briefly to set flag, then calls
`notify_all()` — no nested locks, no lock ordering issues.

### Public API impact

- `nvmpi_decoder_get_frame(ctx, frame, wait=true)` now blocks — behavior change
  for external consumers passing `wait=true` (previously a no-op)
- ABI unchanged (same function signature)
- Requires minor version bump in release
- Release notes entry documenting behavior change

### Commits (feat branch)

1. `test(nvmpi): add unit tests for NVMPI_bufPool blocking dequeue (TDD, all fail)`
2. `test(nvmpi): add hw-decoder-blocking-wait.sh hardware test suite`
3. `test(nvmpi): add hw-decoder-lifecycle.sh hardware test suite`
4. `test(nvmpi): add hw-perf-blocking-wait.sh performance measurement suite`
5. `feat(nvmpi): add condition-variable blocking dequeue + shutdown to NVMPI_bufPool`
6. `feat(nvmpi): make ctx->eos std::atomic<bool>`
7. `feat(nvmpi): wire shutdown() to all capture-loop exit paths`
8. `feat(nvmpi): implement blocking wait in nvmpi_decoder_get_frame()`
9. `feat(ffmpeg): add wait_timeout AVOption for low-delay decoder`
10. `chore(ffmpeg-dev): regenerate patches for all FFmpeg versions`
11. `docs(nvmpi): add API_REFERENCE.md with full libnvmpi API documentation`
12. `docs(nvmpi): add THREAD_SAFETY.md with thread model and synchronization`
13. `docs: update DEVELOPMENT.md, BUILD.md, test/README.md, TODO.md`

---

## TDD Test Plan

Tests written **before** implementation. Two layers.

### Layer 1: Unit tests (off-Jetson, C++)

**File:** `test/unit/test_bufpool.cpp`

| Test case | What it validates |
|-----------|-------------------|
| `dqFilledBuf_blocking_returns_on_push` | Thread A waits on `dqFilledBuf(500ms)`, thread B pushes after 50ms → A returns item within ~50ms |
| `dqFilledBuf_blocking_returns_null_on_timeout` | No producer, `dqFilledBuf(200ms)` returns `nullptr` after ~200ms (±50ms) |
| `dqFilledBuf_blocking_returns_null_on_shutdown` | Thread A waits, thread B calls `shutdown()` after 50ms → A returns `nullptr` immediately |
| `dqFilledBuf_nonblocking_unchanged` | Existing `dqFilledBuf()` (no timeout arg) still returns `nullptr` immediately when empty |
| `shutdown_wakes_all_waiters` | 3 threads waiting on `dqFilledBuf(5000ms)`, `shutdown()` wakes all within 200ms |
| `reset_clears_shutdown` | After `shutdown()` + `reset()`, `dqFilledBuf(timeout)` blocks again (doesn't return immediately) |
| `qFilledBuf_notifies_waiter` | Push into empty queue triggers exactly one waiting consumer |
| `concurrent_push_pop_stress` | 4 producers, 4 consumers, 10k items each. No lost items, no deadlock (30s timeout) |

**Framework:** Lightweight — `assert()` + `<thread>` + `<chrono>`. No external
dependencies. Single file compiled via CMake `add_executable`. Returns 0/non-zero.

**CMake integration:**
```cmake
if(BUILD_TESTING)
  add_executable(test_bufpool test/unit/test_bufpool.cpp)
  target_include_directories(test_bufpool PRIVATE include/)
  add_test(NAME bufpool COMMAND test_bufpool)
endif()
```

Runs in CI on x86 + arm64 (no Jetson needed — pool is pure C++).

### Layer 2: Hardware test suites (on-Jetson)

**Suite: `test/hw-decoder-blocking-wait.sh`**

| Test | Method | Pass criteria |
|------|--------|---------------|
| `low_delay_frames_arrive` | `ffmpeg -flags low_delay -c:v h264_nvmpi -i <short.mp4> -f null -` | Exit 0, all frames decoded (ffprobe frame count match) |
| `low_delay_no_hang_on_eos` | Same command, `timeout -k 5 30` wrapper | Exit code < 124 (not timeout) |
| `low_delay_vs_normal_latency` | Decode same file ±`-flags low_delay`, compare first-frame arrival | Low-delay first frame ≤ normal |
| `wait_timeout_option` | `ffmpeg -wait_timeout 100 -flags low_delay ...` | Runs without error, custom timeout accepted |
| `blocking_wait_hevc` | Same tests with `hevc_nvmpi` | HEVC path exercises same pool code |
| `rapid_open_close_blocking` | Loop: open decoder with low_delay, decode 5 frames, close × 20 | No hang, no crash, no leaked fds |

**Suite: `test/hw-decoder-lifecycle.sh`**

| Test | Method | Pass criteria |
|------|--------|---------------|
| `normal_decode_still_works` | Standard H.264 decode without low_delay | Same output as pre-refactor |
| `flush_and_reuse` | Decode file A, flush, decode file B in same context | Both produce correct frame count |
| `eos_no_hang` | Short file (10 frames), `timeout -k 5 15` | Clean exit, no timeout |

---

## Performance Measurement

**Suite: `test/hw-perf-blocking-wait.sh`**

Non-fatal — reports numbers, does not gate pass/fail (hardware variance).

### Metric 1: First-frame latency

```bash
# Wall-clock delta from ffmpeg start to first stderr line containing frame=
start_ns=$(date +%s%N)
ffmpeg -flags low_delay -c:v h264_nvmpi -i "$sample" -f null - 2>&1 | \
  grep -m1 'frame=' | head -1
end_ns=$(date +%s%N)
latency_ms=$(( (end_ns - start_ns) / 1000000 ))
```

Compare with and without `-flags low_delay`. Low-delay should produce first
frame after 1 input packet; normal after pipeline fill (2-5 packets).

### Metric 2: Per-frame jitter

Decode 300-frame file, capture per-frame decode timestamps via
`ffmpeg -benchmark`. Low-delay: timestamps evenly spaced (low stddev).
Normal: first N frames delayed, then burst.

### Metric 3: CPU usage comparison

```bash
/usr/bin/time -v ffmpeg -flags low_delay -c:v h264_nvmpi -i "$sample" -f null - 2>&1 | \
  grep "Percent of CPU"
```

Blocking wait should use **less** CPU than polling (no spin-sleep).

### Reporting format

```
PERF: first_frame_latency_normal=148ms
PERF: first_frame_latency_lowdelay=31ms
PERF: improvement=4.8x
PERF: cpu_normal=23%
PERF: cpu_lowdelay=18%
PERF: jitter_stddev_normal=45ms
PERF: jitter_stddev_lowdelay=8ms
```

---

## Subagent Strategy

### Model tiering

| Model | Cost | Use for |
|-------|------|---------|
| **haiku** | cheapest | Mechanical edits, file splits, comment updates, CMake changes |
| **sonnet** | mid | Test writing, doc authoring, FFmpeg integration, code review |
| **opus** | expensive | Thread-safety critical code (pool CV, shutdown, exit-path audit) |

### Branch 1: refactor/10-decoder-split

| Task | Agent type | Model |
|------|-----------|-------|
| Extract `nvmpictx` struct → `nvmpi_dec_internal.h` | cavecrew-builder | haiku |
| Extract `dec_capture_loop_fcn()` → `nvmpi_dec_capture.cpp` | cavecrew-builder | haiku |
| Extract V4L2 plane setup → `nvmpi_dec_planes.cpp` | cavecrew-builder | haiku |
| Trim `nvmpi_dec.cpp` → `nvmpi_dec_api.cpp` | cavecrew-builder | haiku |
| Update `CMakeLists.txt` | cavecrew-builder | haiku |
| Update all file headers + comments | cavecrew-builder | haiku |
| Write `docs/ARCHITECTURE.md` | general-purpose | sonnet |
| Review refactor | cavecrew-reviewer | sonnet |

First 4 extractions run as parallel agents (independent files).

### Branch 2: feat/10-decoder-blocking-wait

| Task | Agent type | Model |
|------|-----------|-------|
| Write `test/unit/test_bufpool.cpp` (TDD) | general-purpose | sonnet |
| Write `test/hw-decoder-blocking-wait.sh` | general-purpose | sonnet |
| Write `test/hw-decoder-lifecycle.sh` | cavecrew-builder | haiku |
| Write `test/hw-perf-blocking-wait.sh` | general-purpose | sonnet |
| Implement NVMPI_bufPool CV + shutdown | **main context** | opus |
| `ctx->eos` → `std::atomic<bool>` | cavecrew-builder | haiku |
| Wire `shutdown()` to capture-loop exits | **main context** | opus |
| Implement `get_frame()` wait=true | **main context** | opus |
| Add `wait_timeout` AVOption | cavecrew-builder | sonnet |
| Regenerate patches | Bash direct | — |
| Write API docs | general-purpose | sonnet |
| Write thread-safety docs | **main context** | opus |
| Update `nvmpi.h` comments | cavecrew-builder | haiku |
| Full code review | cavecrew-reviewer | sonnet |

### Execution phases

```
Phase 1: TDD — Write tests (all fail)
  ├─ [sonnet] test_bufpool.cpp            ─┐
  ├─ [sonnet] hw-decoder-blocking-wait.sh  ├─ parallel
  ├─ [haiku]  hw-decoder-lifecycle.sh     ─┘
  └─ [sonnet] hw-perf-blocking-wait.sh

Phase 2: Implementation (make tests pass)
  ├─ [opus/main] NVMPI_bufPool CV + shutdown
  ├─ [haiku] ctx->eos atomic (parallel)
  ├─ [opus/main] Capture loop exit paths
  ├─ [opus/main] get_frame wait=true
  ├─ [sonnet] AVOption wait_timeout
  └─ Bash: update_patch.sh

Phase 3: Documentation
  ├─ [sonnet] API_REFERENCE.md            ─┐
  ├─ [opus/main] THREAD_SAFETY.md          ├─ parallel
  ├─ [haiku] nvmpi.h comment updates      ─┘
  └─ [sonnet] ARCHITECTURE.md update

Phase 4: Verification
  ├─ Bash: build.sh --stubs
  ├─ Bash: test_bufpool (unit tests)
  ├─ Bash: try_build.sh (all 7 FFmpeg versions)
  └─ [sonnet] Full code review
```

### Cost estimate

| Model | Agent count | Avg tokens/agent |
|-------|------------|-------------------|
| haiku | ~8 | ~15k |
| sonnet | ~8 | ~30k |
| opus (main) | 4 tasks | in-context |

~60-70% cheaper than all-opus. Thread-safety critical code stays in opus main
context. Mechanical work goes haiku. Everything in between goes sonnet.

---

## Documentation Plan

### New documentation files

| File | Contents |
|------|----------|
| `docs/ARCHITECTURE.md` | Modular split convention, file naming pattern (`nvmpi_{codec}_{concern}`), when to split further, include rules, internal header pattern. Encoder must follow same convention. |
| `docs/THREAD_SAFETY.md` | Thread model diagram (user thread vs capture thread), mutex/CV ownership, `std::atomic` usage, shutdown ordering, deadlock prevention rules, EOS signalling contract. Per-function thread-safety annotations. |
| `docs/API_REFERENCE.md` | Full libnvmpi public API docs. Every function in `nvmpi.h`: signature, parameters, return values, thread-safety, blocking behavior, error conditions. |

### Updated documentation

| File | Changes |
|------|---------|
| `docs/DEVELOPMENT.md` | Add "Modular File Structure" section cross-referencing ARCHITECTURE.md. Update "Adding a New Feature" with split-file workflow. |
| `docs/BUILD.md` | Unit test build instructions (`-DBUILD_TESTING=ON`), test_bufpool execution. |
| `test/README.md` | Unit test section. Document new hw-* suites. |
| `TODO.md` | Remove blocking-wait item (implemented). |

### In-code documentation

| File | Annotations |
|------|-------------|
| `include/nvmpi.h` | Doxygen-style `/** */` comments on every public function. `nvmpi_decoder_get_frame()` gets detailed wait/timeout semantics. |
| `include/NVMPI_bufPool.hpp` | Class-level doc: thread-safety model, CV semantics, shutdown/reset lifecycle. Per-method docs. |
| `src/nvmpi_dec_internal.h` | Struct field docs. `ctx->eos` atomic semantics. `ctx->wait_timeout_ms` range/default. |
| `src/nvmpi_dec_capture.cpp` | Capture loop contract: shutdown obligations, exit-path checklist. |
| `src/nvmpi_dec_api.cpp` | API contract. Blocking behavior per function. |
| `src/nvmpi_dec_planes.cpp` | V4L2 plane lifecycle docs. |

### Documentation standards

- Public headers: doxygen-compatible `/** */` blocks
- Internal files: single-line `//` comments where WHY is non-obvious
- Thread-safety annotations: `/** @threadsafety ... */` on every public function
- Cross-references between docs (ARCHITECTURE ↔ THREAD_SAFETY ↔ API_REFERENCE)

---

## Prior Art Analysis

### 237-fork network sweep

| Finding | Source fork | Commit/branch | In our tree? | Quality |
|---------|-----------|---------------|-------------|---------|
| Blocking wait (condition_variable) | cgutman/cybernhl | `caac9899` / `low_latency` | NO | HIGH — Moonlight-tested, sole impl in network |
| EOS deadlock fix | cgutman | `58a977d4` | YES (#2) | CRITICAL — battle-tested |
| Pool refactor (better scoping) | w3sip | `feat/dq` (19 commits) | NO | Reference only — too divergent to cherry-pick |
| Pool size config | spotai | `cf0560b5` | YES (ancestor) | Already merged |
| setMaxPerfMode | douo, LanderN, 2+ | various | NO (tracked #9) | Separate issue |
| New encode API | bmegli | `437f8c55` | NO (tracked #15) | Separate issue |

### cgutman assessment

cgutman's `caac9899` is the **sole production blocking-wait implementation** in
the entire 237-fork network. Designed for Moonlight game streaming (extreme
latency sensitivity). However, it is used as **reference/prior art only**, not
a direct cherry-pick. Our implementation improves on it:

- **Tiered timeout** (cgutman uses indefinite wait → potential hang)
- **Configurable ceiling** via AVOption (cgutman hardcoded)
- **Explicit shutdown/reset lifecycle** (cgutman relies on EOS flag only)
- **Defense-in-depth** via 100ms re-check loop (cgutman single wait_for)

---

## Risks and Mitigations

| Risk | Severity | Mitigation |
|------|----------|------------|
| Deadlock: missed shutdown in capture-loop exit path | HIGH | Exhaustive audit of all exit paths. Unit test `shutdown_wakes_all_waiters`. hw-test `rapid_open_close_blocking`. |
| Behavior change for external `wait=true` callers | MEDIUM | Minor version bump. Release notes. ABI unchanged. |
| Performance regression in non-blocking path | LOW | Existing `dqFilledBuf()` unchanged. `qFilledBuf()` adds one `notify_one()` — negligible overhead (uncontended CV). |
| `ctx->eos` atomic migration breaks existing code | LOW | `std::atomic<bool>` is drop-in for `bool` reads/writes. No conditional logic changes. |
| Stubs build breaks (off-Jetson CI) | LOW | Unit tests exercise pool directly. `build.sh --stubs` in verification phase. |
| Refactor branch introduces regression | LOW | `hw-decoder-lifecycle.sh` guards existing behavior. `smoke-all.sh` before merge. |
