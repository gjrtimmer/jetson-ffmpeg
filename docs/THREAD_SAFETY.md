# Thread Safety Model

## Overview

The libnvmpi decoder uses two threads:

- **User thread** — calls the public API (`create`, `put_packet`, `get_frame`,
  `flush`, `close`). Only one user thread may interact with a decoder context.
- **Capture thread** — runs `dec_capture_loop_fcn()` for the lifetime of the
  context. Dequeues decoded V4L2 buffers, transforms them via VIC, and
  publishes frames to the pool.

The two threads communicate through the buffer pool (`NVMPI_bufPool`) and the
atomic `eos` flag.

The encoder follows the same two-thread pattern: a user thread feeds frames
via `put_frame` and reads packets via `get_packet`, while NVIDIA's internal
DQ thread calls back into the packet pool.

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
| `qFilledBuf()` | Capture | Lock + push + notify_one |
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

## Blocking Wait Details

When `nvmpi_decoder_get_frame()` is called with `wait=true`:

1. Calls `dqFilledBuf(milliseconds(wait_timeout_ms))` on the frame pool.
2. Inside, the thread acquires `m_filledBuf` and enters a loop:
   - If `filledBuf` is non-empty, pop and return immediately.
   - If `m_shutdown` is set, return NULL.
   - If deadline has passed, return NULL.
   - Otherwise, `cv.wait_for(min(remaining, 100ms))` — releases the lock
     during the wait and reacquires on wakeup.
3. The 100ms granularity ensures the thread checks `m_shutdown` at least
   10 times per second, regardless of whether `notify_all()` arrived.

The `wait_timeout_ms` defaults to 500ms and is configurable via
`nvDecParam.wait_timeout` (range 50–5000ms, 0 = use default).

## Encoder Threading (Summary)

The encoder uses NVIDIA's built-in DQ thread (`startDQThread()`/`stopDQThread()`):

- User thread calls `put_frame()` to feed raw frames.
- DQ thread dequeues encoded packets from the capture plane and calls
  `encoder_capture_plane_dq_callback()`, which fills packet buffers from
  the pool via `dqEmptyPacket()`/`qFilledPacket()`.
- `close()` calls `stopDQThread()` + `waitForDQThread(1000)` before
  destroying the pool and encoder.

The same `NVMPI_bufPool` template is used for the encoder's packet pool,
with the same mutex/queue semantics but without blocking dequeue (the
encoder wrapper uses non-blocking polling).
