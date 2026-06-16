# libnvmpi API Reference

Public C API defined in `include/nvmpi.h`. Both decoder and encoder expose
opaque context handles (`nvmpictx*`) — a handle must only be passed back to
the API family that created it.

For thread safety details, see [THREAD_SAFETY.md](THREAD_SAFETY.md).

---

## Constants

### `NVMPI_ENC_CHUNK_SIZE`

```c
#define NVMPI_ENC_CHUNK_SIZE 10*1024*1024
```

Maximum size (bytes) of encoded buffers on the encoder capture plane.
Also used as the allocation size of each pooled packet buffer. A single
encoded frame must not exceed this; frames larger than 10 MiB are dropped.

---

## Enums

### `nvPixFormat`

Raw (uncompressed) pixel layouts for decoder output and encoder input.

| Value | Description |
|-------|-------------|
| `NV_PIX_NV12` | 4:2:0 semi-planar (Y + interleaved UV). Default encoder input. |
| `NV_PIX_YUV420` | 4:2:0 planar (Y + U + V separate planes). |
| `NV_PIX_P010` | 10-bit 4:2:0 semi-planar (matches FFmpeg P010LE). Decode-only, HEVC Main10, requires NvUtils buffer API. |

### `nvCodingType`

Compressed bitstream codec selector.

| Value | Codec | Decode | Encode |
|-------|-------|--------|--------|
| `NV_VIDEO_CodingUnused` | None | — | — |
| `NV_VIDEO_CodingH264` | H.264/AVC | Yes | Yes |
| `NV_VIDEO_CodingMPEG4` | MPEG-4 Part 2 | Yes | No |
| `NV_VIDEO_CodingMPEG2` | MPEG-2 | Yes | No |
| `NV_VIDEO_CodingVP8` | VP8 | Yes | No |
| `NV_VIDEO_CodingVP9` | VP9 | Yes | No |
| `NV_VIDEO_CodingHEVC` | H.265/HEVC | Yes | Yes |

---

## Structures

### `nvDecParam`

Decoder creation parameters. Consumed once by `nvmpi_create_decoder()`.

| Field | Type | Description | Default |
|-------|------|-------------|---------|
| `frame_pool_size` | `int` | Decoded frames buffered before user must read. | 12 |
| `codingType` | `nvCodingType` | Input bitstream codec. | — (required) |
| `pixFormat` | `nvPixFormat` | Requested output pixel layout. | — (required) |
| `resized` | `nvSize` | Optional hw scaling target; `{0,0}` keeps stream resolution. | `{0,0}` |
| `chunk_size` | `unsigned int` | Bytes per compressed-input V4L2 buffer. 0 = default 10 MiB. | 0 |
| `max_perf` | `int` | Non-zero: lift NVDEC clock governor (max clocks). | 0 |
| `disable_dpb` | `int` | Non-zero: skip DPB reordering (low-latency, no B-frames). | 0 |
| `wait_timeout` | `int` | Blocking wait timeout in ms. 0 = use default 500ms. Range: 50–5000. | 0 |

### `nvEncParam`

Encoder creation parameters. Consumed once by `nvmpi_create_encoder()`.

