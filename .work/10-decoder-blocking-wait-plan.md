# Decoder Blocking Wait — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement true blocking wait in `nvmpi_decoder_get_frame()` for `AV_CODEC_FLAG_LOW_DELAY` using condition variables in the buffer pool.

**Architecture:** Two sequential branches. `refactor/10-decoder-split` splits the 1043-line `nvmpi_dec.cpp` into four files by concern — pure mechanical moves, no behavioral changes, no new code. After that branch merges to main, `feat/10-decoder-blocking-wait` adds `std::condition_variable` + `shutdown()` to `NVMPI_bufPool`, makes `ctx->eos` atomic, wires blocking wait through the capture loop, and honours the `wait` parameter with a configurable tiered timeout (AVOption `wait_timeout`, default 500ms).

**Tech Stack:** C++11 (`std::condition_variable`, `std::atomic`, `std::chrono`), V4L2, CMake, Bash (hw-test suites), FFmpeg C API (AVOption system).

**Design spec:** `.work/2026-06-15-issue-10-decoder-blocking-wait-design.md`

---

## File Map

### Branch 1: refactor/10-decoder-split (pure mechanical moves)

| Action | Path | Responsibility |
|--------|------|----------------|
| Create | `src/nvmpi_dec_internal.h` | `nvmpictx` struct, forward decls, internal helpers |
| Create | `src/nvmpi_dec_capture.cpp` | `dec_capture_loop_fcn()`, `respondToResolutionEvent()` |
| Create | `src/nvmpi_dec_planes.cpp` | `initDecoderCapturePlane()`, `deinitDecoderCapturePlane()`, `getNvColorFormatFromV4l2Format()` |
| Rename | `src/nvmpi_dec.cpp` → `src/nvmpi_dec_api.cpp` | Public API + helpers that remain |
| Modify | `CMakeLists.txt` | Replace `nvmpi_dec.cpp` with 3 new source files |
| Create | `docs/ARCHITECTURE.md` | Document the modular split convention |
| Modify | `docs/DEVELOPMENT.md` | Add "Modular File Structure" section |

### Branch 2: feat/10-decoder-blocking-wait (all new code)

| Action | Path | Responsibility |
|--------|------|----------------|
| Create | `test/unit/test_bufpool.cpp` | Unit tests for blocking dequeue (TDD) |
| Create | `test/hw-decoder-blocking-wait.sh` | Hardware test suite for low-delay decode |
| Create | `test/hw-decoder-lifecycle.sh` | Hardware test suite guarding refactor correctness |
| Create | `test/hw-perf-blocking-wait.sh` | Performance measurement suite |
| Modify | `include/NVMPI_bufPool.hpp` | Add CV, shutdown, blocking dqFilledBuf, reset |
| Modify | `src/nvmpi_dec_internal.h` | `ctx->eos` → `std::atomic<bool>`, add `wait_timeout_ms` |
| Modify | `src/nvmpi_dec_capture.cpp` | Wire `shutdown()` to all exit paths |
| Modify | `src/nvmpi_dec_api.cpp` | Honour `wait` param in `get_frame()`, call `reset()` in flush |
| Modify | `ffmpeg/dev/common/libavcodec/nvmpi_dec.c` | Add `wait_timeout` AVOption, pass to libnvmpi |
| Modify | `include/nvmpi.h` | Add `wait_timeout` to `nvDecParam`, update doc comments |
| Modify | `CMakeLists.txt` | Add `test/unit/test_bufpool.cpp` under `BUILD_TESTING` |
| Create | `docs/THREAD_SAFETY.md` | Thread model, sync primitives, deadlock prevention |
| Create | `docs/API_REFERENCE.md` | Full libnvmpi public API documentation |
| Modify | `docs/BUILD.md` | Add unit test build instructions |
| Modify | `test/README.md` | Document new test suites |
| Modify | `TODO.md` | Remove blocking-wait item |

---

# BRANCH 1: refactor/10-decoder-split

**Rule:** NO new code, NO behavioral changes, NO tests. Every file must contain only code that was already in `nvmpi_dec.cpp`, moved verbatim. Header comments describe what the code does NOW, not what future issues will change.

---

### Task 1: Create branch and post issue comment

- [ ] **Step 1: Create the refactor branch**

```bash
git checkout -b refactor/10-decoder-split main
```

- [ ] **Step 2: Post implementation plan comment on issue #10**

```bash
gh issue comment 10 -R gjrtimmer/jetson-ffmpeg --body "Starting work on #10 with a two-branch approach.

**Branch 1:** \`refactor/10-decoder-split\` — mechanical file split of \`nvmpi_dec.cpp\` (~1040 lines) into modular files by concern. No behavioral changes. Establishes file structure for the feature.

**Branch 2:** \`feat/10-decoder-blocking-wait\` — implements blocking wait with condition variables, tiered timeout, configurable AVOption. TDD with unit tests + hw test suites.

Design spec: \`.work/2026-06-15-issue-10-decoder-blocking-wait-design.md\`
Plan: \`.work/10-decoder-blocking-wait-plan.md\`"
```

---

### Task 2: Extract `nvmpictx` struct to `nvmpi_dec_internal.h`

**Agent:** cavecrew-builder (haiku)

**Files:**
- Create: `src/nvmpi_dec_internal.h`
- Modify: `src/nvmpi_dec.cpp`

- [ ] **Step 1: Create `src/nvmpi_dec_internal.h`**

This header is a verbatim extraction of the struct, macros, includes, and forward declarations from `nvmpi_dec.cpp` lines 1–132. The only new content is the `#pragma once` guard and the include of this header from each `.cpp` file.

```cpp
#pragma once

#include "nvmpi.h"
#include "NvVideoDecoder.h"
#include "nvUtils2NvBuf.h"
#include "NVMPI_bufPool.hpp"
#include "NVMPI_frameBuf.hpp"
#include <vector>
#include <iostream>
#include <thread>
#include <unistd.h>
#include <queue>
#include <mutex>
#include <condition_variable>

#define CHUNK_SIZE_DEFAULT 10000000
#define MAX_BUFFERS 32

#define TEST_ERROR(condition, message, errorCode)    \
	if (condition)                               \
{                                                    \
	std::cerr<< message;			     \
}

using namespace std;

struct nvmpictx
{
	NvVideoDecoder *dec{nullptr};
	bool eos{false};
	int index{0};
	unsigned int coded_width{0};
	unsigned int coded_height{0};
	unsigned int output_width{0};
	unsigned int output_height{0};
	nvSize resized{0, 0};

	int numberCaptureBuffers{0};
	int dmaBufferFileDescriptor[MAX_BUFFERS];

#ifdef WITH_NVUTILS
	NvBufSurface *dmaBufferSurface[MAX_BUFFERS];
	NvBufSurfTransformConfigParams session;
#else
	NvBufferSession session;
#endif
	NvBufferTransformParams transform_params;
	NvBufferRect src_rect, dest_rect;

	nvPixFormat out_pixfmt;
	unsigned int decoder_pixfmt{0};
	std::thread dec_capture_loop;

	int frame_pool_size{12};
	uint32_t chunk_size{CHUNK_SIZE_DEFAULT};
	bool max_perf{true};
	bool disable_dpb{false};
	NVMPI_bufPool<NVMPI_frameBuf*>* framePool;
	std::vector<NVMPI_frameBuf*> allocatedFrameBufs;

	unsigned int num_planes;
	unsigned int frame_linesize[MAX_NUM_PLANES];
	unsigned int frame_height[MAX_NUM_PLANES];
	unsigned int frame_linedatasize[MAX_NUM_PLANES];

	void deinitFramePool();
	void initFramePool();
	void updateFrameSizeParams();
	void updateBufferTransformParams();
	void initDecoderCapturePlane(v4l2_format &format);
	void deinitDecoderCapturePlane();
};

NvBufferColorFormat getNvColorFormatFromV4l2Format(v4l2_format &format, bool want_10bit);
void dec_capture_loop_fcn(void *arg);
void respondToResolutionEvent(v4l2_format &format, v4l2_crop &crop, nvmpictx* ctx);
int copyNvBufToFrame(nvmpictx* ctx, NVMPI_frameBuf *nvmpiBuf, nvFrame* frame);
```

Note: preserve all existing comments from the struct members exactly as they appear in `nvmpi_dec.cpp` lines 68–132. The code block above omits them for brevity; the actual file must include them verbatim.

- [ ] **Step 2: Replace the top of `nvmpi_dec.cpp`**

Remove everything from line 1 (the file header comment) through line 132 (closing `};` of `struct nvmpictx`), and the `using namespace std;` line. Replace with:

