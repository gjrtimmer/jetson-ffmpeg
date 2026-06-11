# TODO

## Implement blocking `wait` in `nvmpi_decoder_get_frame()`

### Current state

`int nvmpi_decoder_get_frame(nvmpictx* ctx, nvFrame* frame, bool wait)`
(declared in `include/nvmpi.h`, implemented in `src/nvmpi_dec.cpp`) ignores its
`wait` parameter — the body starts with `(void)wait;` and the call is always
non-blocking: it pops a filled buffer from the frame pool and returns `-1`
immediately when none is ready.

The FFmpeg integration already passes a meaningful value:
`ffmpeg/dev/common/libavcodec/nvmpi_dec.c` calls

```c
nvmpi_decoder_get_frame(ctx, &_nvframe, avctx->flags & AV_CODEC_FLAG_LOW_DELAY);
```

so users running `ffmpeg -flags low_delay` are silently *not* getting
low-delay semantics — the decoder still returns "no frame yet" instead of
waiting for the frame belonging to the packet just submitted.

### Intended semantics

- `wait == false` — current behavior, unchanged: non-blocking, `-1` when no
  filled frame is queued.
- `wait == true` — block until one of:
  1. a filled frame becomes available → copy it out, return `0`;
  2. EOS or decoder error is signalled → return `-1` (no frame will ever come);
  3. a bounded timeout expires → return `-1` (see "Timeout" below).

### Implementation sketch

1. **`include/NVMPI_bufPool.hpp`** — the pool is deliberately non-blocking
   ("callers implement their own waiting/polling"). Add an *optional* blocking
   dequeue so the encoder paths are unaffected:
   - add a `std::condition_variable cv_filledBuf` next to `m_filledBuf`
     (switch that path from bare `lock()/unlock()` to
     `std::unique_lock` so the cv can be used);
   - `qFilledBuf()` gains a `cv_filledBuf.notify_one()` after the push;
   - new `T dqFilledBuf(std::chrono::milliseconds timeout)` that waits on the
     cv with a predicate (`!filledBuf.empty() || shutdown`);
   - new `void shutdown()` that sets a `bool shutdown` flag under the filled
     mutex and `notify_all()`s, so blocked consumers wake when no producer
     will ever queue again.

2. **`src/nvmpi_dec.cpp`** — wire shutdown signalling to every place the
   producer (capture thread) stops producing, otherwise a blocked
   `nvmpi_decoder_get_frame(wait=true)` deadlocks:
   - `dec_capture_loop_fcn()` must call `framePool->shutdown()` on exit
     (covers EOS buffer, `V4L2_EVENT_RESOLUTION_CHANGE` error paths, and
     `dec->isInError()`);
   - `nvmpi_decoder_close()` sets `ctx->eos` then joins the capture thread —
     the capture loop exiting (and shutting the pool down) already unblocks a
     waiter, but note close() and get_frame() are expected on the same user
     thread, so close-while-blocked is not the primary hazard; EOS-with-no-
     frames-left is.
   - `ctx->eos` is a plain `bool` written by both user and capture threads;
     while touching this, make it `std::atomic<bool>` (it is currently
     best-effort, see the comment at its declaration).

3. **Timeout** — prefer `cv.wait_for()` with a bounded timeout (e.g.
   100–500 ms, possibly retried) over an indefinite wait. FFmpeg's
   send/receive model tolerates spurious "no frame" returns (`got_frame = 0`),
   but an indefinite block on a stalled/broken V4L2 pipeline would hang the
   whole `ffmpeg` process with no recovery path.

4. **`include/nvmpi.h`** — update the doc comment on
   `nvmpi_decoder_get_frame()` (currently says "the 'wait' flag is currently
   not honoured"). Signature is unchanged, so this is not an ABI/API break for
   FFmpeg builds linked against `libnvmpi.so.1`.

5. **`ffmpeg/dev/common/libavcodec/nvmpi_dec.c`** — no change strictly
   required (it already passes the flag). Re-check the `res < 0` handling at
   the call site still does the right thing when `wait=true` returns `-1` on
   EOS. If anything changes here, regenerate patches with
   `ffmpeg/dev/update_patch.sh` (never hand-edit `ffmpeg/patches/`).

### Risks / things to watch

- **Behavior change for external API users**: any non-FFmpeg consumer of
  `libnvmpi` that has been passing `wait=true` (currently a no-op) will start
  blocking. Worth a release-notes entry and arguably a minor version bump.
- **Deadlock** is the main failure mode: every capture-loop exit path must
  signal the pool. Audit all `ctx->eos = true` sites in `src/nvmpi_dec.cpp`.
- **Encoder symmetry**: `nvmpi_encoder_get_packet()` has a similar
  non-blocking pattern. Out of scope here, but the pool changes (optional
  blocking dequeue) are generic, so the encoder could adopt them later.

### Testing

- `JETSON_VARIANT=<variant> ./test/hw-test.sh` for the basic transcode path.
- Add a low-delay run (`ffmpeg -flags low_delay -c:v h264_nvmpi ...`) and
  verify frames are returned per-packet rather than after pipeline fill.
- `./test/smoke-all.sh` before merging — the change is in libnvmpi, so all
  seven FFmpeg versions exercise it.
- Specifically test stream-end: decode a short file and confirm the process
  exits (no hang) when EOS arrives while `wait=true`.