| Field | Type | Description | Default |
|-------|------|-------------|---------|
| `width` | `unsigned int` | Frame width in pixels. | — (required) |
| `height` | `unsigned int` | Frame height in pixels. | — (required) |
| `profile` | `unsigned int` | FFmpeg-style H.264 profile id (66/77/100). | 0 |
| `level` | `unsigned int` | H.264 level_idc (e.g. 41 = 4.1). | 0 |
| `bitrate` | `unsigned int` | Target bitrate in bit/s. | 0 |
| `peak_bitrate` | `unsigned int` | Peak bitrate for VBR (0 = derive from bitrate). | 0 |
| `enableLossless` | `char` | Non-zero: constant QP 0 / High 4:4:4 (H.264 only). | 0 |
| `mode_vbr` | `char` | Non-zero: VBR rate control instead of CBR. | 0 |
| `insert_spspps_idr` | `char` | Non-zero: repeat SPS/PPS at every IDR frame. | 0 |
| `insert_vui` | `char` | Non-zero: embed VUI timing_info (fps) in bitstream. | 0 |
| `pixFormat` | `nvPixFormat` | Raw input layout: `NV_PIX_YUV420` or `NV_PIX_NV12`. | `NV_PIX_YUV420` |
| `iframe_interval` | `unsigned int` | I-frame period in frames. | 0 |
| `idr_interval` | `unsigned int` | IDR period in frames. | 0 |
| `fps_n` | `unsigned int` | Framerate numerator. | 0 |
| `fps_d` | `unsigned int` | Framerate denominator. | 0 |
| `capture_num` | `int` | Number of V4L2 buffers per plane. | 0 |
| `max_b_frames` | `unsigned int` | B frames between references (H.264 only). | 0 |
| `refs` | `unsigned int` | Reference frames (0 = encoder default). | 0 |
| `qmax` | `unsigned int` | Max quantizer (applied only with qmin). | 0 |
| `qmin` | `unsigned int` | Min quantizer (applied only with qmax). | 0 |
| `hw_preset_type` | `unsigned int` | Speed/quality preset: 1=ultrafast .. 4=slow. | 0 |
| `vbv_buffer_size` | `unsigned int` | Virtual buffer size of the encoder. | 0 |
| `codingType` | `nvCodingType` | `NV_VIDEO_CodingH264` or `NV_VIDEO_CodingHEVC`. | — (required) |
| `max_perf` | `int` | Non-zero: lift NVENC clock governor (max clocks). | 0 |
| `poc_type` | `unsigned int` | H.264 picture order count type (0=default, 2=low-latency). | 0 |

### `nvPacket`

Compressed packet exchanged across the API boundary.

| Field | Type | Description |
|-------|------|-------------|
| `flags` | `unsigned long` | Keyframe flag (matches `AV_PKT_FLAG_KEY`). |
| `payload_size` | `unsigned long` | Valid bytes in `payload`. |
| `payload` | `unsigned char*` | Bitstream data. |
| `pts` | `unsigned long` | Presentation timestamp in microseconds. |
| `privData` | `void*` | Internal: NVMPI_pkt pointer (encoder only). |

**Ownership:** Decoder direction — caller owns `payload` (memcpy'd into V4L2
buffer, may be freed on return). Encoder direction — packets are pooled;
see `qEmptyPacket`/`dqEmptyPacket`.

### `nvFrame`

Raw planar frame exchanged across the API boundary.

| Field | Type | Description |
|-------|------|-------------|
| `flags` | `unsigned long` | Frame flags. |
| `payload_size[3]` | `unsigned long[3]` | Per-plane byte size. |
| `payload[3]` | `unsigned char*[3]` | Per-plane data pointers (caller-owned). |
| `linesize[3]` | `unsigned int[3]` | Per-plane stride in bytes. |
| `type` | `nvPixFormat` | Pixel format. |
| `width` | `unsigned int` | Frame width. |
| `height` | `unsigned int` | Frame height. |
| `timestamp` | `time_t` | Microseconds (carried via V4L2 buffer timestamp). |

**Ownership:** Payload pointers always reference caller-owned memory.
The decoder memcpy's into them (`get_frame`), the encoder memcpy's out
(`put_frame`). No plane buffer changes ownership.

### `nvSize`

Simple width/height pair for the optional decoder resize target.

| Field | Type | Description |
|-------|------|-------------|
| `width` | `unsigned int` | Target width (0 = no resize). |
| `height` | `unsigned int` | Target height (0 = no resize). |

---

## Decoder API

### `nvmpi_create_decoder`

```c
nvmpictx* nvmpi_create_decoder(nvDecParam* param);
```

Create a decoder context. Opens the V4L2 decoder device, sets up the
bitstream (OUTPUT) plane, and spawns the internal capture thread. The
`param` struct is read once and not retained.

**Returns:** Opaque context handle. NULL on failure.

**Thread safety:** Call from user thread only. No concurrent access exists
yet at creation time.

### `nvmpi_decoder_put_packet`

```c
int nvmpi_decoder_put_packet(nvmpictx* ctx, nvPacket* packet);
```

Feed one compressed packet (Annex-B for H.264/HEVC). May block waiting
for a free V4L2 OUTPUT-plane buffer once all buffers are in flight.

A packet with `payload_size == 0` signals end-of-stream (starts flushing).

**Returns:**

| Code | Meaning |
|------|---------|
| `0` | Success. |
| `-1` | Dequeue failure (V4L2 output plane). |
| `-2` | Queue failure (V4L2 output plane). |
| `-3` | Packet exceeds `chunk_size` — dropped. Smaller packets continue. |

**Thread safety:** User thread only.

### `nvmpi_decoder_get_frame`