```cpp
#include "nvmpi_dec_internal.h"
```

The existing file header comment (`nvmpi_dec.cpp — hardware video decoder pipeline...`) is removed because it will be replaced with a new header when the file is renamed to `nvmpi_dec_api.cpp` in Task 5.

- [ ] **Step 3: Verify it compiles**

```bash
./scripts/build.sh --stubs --clean
```

Expected: build succeeds (single translation unit still has everything via include).

- [ ] **Step 4: Commit**

```bash
git add src/nvmpi_dec_internal.h src/nvmpi_dec.cpp
git commit -m "refactor(nvmpi): extract nvmpictx struct to nvmpi_dec_internal.h

Move the decoder context struct, macros, includes, and forward
declarations to a shared internal header. No behavioral change."
```

---

### Task 3: Extract capture loop to `nvmpi_dec_capture.cpp`

**Agent:** cavecrew-builder (haiku)

**Files:**
- Create: `src/nvmpi_dec_capture.cpp`
- Modify: `src/nvmpi_dec.cpp`

- [ ] **Step 1: Create `src/nvmpi_dec_capture.cpp`**

Move these functions verbatim from `nvmpi_dec.cpp`:
- `respondToResolutionEvent()` (lines 464–514)
- The commented-out `transFormWorker` struct (lines 516–543) — keep as-is
- `dec_capture_loop_fcn()` (lines 555–695)

Include their existing comments exactly as they are. File content:

```cpp
#include "nvmpi_dec_internal.h"

// --- respondToResolutionEvent (verbatim from nvmpi_dec.cpp) ---
// --- commented-out transFormWorker struct (verbatim) ---
// --- dec_capture_loop_fcn (verbatim) ---
```

(All code moved verbatim — no added/changed/removed lines beyond the `#include` at top.)

- [ ] **Step 2: Remove those functions from `nvmpi_dec.cpp`**

Delete lines 460–695 (from the comment above `respondToResolutionEvent` through the closing `}` of `dec_capture_loop_fcn`). The forward declarations in `nvmpi_dec_internal.h` make them visible to `nvmpi_dec.cpp`.

- [ ] **Step 3: Verify it compiles**

```bash
./scripts/build.sh --stubs --clean
```

Expected: build succeeds. `dec_capture_loop_fcn` is referenced by `nvmpi_create_decoder` (still in `nvmpi_dec.cpp`) and resolved at link time.

- [ ] **Step 4: Commit**

```bash
git add src/nvmpi_dec_capture.cpp src/nvmpi_dec.cpp
git commit -m "refactor(nvmpi): extract capture loop to nvmpi_dec_capture.cpp

Move dec_capture_loop_fcn() and respondToResolutionEvent() to a
dedicated file. No behavioral change."
```

---

### Task 4: Extract V4L2 plane setup to `nvmpi_dec_planes.cpp`

**Agent:** cavecrew-builder (haiku)

**Files:**
- Create: `src/nvmpi_dec_planes.cpp`
- Modify: `src/nvmpi_dec.cpp`

- [ ] **Step 1: Create `src/nvmpi_dec_planes.cpp`**

Move these functions verbatim from `nvmpi_dec.cpp`:
- `getNvColorFormatFromV4l2Format()` (lines 134–209 before previous extractions; recalculate after Task 3)
- `nvmpictx::initDecoderCapturePlane()` (lines 217–292 original)
- `nvmpictx::deinitDecoderCapturePlane()` (lines 297–319 original)

Include their existing comments exactly. File content:

```cpp
#include "nvmpi_dec_internal.h"

// --- getNvColorFormatFromV4l2Format (verbatim) ---
// --- nvmpictx::initDecoderCapturePlane (verbatim) ---
// --- nvmpictx::deinitDecoderCapturePlane (verbatim) ---
```

- [ ] **Step 2: Remove those functions from `nvmpi_dec.cpp`**

Delete `getNvColorFormatFromV4l2Format`, `initDecoderCapturePlane`, `deinitDecoderCapturePlane` and their preceding comments.

- [ ] **Step 3: Verify it compiles**

```bash
./scripts/build.sh --stubs --clean
```

- [ ] **Step 4: Commit**

```bash
git add src/nvmpi_dec_planes.cpp src/nvmpi_dec.cpp
git commit -m "refactor(nvmpi): extract V4L2 plane setup to nvmpi_dec_planes.cpp

Move initDecoderCapturePlane(), deinitDecoderCapturePlane(), and
getNvColorFormatFromV4l2Format() to a dedicated file. No behavioral
change."
```

---

### Task 5: Rename residual file and update CMakeLists.txt

**Agent:** cavecrew-builder (haiku)

**Files:**
- Rename: `src/nvmpi_dec.cpp` → `src/nvmpi_dec_api.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Rename the file**

```bash
git mv src/nvmpi_dec.cpp src/nvmpi_dec_api.cpp
```

- [ ] **Step 2: Update `CMakeLists.txt` source list**

Replace the `nvmpi_dec.cpp` entry in `NVMPI_SRC` (line 86):

Old:
```cmake
set(NVMPI_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nvmpi_dec.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nvmpi_enc.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/NVMPI_frameBuf.cpp)
```

New:
```cmake
set(NVMPI_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nvmpi_dec_api.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nvmpi_dec_capture.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nvmpi_dec_planes.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nvmpi_enc.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/NVMPI_frameBuf.cpp)
```

- [ ] **Step 3: Verify build**

```bash
./scripts/build.sh --stubs --clean
```

- [ ] **Step 4: Commit**

```bash
git add src/nvmpi_dec_api.cpp CMakeLists.txt
git commit -m "refactor(nvmpi): rename nvmpi_dec.cpp to nvmpi_dec_api.cpp

Update CMakeLists.txt to list all three split decoder source files.
No behavioral change."
```

---

### Task 6: Write `docs/ARCHITECTURE.md`

**Agent:** general-purpose (sonnet)

**Files:**
- Create: `docs/ARCHITECTURE.md`

- [ ] **Step 1: Create `docs/ARCHITECTURE.md`**

```markdown
# Architecture

## Modular File Structure

libnvmpi source files follow a naming convention that keeps each file focused
on one concern. The convention applies to both the decoder and encoder.

### Naming pattern

`nvmpi_{codec}_{concern}.cpp` where `{codec}` is `dec` or `enc`:

| Concern | Suffix | Decoder | Encoder (future) |
|---------|--------|---------|------------------|
| Public API | `_api` | `nvmpi_dec_api.cpp` | `nvmpi_enc_api.cpp` |
| Capture/output loop | `_capture` / `_output` | `nvmpi_dec_capture.cpp` | `nvmpi_enc_output.cpp` |
| V4L2 plane management | `_planes` | `nvmpi_dec_planes.cpp` | `nvmpi_enc_planes.cpp` |
| Internal header | `_internal.h` | `nvmpi_dec_internal.h` | `nvmpi_enc_internal.h` |

Shared infrastructure:

| File | Contents |
|------|----------|
| `NVMPI_bufPool.hpp` | Generic thread-safe producer/consumer buffer pool (header-only template) |
| `NVMPI_frameBuf.hpp` / `.cpp` | DMA buffer allocation and destruction |

### Rules

1. **Internal headers** (`*_internal.h`) hold the context struct (`nvmpictx`)
   and internal forward declarations. Located in `src/`, NOT installed.
2. **Public API** (`include/nvmpi.h`) is the only installed header — no ABI
   break from internal refactors.
3. Each `.cpp` file must be independently compilable — no circular includes.
4. `CMakeLists.txt` lists source files explicitly (no globs).
5. **When to split further:** a file exceeds ~500 lines or gains a new logical
   concern (e.g., a new thread, a new V4L2 device interaction).

### Include structure

```
include/nvmpi.h          <- public, installed
include/NVMPI_bufPool.hpp <- public (used by both dec and enc)
include/NVMPI_frameBuf.hpp
src/nvmpi_dec_internal.h  <- private, includes all of the above + NVIDIA headers
src/nvmpi_dec_api.cpp     <- includes nvmpi_dec_internal.h
src/nvmpi_dec_capture.cpp <- includes nvmpi_dec_internal.h
src/nvmpi_dec_planes.cpp  <- includes nvmpi_dec_internal.h
```

### Applying to the encoder

When encoder issues grow it beyond its current single-file size, apply the
same split:

1. Create `src/nvmpi_enc_internal.h` with the encoder's context struct.
2. Extract the capture callback to `src/nvmpi_enc_output.cpp`.
3. Extract plane setup to `src/nvmpi_enc_planes.cpp`.
4. Rename `src/nvmpi_enc.cpp` to `src/nvmpi_enc_api.cpp`.
5. Update `CMakeLists.txt`.

Use the same `_api`, `_output`/`_capture`, `_planes`, `_internal.h` suffixes.
```

- [ ] **Step 2: Commit**

```bash
git add docs/ARCHITECTURE.md
git commit -m "docs: add ARCHITECTURE.md with modular split convention

Documents the nvmpi_{codec}_{concern} file naming pattern, include
structure, split rules, and encoder migration path."
```

---

### Task 7: Update `docs/DEVELOPMENT.md`

**Agent:** cavecrew-builder (haiku)

**Files:**
- Modify: `docs/DEVELOPMENT.md`

- [ ] **Step 1: Add a "Modular File Structure" section**

Find an appropriate location (after existing architecture/overview content) and add:

```markdown
## Modular File Structure

Source files follow `nvmpi_{codec}_{concern}.cpp` — see
[ARCHITECTURE.md](ARCHITECTURE.md) for the full convention, include
structure, and encoder migration path.

Current decoder files:

| File | Concern |
|------|---------|
| `src/nvmpi_dec_api.cpp` | Public API (`create`, `put_packet`, `get_frame`, `flush`, `close`) |
| `src/nvmpi_dec_capture.cpp` | Capture thread loop, resolution-change handler |
| `src/nvmpi_dec_planes.cpp` | V4L2 CAPTURE-plane alloc/teardown, color format selection |
| `src/nvmpi_dec_internal.h` | Context struct, macros, forward declarations |

When adding a new feature:
1. Identify which concern it touches (API? capture loop? planes?).
2. Edit only that file. If it doesn't fit any existing concern, create a new
   `nvmpi_dec_{concern}.cpp` and add it to `CMakeLists.txt`.
3. Never put new public API signatures outside `include/nvmpi.h`.
```

- [ ] **Step 2: Commit**

```bash
git add docs/DEVELOPMENT.md
git commit -m "docs: update DEVELOPMENT.md with modular file structure

Cross-references ARCHITECTURE.md and lists current decoder file layout."
```

---

### Task 8: Build verification

- [ ] **Step 1: Full stubs build**

```bash
./scripts/build.sh --stubs --clean
```

Expected: clean build, no warnings from `src/nvmpi_dec_*.cpp`.

- [ ] **Step 2: Verify file layout**

```bash
ls -la src/nvmpi_dec_*.cpp src/nvmpi_dec_internal.h
```

Expected:
```
src/nvmpi_dec_api.cpp
src/nvmpi_dec_capture.cpp
src/nvmpi_dec_internal.h
src/nvmpi_dec_planes.cpp
```

- [ ] **Step 3: Verify commit history**

```bash
git log --oneline refactor/10-decoder-split ^main
```

Expected: 6 commits, all `refactor(nvmpi)` or `docs:`. No `feat` or `test` commits.

---

### Task 9: Push, create MR, set auto-merge

- [ ] **Step 1: Verify auth**

```bash
glab auth status
```

- [ ] **Step 2: Push refactor branch**

```bash
git push -u origin refactor/10-decoder-split
```

- [ ] **Step 3: Create MR**

```bash
glab mr create --title "refactor(nvmpi): split nvmpi_dec.cpp into modular files" --description "$(cat <<'EOF'
## Summary

Mechanical file split of `nvmpi_dec.cpp` (~1040 lines) into four files
by concern, establishing the modular structure for issue #10 and future
decoder work.

- `src/nvmpi_dec_internal.h` — context struct, macros, forward decls
- `src/nvmpi_dec_capture.cpp` — capture thread loop, resolution-change handler
- `src/nvmpi_dec_planes.cpp` — V4L2 plane alloc/teardown, color format selection
- `src/nvmpi_dec_api.cpp` — public API functions + helpers

Also adds `docs/ARCHITECTURE.md` documenting the split convention
(applies to encoder too when it grows).

No behavioral changes. Pure code movement.

Ref #10
EOF
)" --target-branch main
```

- [ ] **Step 4: Wait for pipeline, then set auto-merge**

After the MR is created, wait ~30s for the pipeline to be created, then:

```bash
MR_ID=$(glab mr list --state opened --head refactor/10-decoder-split --json iid -q | head -1)
# Wait for pipeline to exist
sleep 30
glab mr merge "$MR_ID" --auto-merge
```

- [ ] **Step 5: Wait for merge to complete**

Monitor the MR until it merges. Do NOT proceed to Branch 2 until this is merged.

```bash
# Check MR state
glab mr view "$MR_ID" --json state -q
```

---

# BRANCH 2: feat/10-decoder-blocking-wait

**Prerequisites:** Branch 1 (`refactor/10-decoder-split`) MUST be merged to main. Pull main first.

---

### Task 10: Create feature branch

- [ ] **Step 1: Update main and create branch**

```bash
git checkout main
git pull
git checkout -b feat/10-decoder-blocking-wait main
```

---

### Task 11: Write unit tests for NVMPI_bufPool (TDD red phase)

**Agent:** general-purpose (sonnet)

**Files:**
- Create: `test/unit/test_bufpool.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Create `test/unit/test_bufpool.cpp`**

```cpp
#include "NVMPI_bufPool.hpp"
#include <cassert>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>
#include <iostream>
#include <cmath>

using Clock = std::chrono::steady_clock;
using Ms = std::chrono::milliseconds;

static int tests_run = 0;
static int tests_passed = 0;

#define RUN_TEST(fn) do { \
    tests_run++; \
    std::cout << "  " #fn "... " << std::flush; \
    fn(); \
    tests_passed++; \
    std::cout << "OK" << std::endl; \
} while(0)

template<typename F>
long elapsed_ms(F&& fn) {
    auto t0 = Clock::now();
    fn();
    auto t1 = Clock::now();
    return std::chrono::duration_cast<Ms>(t1 - t0).count();
}

static void test_dqFilledBuf_nonblocking_unchanged() {
    NVMPI_bufPool<int*> pool;
    auto ms = elapsed_ms([&]{ assert(pool.dqFilledBuf() == nullptr); });
    assert(ms < 50);
    int val = 42;
    pool.qFilledBuf(&val);
    int* out = pool.dqFilledBuf();
    assert(out == &val);
    assert(pool.dqFilledBuf() == nullptr);
}

static void test_dqFilledBuf_blocking_returns_on_push() {
    NVMPI_bufPool<int*> pool;
    int val = 99;
    int* result = nullptr;
    std::thread consumer([&]{
        result = pool.dqFilledBuf(Ms(2000));
    });
    std::this_thread::sleep_for(Ms(100));
    pool.qFilledBuf(&val);
    consumer.join();
    assert(result == &val);
}

static void test_dqFilledBuf_blocking_returns_null_on_timeout() {
    NVMPI_bufPool<int*> pool;
    int* result = reinterpret_cast<int*>(1);
    auto ms = elapsed_ms([&]{
        result = pool.dqFilledBuf(Ms(200));
    });
    assert(result == nullptr);
    assert(ms >= 150 && ms <= 400);
}

static void test_dqFilledBuf_blocking_returns_null_on_shutdown() {
    NVMPI_bufPool<int*> pool;
    int* result = reinterpret_cast<int*>(1);
    std::thread consumer([&]{
        result = pool.dqFilledBuf(Ms(5000));
    });
    std::this_thread::sleep_for(Ms(100));
    pool.shutdown();
    consumer.join();
    assert(result == nullptr);
}

static void test_shutdown_wakes_all_waiters() {
    NVMPI_bufPool<int*> pool;
    constexpr int N = 3;
    std::atomic<int> woke{0};
    std::vector<std::thread> consumers;
    for (int i = 0; i < N; i++) {
        consumers.emplace_back([&]{
            pool.dqFilledBuf(Ms(5000));
            woke.fetch_add(1);
        });
    }
    std::this_thread::sleep_for(Ms(100));
    pool.shutdown();
    std::this_thread::sleep_for(Ms(200));
    assert(woke.load() == N);
    for (auto& t : consumers) t.join();
}

static void test_reset_clears_shutdown() {
    NVMPI_bufPool<int*> pool;
    int val = 77;
    pool.shutdown();
    pool.reset();
    int* result = nullptr;
    std::thread consumer([&]{
        result = pool.dqFilledBuf(Ms(2000));
    });
    std::this_thread::sleep_for(Ms(100));
    pool.qFilledBuf(&val);
    consumer.join();
    assert(result == &val);
}

static void test_qFilledBuf_notifies_one_waiter() {
    NVMPI_bufPool<int*> pool;
    int val = 55;
    std::atomic<int> got_item{0};
    std::thread c1([&]{ if(pool.dqFilledBuf(Ms(1000))) got_item.fetch_add(1); });
    std::thread c2([&]{ if(pool.dqFilledBuf(Ms(1000))) got_item.fetch_add(1); });
    std::this_thread::sleep_for(Ms(100));
    pool.qFilledBuf(&val);
    std::this_thread::sleep_for(Ms(200));
    pool.shutdown();
    c1.join();
    c2.join();
    assert(got_item.load() == 1);
}

static void test_concurrent_push_pop_stress() {
    NVMPI_bufPool<int*> pool;
    constexpr int ITEMS_PER_THREAD = 10000;
    constexpr int NUM_PRODUCERS = 4;
    constexpr int NUM_CONSUMERS = 4;
    std::vector<int> items(NUM_PRODUCERS * ITEMS_PER_THREAD, 1);
    std::atomic<int> produced{0};
    std::atomic<int> consumed{0};
    std::atomic<bool> done{false};

    std::vector<std::thread> producers;
    for (int p = 0; p < NUM_PRODUCERS; p++) {
        producers.emplace_back([&, p]{
            for (int i = 0; i < ITEMS_PER_THREAD; i++) {
                pool.qFilledBuf(&items[p * ITEMS_PER_THREAD + i]);
                produced.fetch_add(1);
            }
        });
    }

    std::vector<std::thread> consumers;
    for (int c = 0; c < NUM_CONSUMERS; c++) {
        consumers.emplace_back([&]{
            while (!done.load()) {
                int* item = pool.dqFilledBuf(Ms(100));
                if (item) consumed.fetch_add(1);
            }
            while (int* item = pool.dqFilledBuf()) {
                (void)item;
                consumed.fetch_add(1);
            }
        });
    }

    for (auto& t : producers) t.join();
    std::this_thread::sleep_for(Ms(500));
    done.store(true);
    pool.shutdown();
    for (auto& t : consumers) t.join();

    int total = NUM_PRODUCERS * ITEMS_PER_THREAD;
    assert(produced.load() == total);
    assert(consumed.load() == total);
}

int main() {
    std::cout << "=== NVMPI_bufPool unit tests ===" << std::endl;
    RUN_TEST(test_dqFilledBuf_nonblocking_unchanged);
    RUN_TEST(test_dqFilledBuf_blocking_returns_on_push);
    RUN_TEST(test_dqFilledBuf_blocking_returns_null_on_timeout);
    RUN_TEST(test_dqFilledBuf_blocking_returns_null_on_shutdown);
    RUN_TEST(test_shutdown_wakes_all_waiters);
    RUN_TEST(test_reset_clears_shutdown);
    RUN_TEST(test_qFilledBuf_notifies_one_waiter);
    RUN_TEST(test_concurrent_push_pop_stress);
    std::cout << std::endl;
    std::cout << tests_passed << "/" << tests_run << " tests passed." << std::endl;
    return (tests_passed == tests_run) ? 0 : 1;
}
```