```c
int nvmpi_decoder_get_frame(nvmpictx* ctx, nvFrame* frame, bool wait);
```

Retrieve one decoded frame by copying it into `frame->payload` planes
(which must already be allocated with matching `linesize`).

**Behavior depends on `wait`:**

| `wait` | Behavior |
|--------|----------|
| `false` | Non-blocking. Returns `-1` immediately if no frame is ready. |
| `true` | Blocks up to `wait_timeout_ms` (default 500ms, configurable via `nvDecParam.wait_timeout`). Returns `-1` on timeout or shutdown. |

The blocking path uses condition-variable waits with 100ms internal
granularity for shutdown responsiveness. See [THREAD_SAFETY.md](THREAD_SAFETY.md)
for full details.

**Returns:** `0` on success, `-1` on no frame available / timeout / shutdown.

**Thread safety:** User thread only. The blocking path holds `m_filledBuf`
during CV waits but releases it between iterations.

### `nvmpi_decoder_flush`

```c
int nvmpi_decoder_flush(nvmpictx* ctx);
```

Reset the decoder pipeline for seek / stream restart. Stops the capture
thread, drains in-flight frames, clears the shutdown flag, and restarts.

The caller **must** re-prime extradata (SPS/PPS) after this call so the
hardware decoder can reconfigure its capture plane.

**Returns:** Always `0`.

**Thread safety:** User thread only. Joins the capture thread internally.

### `nvmpi_decoder_close`

```c
int nvmpi_decoder_close(nvmpictx* ctx);
```

Stop the capture thread, free all DMA buffers/pools, and destroy `ctx`.
The handle is invalid afterwards.

Internally: sets `eos`, calls `shutdown()` on the frame pool (unblocking
any waiting consumer), stops the V4L2 stream, joins the capture thread,
then frees all resources.

**Returns:** Always `0`.

**Thread safety:** User thread only.

---

## Encoder API

### `nvmpi_create_encoder`

```c
nvmpictx* nvmpi_create_encoder(nvEncParam* param);
```

Create an encoder context. Opens the V4L2 encoder device, programs
profile/level/rate-control from `param`, sets up both planes, and starts
the capture-plane dequeue thread.

The caller must still fill the packet pool (`qEmptyPacket`) before
encoded output can be delivered.

**Returns:** Opaque context handle.

**Thread safety:** User thread only.

### `nvmpi_encoder_put_frame`

```c
int nvmpi_encoder_put_frame(nvmpictx* ctx, nvFrame* frame);
```

Copy frame planes into a V4L2 OUTPUT-plane buffer and queue it. May
block waiting for a free buffer.

`frame == NULL` sends EOS and puts the encoder into flushing mode.

**Returns:** `0` on success, negative on error or if already flushing.

**Thread safety:** User thread only.

### `nvmpi_encoder_get_packet`

```c
int nvmpi_encoder_get_packet(nvmpictx* ctx, nvPacket** packet);
```

Dequeue one encoded packet.

| Mode | Behavior |
|------|----------|
| Encoding | Non-blocking. Returns `-1` if nothing ready. |
| Flushing | Blocks until a packet arrives or EOS. Returns `-2` on EOS. |

On success, the caller owns the packet until it re-queues it with
`nvmpi_encoder_qEmptyPacket()`.

**Thread safety:** User thread only.

### `nvmpi_encoder_dqEmptyPacket`

```c
int nvmpi_encoder_dqEmptyPacket(nvmpictx* ctx, nvPacket** packet);
```

Non-blocking dequeue from the empty packet pool. Returns `-1` when
exhausted. Used by the FFmpeg wrapper to drain/teardown the pool.

**Thread safety:** User thread only.

### `nvmpi_encoder_qEmptyPacket`

```c
void nvmpi_encoder_qEmptyPacket(nvmpictx* ctx, nvPacket* packet);
```

Hand a caller-allocated packet (payload buffer included) to the pool.
The encoder's capture thread fills it later. The pool stores only the
pointer — the caller remains responsible for eventually freeing it.

**Thread safety:** User thread only. The pool itself is thread-safe
(the DQ thread reads from the other end).

### `nvmpi_encoder_close`

```c
int nvmpi_encoder_close(nvmpictx* ctx);
```

Stop the dequeue thread and free the context. Packets still held in
the pools are **not** freed here — drain them first.

**Returns:** Always `0`.

**Thread safety:** User thread only.