- [ ] **Step 2: Add CMake test target**

Add to end of `CMakeLists.txt`:

```cmake
include(CTest)
if(BUILD_TESTING)
  add_executable(test_bufpool test/unit/test_bufpool.cpp)
  target_include_directories(test_bufpool PRIVATE ${NVMPI_INCLUDE_DIR})
  target_link_libraries(test_bufpool PRIVATE ${CMAKE_THREAD_LIBS_INIT})
  add_test(NAME bufpool COMMAND test_bufpool)
endif()
```

- [ ] **Step 3: Verify tests FAIL (TDD red phase)**

```bash
mkdir -p build && cd build && cmake -DBUILD_TESTING=ON .. && make test_bufpool 2>&1
```

Expected: compile error — `dqFilledBuf(Ms)` overload doesn't exist yet.

- [ ] **Step 4: Commit**

```bash
git add test/unit/test_bufpool.cpp CMakeLists.txt
git commit -m "test(nvmpi): add unit tests for NVMPI_bufPool blocking dequeue

TDD red phase: 8 test cases for dqFilledBuf(timeout), shutdown(), and
reset(). All fail because the blocking overload does not exist yet."
```

---

### Task 12: Write hardware test suites (TDD)

**Agent:** general-purpose (sonnet) for blocking-wait + perf; cavecrew-builder (haiku) for lifecycle

**Files:**
- Create: `test/hw-decoder-blocking-wait.sh`
- Create: `test/hw-decoder-lifecycle.sh`
- Create: `test/hw-perf-blocking-wait.sh`

- [ ] **Step 1: Create `test/hw-decoder-blocking-wait.sh`**

```bash
#!/usr/bin/env bash
# Decoder blocking-wait suite: validates that -flags low_delay activates
# true blocking wait in nvmpi_decoder_get_frame() (issue #10).
set -eu
# shellcheck source=test/gen-samples.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/gen-samples.sh"

variant="${JETSON_VARIANT:-unknown}"
echo "=== hw-decoder-blocking-wait on variant: ${variant} ==="

[ -s "${SAMPLE_H264_720P}" ] || gen-sample-h264 "${SAMPLE_H264_720P}" 3

expected_frames() {
  ffprobe -v error -select_streams v:0 -count_frames \
    -show_entries stream=nb_read_frames -of csv=p=0 "$1" 2>/dev/null || echo 0
}

echo "== 1. low_delay frames arrive (H.264) =="
rc=0
out=$(timeout -k 5 30 ffmpeg -y -hide_banner \
  -flags low_delay -c:v h264_nvmpi -i "${SAMPLE_H264_720P}" \
  -f null - 2>&1) || rc=$?
if [ "$rc" -eq 124 ]; then
  echo "FAIL: low_delay decode timed out (30s) — possible deadlock."
  exit 1
fi
if [ "$rc" -ge 128 ]; then
  echo "FAIL: low_delay decode crashed (signal $((rc-128)))."
  echo "$out" | tail -15
  exit 1
fi
frames=$(echo "$out" | grep -oP 'frame=\s*\K[0-9]+' | tail -1)
exp=$(expected_frames "${SAMPLE_H264_720P}")
if [ -z "$frames" ] || [ "$frames" -lt 1 ]; then
  echo "FAIL: no frames decoded with -flags low_delay."
  echo "$out" | tail -15
  exit 1
fi
echo "   decoded ${frames} frames (expected ~${exp}) — OK"

echo "== 2. low_delay no hang on EOS =="
rc=0
timeout -k 5 30 ffmpeg -y -hide_banner -loglevel error \
  -flags low_delay -c:v h264_nvmpi -i "${SAMPLE_H264_720P}" \
  -f null - 2>/dev/null || rc=$?
if [ "$rc" -eq 124 ]; then
  echo "FAIL: EOS hang with -flags low_delay."
  exit 1
fi
echo "   EOS exit clean (rc=${rc}) — OK"

echo "== 3. low_delay vs normal first-frame latency =="
measure_first_frame() {
  local flags="$1" start_ns end_ns
  start_ns=$(date +%s%N)
  timeout -k 5 15 ffmpeg -y -hide_banner $flags \
    -c:v h264_nvmpi -i "${SAMPLE_H264_720P}" \
    -frames:v 1 -f null - 2>/dev/null || true
  end_ns=$(date +%s%N)
  echo $(( (end_ns - start_ns) / 1000000 ))
}
latency_normal=$(measure_first_frame "")
latency_lowdelay=$(measure_first_frame "-flags low_delay")
echo "   normal: ${latency_normal}ms, low_delay: ${latency_lowdelay}ms"
if [ "$latency_lowdelay" -gt "$((latency_normal + 500))" ]; then
  echo "FAIL: low_delay latency significantly worse than normal."
  exit 1
fi
echo "   latency comparison OK"

echo "== 4. wait_timeout AVOption =="
rc=0
out=$(timeout -k 5 30 ffmpeg -y -hide_banner -loglevel error \
  -flags low_delay -wait_timeout 100 -c:v h264_nvmpi \
  -i "${SAMPLE_H264_720P}" -f null - 2>&1) || rc=$?
if [ "$rc" -eq 124 ]; then
  echo "FAIL: decode with wait_timeout=100 timed out."
  exit 1
fi
if echo "$out" | grep -qi "unrecognized option\|no such option"; then
  echo "FAIL: wait_timeout AVOption not recognized."
  echo "$out"
  exit 1
fi
echo "   wait_timeout=100 accepted — OK"

echo "== 5. blocking wait HEVC =="
if ffmpeg -hide_banner -encoders 2>/dev/null | grep -q libx265; then
  [ -s "${SAMPLE_HEVC_720P}" ] || gen-sample-hevc "${SAMPLE_HEVC_720P}" 3
  rc=0
  timeout -k 5 30 ffmpeg -y -hide_banner -loglevel error \
    -flags low_delay -c:v hevc_nvmpi -i "${SAMPLE_HEVC_720P}" \
    -f null - 2>/dev/null || rc=$?
  if [ "$rc" -eq 124 ]; then
    echo "FAIL: HEVC low_delay decode timed out."
    exit 1
  fi
  echo "   HEVC low_delay OK (rc=${rc})"
else
  echo "   skipped (libx265 not available)"
fi

echo "== 6. rapid open/close under blocking mode =="
for i in $(seq 1 20); do
  rc=0
  timeout -k 5 10 ffmpeg -y -hide_banner -loglevel error \
    -flags low_delay -c:v h264_nvmpi -i "${SAMPLE_H264_720P}" \
    -frames:v 5 -f null - 2>/dev/null || rc=$?
  if [ "$rc" -ge 128 ]; then
    echo "FAIL: crash on iteration ${i} (signal $((rc-128)))."
    exit 1
  fi
  if [ "$rc" -eq 124 ]; then
    echo "FAIL: hang on iteration ${i}."
    exit 1
  fi
done
echo "   20 rapid open/close cycles — OK"

echo "OK: hw-decoder-blocking-wait passed on ${variant}."
```

- [ ] **Step 2: Create `test/hw-decoder-lifecycle.sh`**

```bash
#!/usr/bin/env bash
# Decoder lifecycle suite: guards that blocking-wait changes did not break
# normal (non-low-delay) decode paths.
set -eu
# shellcheck source=test/gen-samples.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/gen-samples.sh"

variant="${JETSON_VARIANT:-unknown}"
echo "=== hw-decoder-lifecycle on variant: ${variant} ==="

[ -s "${SAMPLE_H264_720P}" ] || gen-sample-h264 "${SAMPLE_H264_720P}" 3

echo "== 1. normal decode still works (no low_delay) =="
rc=0
out=$(timeout -k 5 30 ffmpeg -y -hide_banner \
  -c:v h264_nvmpi -i "${SAMPLE_H264_720P}" \
  -f null - 2>&1) || rc=$?
if [ "$rc" -eq 124 ]; then
  echo "FAIL: normal decode timed out."
  exit 1
fi
if [ "$rc" -ge 128 ]; then
  echo "FAIL: normal decode crashed (signal $((rc-128)))."
  echo "$out" | tail -15
  exit 1
fi
frames=$(echo "$out" | grep -oP 'frame=\s*\K[0-9]+' | tail -1)
if [ -z "$frames" ] || [ "$frames" -lt 10 ]; then
  echo "FAIL: expected >= 10 frames, got ${frames:-0}."
  echo "$out" | tail -15
  exit 1
fi
echo "   normal decode: ${frames} frames — OK"

echo "== 2. flush and reuse (seek mid-stream) =="
SAMPLE_SEEK="/tmp/nvmpi-sample-h264-lifecycle-seek.mp4"
[ -s "${SAMPLE_SEEK}" ] || ffmpeg -y -hide_banner -loglevel error \
  -f lavfi -i testsrc2=s=1280x720:r=30 -t 5 \
  -c:v libx264 -preset fast -g 30 "${SAMPLE_SEEK}"
rc=0
out=$(timeout -k 5 30 ffmpeg -y -hide_banner \
  -ss 2 -c:v h264_nvmpi -i "${SAMPLE_SEEK}" \
  -f null - 2>&1) || rc=$?
if [ "$rc" -eq 124 ]; then
  echo "FAIL: seek+decode timed out — flush may be broken."
  exit 1
fi
frames=$(echo "$out" | grep -oP 'frame=\s*\K[0-9]+' | tail -1)
if [ -z "$frames" ] || [ "$frames" -lt 10 ]; then
  echo "FAIL: expected >= 10 frames after seek, got ${frames:-0}."
  exit 1
fi
echo "   flush+reuse: ${frames} frames after seek — OK"

echo "== 3. EOS no hang (short file) =="
SAMPLE_SHORT="/tmp/nvmpi-sample-h264-short.mp4"
[ -s "${SAMPLE_SHORT}" ] || ffmpeg -y -hide_banner -loglevel error \
  -f lavfi -i testsrc2=s=1280x720:r=30 -t 0.3 \
  -c:v libx264 "${SAMPLE_SHORT}"
rc=0
timeout -k 5 15 ffmpeg -y -hide_banner -loglevel error \
  -c:v h264_nvmpi -i "${SAMPLE_SHORT}" \
  -f null - 2>/dev/null || rc=$?
if [ "$rc" -eq 124 ]; then
  echo "FAIL: short-file decode timed out — EOS not signalled."
  exit 1
fi
echo "   short-file EOS OK (rc=${rc})"

echo "OK: hw-decoder-lifecycle passed on ${variant}."
```

- [ ] **Step 3: Create `test/hw-perf-blocking-wait.sh`**

```bash
#!/usr/bin/env bash
# Performance measurement for blocking wait (issue #10).
# NON-FATAL: reports metrics, does not gate pass/fail.
set -eu
# shellcheck source=test/gen-samples.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/gen-samples.sh"

variant="${JETSON_VARIANT:-unknown}"
echo "=== hw-perf-blocking-wait on variant: ${variant} ==="

SAMPLE_PERF="/tmp/nvmpi-sample-perf-blocking.mp4"
[ -s "${SAMPLE_PERF}" ] || gen-sample-h264-long "${SAMPLE_PERF}" 10

echo ""
echo "== Metric 1: First-frame latency =="
measure_first_frame_latency() {
  local flags="$1" start_ns end_ns
  start_ns=$(date +%s%N)
  timeout -k 5 15 ffmpeg -y -hide_banner -loglevel error $flags \
    -c:v h264_nvmpi -i "${SAMPLE_PERF}" \
    -frames:v 1 -f null - 2>/dev/null || true
  end_ns=$(date +%s%N)
  echo $(( (end_ns - start_ns) / 1000000 ))
}
sum_normal=0; sum_lowdelay=0
for run in 1 2 3; do
  lat_n=$(measure_first_frame_latency "")
  lat_l=$(measure_first_frame_latency "-flags low_delay")
  sum_normal=$((sum_normal + lat_n))
  sum_lowdelay=$((sum_lowdelay + lat_l))
done
avg_normal=$((sum_normal / 3))
avg_lowdelay=$((sum_lowdelay / 3))
if [ "$avg_lowdelay" -gt 0 ]; then
  improvement=$(echo "$avg_normal $avg_lowdelay" | awk '{printf "%.1fx", $1/$2}')
else
  improvement="N/A"
fi
echo "PERF: first_frame_latency_normal=${avg_normal}ms"
echo "PERF: first_frame_latency_lowdelay=${avg_lowdelay}ms"
echo "PERF: first_frame_improvement=${improvement}"

echo ""
echo "== Metric 2: CPU usage =="
measure_cpu() {
  local flags="$1" cpu
  cpu=$( { /usr/bin/time -v timeout -k 5 30 ffmpeg -y -hide_banner -loglevel error $flags \
    -c:v h264_nvmpi -i "${SAMPLE_PERF}" \
    -f null - ; } 2>&1 | grep "Percent of CPU" | grep -oP '[0-9]+' || echo "N/A" )
  echo "$cpu"
}
cpu_normal=$(measure_cpu "")
cpu_lowdelay=$(measure_cpu "-flags low_delay")
echo "PERF: cpu_percent_normal=${cpu_normal}%"
echo "PERF: cpu_percent_lowdelay=${cpu_lowdelay}%"

echo ""
echo "== Metric 3: Decode throughput (fps) =="
measure_fps() {
  local flags="$1" stderr="/tmp/nvmpi-perf-bw-$2.log"
  ffmpeg -y -hide_banner -benchmark $flags \
    -c:v h264_nvmpi -i "${SAMPLE_PERF}" \
    -f null - 2>"$stderr" || true
  local speed
  speed=$(grep -oP 'speed=\s*\K[0-9.]+' "$stderr" | tail -1)
  if [ -n "$speed" ]; then
    echo "$speed 30" | awk '{printf "%.1f", $1 * $2}'
  else
    echo "N/A"
  fi
}
fps_normal=$(measure_fps "" "normal")
fps_lowdelay=$(measure_fps "-flags low_delay" "lowdelay")
echo "PERF: fps_normal=${fps_normal}"
echo "PERF: fps_lowdelay=${fps_lowdelay}"

echo ""
echo "========================================"
echo "  BLOCKING WAIT PERF SUMMARY (${variant})"
echo "========================================"
echo "  First-frame latency: normal=${avg_normal}ms, low_delay=${avg_lowdelay}ms (${improvement})"
echo "  CPU usage:           normal=${cpu_normal}%, low_delay=${cpu_lowdelay}%"
echo "  Throughput:          normal=${fps_normal} fps, low_delay=${fps_lowdelay} fps"
echo ""
echo "OK: hw-perf-blocking-wait completed on ${variant}."
```

- [ ] **Step 4: Make test scripts executable**

```bash
chmod +x test/hw-decoder-blocking-wait.sh test/hw-decoder-lifecycle.sh test/hw-perf-blocking-wait.sh
```

- [ ] **Step 5: Commit**

```bash
git add test/hw-decoder-blocking-wait.sh test/hw-decoder-lifecycle.sh test/hw-perf-blocking-wait.sh
git commit -m "test(nvmpi): add hw test suites for blocking wait, lifecycle, and perf

TDD red phase (on-Jetson): hw-decoder-blocking-wait (6 tests),
hw-decoder-lifecycle (3 tests), hw-perf-blocking-wait (metrics only).
All fail until the feature is implemented."
```

---

### Task 13: Implement NVMPI_bufPool condition-variable support

**Agent:** main context (opus) — thread-safety critical

**Files:**
- Modify: `include/NVMPI_bufPool.hpp`

- [ ] **Step 1: Replace `include/NVMPI_bufPool.hpp`**

Full replacement — adds `std::condition_variable cv_filledBuf`, `std::atomic<bool> m_shutdown`, blocking `dqFilledBuf(timeout)` overload, `shutdown()`, and `reset()`. Existing non-blocking API preserved unchanged.

```cpp
#pragma once
#include <queue>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <atomic>

template <typename T>
struct NVMPI_bufPool
{
	std::mutex m_emptyBuf;
	std::mutex m_filledBuf;
	std::condition_variable cv_filledBuf;
	std::atomic<bool> m_shutdown{false};
	std::queue<T> emptyBuf;
	std::queue<T> filledBuf;

	T dqEmptyBuf();
	void qEmptyBuf(T buf);
	T peekEmptyBuf();

	T dqFilledBuf();
	T dqFilledBuf(std::chrono::milliseconds timeout);
	void qFilledBuf(T buf);

	void shutdown();
	void reset();
};

template<typename T>
T NVMPI_bufPool<T>::dqEmptyBuf()
{
	T buf = NULL;
	m_emptyBuf.lock();
	if(!emptyBuf.empty())
	{
		buf = emptyBuf.front();
		emptyBuf.pop();
	}
	m_emptyBuf.unlock();
	return buf;
}

template<typename T>
T NVMPI_bufPool<T>::peekEmptyBuf()
{
	T buf = NULL;
	m_emptyBuf.lock();
	if(!emptyBuf.empty())
	{
		buf = emptyBuf.front();
	}
	m_emptyBuf.unlock();
	return buf;
}

template<typename T>
T NVMPI_bufPool<T>::dqFilledBuf()
{
	T buf = NULL;
	m_filledBuf.lock();
	if(!filledBuf.empty())
	{
		buf = filledBuf.front();
		filledBuf.pop();
	}
	m_filledBuf.unlock();
	return buf;
}

template<typename T>
T NVMPI_bufPool<T>::dqFilledBuf(std::chrono::milliseconds timeout)
{
	using Clock = std::chrono::steady_clock;
	constexpr auto GRANULARITY = std::chrono::milliseconds(100);
	auto deadline = Clock::now() + timeout;

	std::unique_lock<std::mutex> lock(m_filledBuf);
	while (true)
	{
		if (!filledBuf.empty())
		{
			T buf = filledBuf.front();
			filledBuf.pop();
			return buf;
		}
		if (m_shutdown.load(std::memory_order_acquire))
			return NULL;

		auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
			deadline - Clock::now());
		if (remaining <= std::chrono::milliseconds(0))
			return NULL;

		auto wait_time = std::min(remaining, GRANULARITY);
		cv_filledBuf.wait_for(lock, wait_time);
	}
}

template<typename T>
void NVMPI_bufPool<T>::qEmptyBuf(T buf)
{
	m_emptyBuf.lock();
	emptyBuf.push(buf);
	m_emptyBuf.unlock();
}

template<typename T>
void NVMPI_bufPool<T>::qFilledBuf(T buf)
{
	{
		std::lock_guard<std::mutex> lock(m_filledBuf);
		filledBuf.push(buf);
	}
	cv_filledBuf.notify_one();
}

template<typename T>
void NVMPI_bufPool<T>::shutdown()
{
	{
		std::lock_guard<std::mutex> lock(m_filledBuf);
		m_shutdown.store(true, std::memory_order_release);
	}
	cv_filledBuf.notify_all();
}

template<typename T>
void NVMPI_bufPool<T>::reset()
{
	std::lock_guard<std::mutex> lock(m_filledBuf);
	m_shutdown.store(false, std::memory_order_release);
}
```

- [ ] **Step 2: Verify unit tests PASS**

```bash
cd build && cmake -DBUILD_TESTING=ON .. && make test_bufpool && ./test_bufpool
```

Expected: 8/8 tests passed.

- [ ] **Step 3: Verify library build**

```bash
cd /workspace && ./scripts/build.sh --stubs --clean
```

- [ ] **Step 4: Commit**

```bash
git add include/NVMPI_bufPool.hpp
git commit -m "feat(nvmpi): add condition-variable blocking dequeue to NVMPI_bufPool

Add dqFilledBuf(timeout) with 100ms internal granularity, shutdown()
to wake all blocked consumers, and reset() for flush/restart.
Existing non-blocking API unchanged."
```

---

### Task 14: Make `ctx->eos` atomic and add `wait_timeout_ms`

**Agent:** cavecrew-builder (haiku)

**Files:**
- Modify: `src/nvmpi_dec_internal.h`

- [ ] **Step 1: Change `bool eos` to `std::atomic<bool> eos`**

In `src/nvmpi_dec_internal.h`, replace:
```cpp
	bool eos{false};
```
with:
```cpp
	std::atomic<bool> eos{false};
```

- [ ] **Step 2: Add `wait_timeout_ms` field**

After `bool disable_dpb{false};`:
```cpp
	int wait_timeout_ms{500};
```

- [ ] **Step 3: Verify build**

```bash
./scripts/build.sh --stubs --clean
```

- [ ] **Step 4: Commit**

```bash
git add src/nvmpi_dec_internal.h
git commit -m "feat(nvmpi): make ctx->eos atomic and add wait_timeout_ms

std::atomic<bool> eliminates the data race between user and capture
threads. wait_timeout_ms stores the configurable blocking ceiling."
```

---

### Task 15: Wire shutdown() to capture-loop exit and use atomic eos

**Agent:** main context (opus) — must audit every path

**Files:**
- Modify: `src/nvmpi_dec_capture.cpp`

- [ ] **Step 1: Add `framePool->shutdown()` before the final return**

At the end of `dec_capture_loop_fcn`, just before the final `return;`:

```cpp
	ctx->framePool->shutdown();
	return;
```

- [ ] **Step 2: Update eos access to use atomic load**

In the backpressure spin loop, change `while(!ctx->eos)` to `while(!ctx->eos.load())`. (The `.load()` is explicit for atomics; compiler can optimize this but it documents intent.)

- [ ] **Step 3: Audit all paths**

All exit paths in `dec_capture_loop_fcn` converge at the single `return` statement at the end of the function:
1. `ctx->eos=true` on dqEvent error (line ~583) → loop exits → hits return ✓
2. `ctx->eos=true` on EOS flag (line ~625) → breaks inner loop → outer while checks eos → exits → return ✓
3. `ctx->eos=true` on dqBuffer error (line ~632) → same path ✓
4. Backpressure `while(!ctx->eos)` (line ~666) → if eos set externally → breaks → same ✓

One `shutdown()` call at the return covers all paths.

- [ ] **Step 4: Verify build**

```bash
./scripts/build.sh --stubs --clean
```

- [ ] **Step 5: Commit**

```bash
git add src/nvmpi_dec_capture.cpp
git commit -m "feat(nvmpi): wire shutdown() to capture-loop exit

Call framePool->shutdown() before the capture thread returns, unblocking
any consumer waiting in dqFilledBuf(timeout). Atomic eos load in
backpressure spin."
```

---

### Task 16: Implement blocking wait in get_frame, flush, close

**Agent:** main context (opus) — API contract

**Files:**
- Modify: `src/nvmpi_dec_api.cpp`
- Modify: `include/nvmpi.h`

- [ ] **Step 1: Add `wait_timeout` to `nvDecParam` in `include/nvmpi.h`**

After `int disable_dpb;` in `nvDecParam`:

```c
	int wait_timeout;        //blocking wait timeout in ms (0 = use default 500ms)
```

- [ ] **Step 2: Update `nvmpi_decoder_get_frame` comment in `nvmpi.h`**

Replace the existing comment (lines 153–156):

```c
	//Retrieve one decoded frame. When wait is false: non-blocking, returns
	//-1 immediately if no frame is ready. When wait is true: blocks up to
	//wait_timeout milliseconds (configurable via nvDecParam.wait_timeout,
	//default 500ms). Returns -1 on timeout or shutdown.
```

- [ ] **Step 3: Wire `wait_timeout` in `nvmpi_create_decoder()`**

In `nvmpi_dec_api.cpp`, after `ctx->disable_dpb = param->disable_dpb;` add:

```cpp
	if (param->wait_timeout >= 50 && param->wait_timeout <= 5000)
		ctx->wait_timeout_ms = param->wait_timeout;
	else if (param->wait_timeout != 0)
		std::cerr << "[libnvmpi][W]: wait_timeout " << param->wait_timeout
		          << " out of range [50, 5000]; using default " << ctx->wait_timeout_ms << std::endl;
```

- [ ] **Step 4: Replace `nvmpi_decoder_get_frame` body**

Replace the current function (lines 942–956 original, now at different offset in `nvmpi_dec_api.cpp`):

```cpp
int nvmpi_decoder_get_frame(nvmpictx* ctx, nvFrame* frame, bool wait)
{
	int ret;
	NVMPI_frameBuf* fb;

	if (wait)
		fb = ctx->framePool->dqFilledBuf(
			std::chrono::milliseconds(ctx->wait_timeout_ms));
	else
		fb = ctx->framePool->dqFilledBuf();

	if (!fb) return -1;

	ret = copyNvBufToFrame(ctx, fb, frame);
	frame->timestamp = fb->timestamp;
	ctx->framePool->qEmptyBuf(fb);

	return ret;
}
```

- [ ] **Step 5: Add `framePool->reset()` in flush**

In `nvmpi_decoder_flush()`, after the drain loop (`while ((fb = ...))`) and before `ctx->eos = false;`:

```cpp
	ctx->framePool->reset();
```

- [ ] **Step 6: Add `framePool->shutdown()` in close**

In `nvmpi_decoder_close()`, after `ctx->eos = true;` and before `ctx->dec->capture_plane.setStreamStatus(false);`:

```cpp
	ctx->framePool->shutdown();
```

- [ ] **Step 7: Verify build**

```bash
./scripts/build.sh --stubs --clean
```

- [ ] **Step 8: Commit**

```bash
git add src/nvmpi_dec_api.cpp include/nvmpi.h
git commit -m "feat(nvmpi): implement blocking wait in nvmpi_decoder_get_frame()

When wait=true, get_frame() blocks up to wait_timeout_ms (default 500ms,
configurable via nvDecParam.wait_timeout) using the pool's CV-based
tiered wait. Non-blocking path unchanged.

flush() calls reset() before restarting. close() calls shutdown()
before joining the capture thread."
```

---

### Task 17: Add `wait_timeout` AVOption to FFmpeg decoder

**Agent:** general-purpose (sonnet)

**Files:**
- Modify: `ffmpeg/dev/common/libavcodec/nvmpi_dec.c`

- [ ] **Step 1: Add field to `nvmpiDecodeContext`**

After `int disable_dpb;` (line 69):

```c
	int wait_timeout;
```

- [ ] **Step 2: Add range defines**

After the `OPT_chunk_size_*` defines (line 55):

```c
#define OPT_wait_timeout_MIN 50
#define OPT_wait_timeout_MAX 5000
#define OPT_wait_timeout_AUTO 0
```

- [ ] **Step 3: Wire to `nvDecParam` in `nvmpi_init_decoder()`**

Find the line `param.disable_dpb = nvmpi_context->disable_dpb;` and add after it:

```c
	param.wait_timeout = nvmpi_context->wait_timeout;
```

- [ ] **Step 4: Add to AVOption table**

After the `disable_dpb` option entry (line 397):

```c
    { "wait_timeout", "Blocking wait timeout in milliseconds for low-delay mode (0 = default 500ms)", OFFSET(wait_timeout), AV_OPT_TYPE_INT, {.i64 = OPT_wait_timeout_AUTO }, 0, OPT_wait_timeout_MAX, VD, "wait_timeout" },
```

- [ ] **Step 5: Commit**

```bash
git add ffmpeg/dev/common/libavcodec/nvmpi_dec.c
git commit -m "feat(ffmpeg): add wait_timeout AVOption for low-delay decoder

Exposes libnvmpi's wait_timeout_ms as '-wait_timeout N'. Range 50-5000ms,
default 0 (= libnvmpi default 500ms). Only affects decode when -flags
low_delay is also set."
```

---

### Task 18: Regenerate FFmpeg patches and build-validate

- [ ] **Step 1: Regenerate patches**

```bash
./ffmpeg/dev/update_patch.sh
```

- [ ] **Step 2: Build-validate all 7 versions**

```bash
./ffmpeg/dev/try_build.sh
```

Expected: all 7 FFmpeg versions pass.

- [ ] **Step 3: Commit**

```bash
git add ffmpeg/patches/
git commit -m "chore(ffmpeg-dev): regenerate patches for all FFmpeg versions

Includes wait_timeout AVOption. All 7 versions build-validated."
```

---

### Task 19: Write `docs/THREAD_SAFETY.md`

**Agent:** main context (opus) — must be accurate

**Files:**
- Create: `docs/THREAD_SAFETY.md`

- [ ] **Step 1: Create `docs/THREAD_SAFETY.md`**

```markdown
# Thread Safety Model

## Overview

The libnvmpi decoder uses two threads:

- **User thread** — calls the public API (`create`, `put_packet`, `get_frame`,
  `flush`, `close`). Only one user thread may interact with a decoder context.
- **Capture thread** — runs `dec_capture_loop_fcn()` for the lifetime of the
  context. Dequeues decoded V4L2 buffers, transforms them, and publishes
  frames to the pool.

The two threads communicate through the buffer pool (`NVMPI_bufPool`) and the
atomic `eos` flag.

## Thread Interaction Diagram

```
User thread                     Capture thread
-----------                     --------------
put_packet()                    dec_capture_loop_fcn()
  | memcpy -> V4L2 output          |
  |                                +- dqBuffer (V4L2 capture)
get_frame(wait=true)               +- VIC transform
  |                                +- qFilledBuf() -> notify_one()
  +- dqFilledBuf(timeout)          |
  |   '- cv.wait_for(100ms) <-----'
  |   '- check shutdown
  |   '- re-wait or return         +- on EOS: shutdown() -> notify_all()
  |                                '- return
  |
close()
  +- eos.store(true)
  +- framePool->shutdown()
  +- capture_plane.setStreamStatus(false)
  '- thread.join()
```

## Synchronization Primitives

| Primitive | Type | Location | Protects |
|-----------|------|----------|----------|
| `m_filledBuf` | `std::mutex` | `NVMPI_bufPool` | `filledBuf` queue and `cv_filledBuf` |
| `m_emptyBuf` | `std::mutex` | `NVMPI_bufPool` | `emptyBuf` queue |
| `cv_filledBuf` | `std::condition_variable` | `NVMPI_bufPool` | Blocking wait on filled-buffer availability |
| `m_shutdown` | `std::atomic<bool>` | `NVMPI_bufPool` | Shutdown signal for blocked consumers |
| `eos` | `std::atomic<bool>` | `nvmpictx` | End-of-stream signal between user and capture threads |

## Deadlock Prevention

1. **Single-lock rule:** No function acquires more than one mutex. `shutdown()`
   acquires `m_filledBuf` briefly to set `m_shutdown`, then releases before
   `notify_all()`.

2. **Tiered timeout:** `dqFilledBuf(timeout)` re-checks `m_shutdown` every
   100ms (hardcoded granularity). Even if a `notify_all()` is missed, the
   worst-case wake latency is 100ms.

3. **Shutdown on every exit path:** `dec_capture_loop_fcn()` calls
   `framePool->shutdown()` before returning. This covers: normal EOS,
   dqEvent error, dqBuffer error, and external eos signal.

4. **Close ordering:** `close()` sets `eos`, calls `shutdown()`, then
   `setStreamStatus(false)`, then `join()`. The capture thread will either
   already be exiting (eos) or will exit on the next dqBuffer failure.

## Per-Function Thread Safety

| Function | Thread | Notes |
|----------|--------|-------|
| `nvmpi_create_decoder()` | User | Creates capture thread; no concurrent access yet |
| `nvmpi_decoder_put_packet()` | User | Feeds V4L2 output plane; may block on dqBuffer |
| `nvmpi_decoder_get_frame()` | User | wait=false: non-blocking. wait=true: blocks on CV |
| `nvmpi_decoder_flush()` | User | Joins capture thread, drains pool, restarts |
| `nvmpi_decoder_close()` | User | Joins capture thread, frees everything |
| `dec_capture_loop_fcn()` | Capture | Runs for context lifetime; calls shutdown() on exit |
| `respondToResolutionEvent()` | Capture | Reinits planes and pool |
| `qFilledBuf()` | Capture | Lock + push + notify |
| `dqFilledBuf()` | User | Lock + pop (non-blocking) or CV wait (blocking) |
| `shutdown()` | Either | Lock + set flag + notify_all |
| `reset()` | User only | Called during flush, after thread join |

## Flush/Restart Lifecycle

1. `flush()` sets `eos = true`, stops capture stream, joins thread.
2. Drains filled frames back to empty queue.
3. Calls `framePool->reset()` to clear shutdown flag.
4. Sets `eos = false`, restarts output stream, spawns new capture thread.

The pool's `reset()` must be called between `shutdown()` and the next blocking
dequeue, otherwise the new capture thread's frames would be ignored.
```

- [ ] **Step 2: Commit**

```bash
git add docs/THREAD_SAFETY.md
git commit -m "docs(nvmpi): add THREAD_SAFETY.md

Documents the two-thread model, synchronization primitives, deadlock
prevention, per-function thread safety, and flush/restart lifecycle."
```

---

### Task 20: Write `docs/API_REFERENCE.md`

**Agent:** general-purpose (sonnet)

**Files:**
- Create: `docs/API_REFERENCE.md`

- [ ] **Step 1: Create `docs/API_REFERENCE.md`**

Full documentation for every function in `include/nvmpi.h`: signature, parameters (with types and valid ranges), return values, thread safety, blocking behavior, error conditions. Cover both decoder and encoder API families. Document `nvDecParam` and `nvEncParam` structs field-by-field. Cross-reference `THREAD_SAFETY.md` for the full concurrency model.

Key sections:
- Decoder API: `create`, `put_packet`, `get_frame` (with full wait/timeout semantics), `flush`, `close`
- Encoder API: `create`, `put_frame`, `get_packet`, `dqEmptyPacket`, `qEmptyPacket`, `close`
- Structures: `nvDecParam`, `nvEncParam`, `nvPacket`, `nvFrame`
- Enums: `nvPixFormat`, `nvCodingType`
- Constants: `NVMPI_ENC_CHUNK_SIZE`

- [ ] **Step 2: Commit**

```bash
git add docs/API_REFERENCE.md
git commit -m "docs(nvmpi): add API_REFERENCE.md

Full libnvmpi API documentation: all functions, structures, enums,
thread safety, blocking behavior, and error conditions."
```

---

### Task 21: Update supporting docs and TODO

**Agent:** cavecrew-builder (haiku)

**Files:**
- Modify: `docs/BUILD.md`
- Modify: `test/README.md`
- Modify: `TODO.md`

- [ ] **Step 1: Add unit test instructions to `docs/BUILD.md`**

Add section:

```markdown
## Unit Tests

Unit tests exercise pure-C++ components (no Jetson hardware required).

```bash
mkdir build && cd build
cmake -DBUILD_TESTING=ON ..
make test_bufpool
./test_bufpool
```

Tests run on any architecture (x86, arm64). CI runs them on every push.
```

- [ ] **Step 2: Document new suites in `test/README.md`**

Add entries for `hw-decoder-blocking-wait.sh` (6 tests), `hw-decoder-lifecycle.sh` (3 tests), `hw-perf-blocking-wait.sh` (metrics only, non-fatal). Add a "Unit Tests" section for `test/unit/test_bufpool.cpp`.

- [ ] **Step 3: Remove blocking-wait item from `TODO.md`**

Delete the "Implement blocking `wait` in `nvmpi_decoder_get_frame()`" section.

- [ ] **Step 4: Commit**

```bash
git add docs/BUILD.md test/README.md TODO.md
git commit -m "docs: update BUILD.md, test README, and TODO for blocking wait

Add unit test build instructions, document new hw-* test suites,
remove completed blocking-wait item from TODO."
```

---

### Task 22: Final verification

- [ ] **Step 1: Unit tests**

```bash
cd build && cmake -DBUILD_TESTING=ON .. && make test_bufpool && ./test_bufpool
```

Expected: 8/8 passed.

- [ ] **Step 2: Stubs build**

```bash
cd /workspace && ./scripts/build.sh --stubs --clean
```

- [ ] **Step 3: Build all 7 FFmpeg versions**

```bash
./ffmpeg/dev/try_build.sh
```

Expected: all 7 pass.

- [ ] **Step 4: Full smoke test (on Jetson)**

```bash
./test/smoke-all.sh
```

Expected: 7/7 matrix green.

- [ ] **Step 5: Code review**

Review full diff `git diff main...feat/10-decoder-blocking-wait` for:
- All capture-loop exit paths call `shutdown()`
- Pool `reset()` called in flush before restart
- `close()` calls `shutdown()` before join
- AVOption wiring matches libnvmpi parameter
- Tests executable and follow conventions

---

### Task 23: Push, create MR, set auto-merge

- [ ] **Step 1: Push feature branch**

```bash
git push -u origin feat/10-decoder-blocking-wait
```

- [ ] **Step 2: Create MR**

```bash
glab mr create --title "feat(nvmpi): implement blocking wait in decoder get_frame()" --description "$(cat <<'EOF'
## Summary

Implements true blocking wait in `nvmpi_decoder_get_frame()` when called
with `wait=true` (triggered by FFmpeg's `-flags low_delay`).

- Condition-variable blocking dequeue in `NVMPI_bufPool` with 100ms
  internal granularity and configurable ceiling (`-wait_timeout`, default
  500ms)
- `ctx->eos` promoted to `std::atomic<bool>` (data race fix)
- `shutdown()` on every capture-loop exit path prevents consumer deadlock
- `reset()` in flush enables clean restart
- New AVOption `-wait_timeout` (50-5000ms)
- 8 unit tests for pool blocking/shutdown/reset
- 3 hw test suites (blocking-wait, lifecycle, perf)
- Full docs: THREAD_SAFETY.md, API_REFERENCE.md

Closes #10
EOF
)" --target-branch main
```

- [ ] **Step 3: Wait for pipeline, then set auto-merge**

```bash
MR_ID=$(glab mr list --state opened --head feat/10-decoder-blocking-wait --json iid -q | head -1)
sleep 30
glab mr merge "$MR_ID" --auto-merge
```

---

## Subagent Strategy

| Task | Agent model | Rationale |
|------|-------------|-----------|
| 2–5 (file moves) | haiku | Mechanical copy/paste, no judgment needed |
| 6 (ARCHITECTURE.md) | sonnet | Documentation writing |
| 7 (DEVELOPMENT.md update) | haiku | Small doc edit |
| 8 (build verify) | bash only | No agent needed |
| 9 (push/MR) | bash only | No agent needed |
| 11 (unit tests) | sonnet | Test design requires judgment |
| 12 (hw test suites) | sonnet + haiku | Sonnet for blocking-wait + perf, haiku for lifecycle |
| 13 (pool CV) | **opus** | Thread-safety critical — must be correct |
| 14 (atomic eos) | haiku | Mechanical change |
| 15 (shutdown wiring) | **opus** | Must audit all exit paths |
| 16 (get_frame impl) | **opus** | API contract + flush/close lifecycle |
| 17 (AVOption) | sonnet | FFmpeg integration, pattern-matching |
| 18 (patch regen) | bash only | Script execution |
| 19 (THREAD_SAFETY.md) | **opus** | Must be technically accurate |
| 20 (API_REFERENCE.md) | sonnet | Documentation writing |
| 21 (supporting docs) | haiku | Small edits |
