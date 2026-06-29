/*
 * nvmpi.h — public C API of libnvmpi (layer 1 of jetson-ffmpeg).
 *
 * libnvmpi wraps NVIDIA's Jetson V4L2/NvBuffer multimedia API behind a small,
 * plain-C encode/decode interface. This header is the ONLY contract between
 * the two layers of the project:
 *   - implemented by src/nvmpi_dec_api.cpp (and its siblings nvmpi_dec_capture.cpp,
 *     nvmpi_dec_planes.cpp, nvmpi_dec_internal.h) and src/nvmpi_enc.cpp (built into
 *     libnvmpi.so, installed system-wide), and
 *   - consumed by the FFmpeg integration layer
 *     (ffmpeg/dev/common/libavcodec/nvmpi_{enc,dec}.c), which is patched
 *     into an FFmpeg source tree and compiled as part of FFmpeg.
 *
 * Usage pattern (both directions are simple put/get loops):
 *   decoder: nvmpi_create_decoder -> put_packet/get_frame ... -> close
 *   encoder: nvmpi_create_encoder -> put_frame/get_packet ... -> close
 */
#ifndef __NVMPI_H__
#define __NVMPI_H__
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

//Maximum size of the encoded buffers on the capture plane in bytes
//Also used by the FFmpeg wrapper as the allocation size of each pooled
//packet buffer, so a single encoded frame must never exceed this.
//10 MiB leaves headroom for 4K high-bitrate I-frames; frames larger than
//this are dropped with an error instead of overflowing the packet buffer.
#define NVMPI_ENC_CHUNK_SIZE 10*1024*1024

//Opaque context handle. Note: the decoder and encoder each define their own
//(different) struct nvmpictx internally; a handle must only be passed back
//to the API family (decoder_* or encoder_*) that created it.
typedef struct nvmpictx nvmpictx;

//Raw (uncompressed) pixel layouts. Used for decoder output and encoder
//input. NV_PIX_P010 is 10-bit 4:2:0 semi-planar (matches FFmpeg P010LE) and
//is decode-only, HEVC Main10 only, and requires the NvUtils buffer API.
typedef enum {
	NV_PIX_NV12,
	NV_PIX_YUV420,
	NV_PIX_P010
}nvPixFormat;

//Compressed bitstream codec selector. Decode supports all entries;
//encode supports only H.264 and HEVC (see https://github.com/gjrtimmer/jetson-ffmpeg/wiki/Development-Guide).
typedef enum {
	NV_VIDEO_CodingUnused,
	NV_VIDEO_CodingH264,             /**< H.264 */
	NV_VIDEO_CodingMPEG4,              /**< MPEG-4 */
	NV_VIDEO_CodingMPEG2,              /**< MPEG-2 */
	NV_VIDEO_CodingVP8,                /**< VP8 */
	NV_VIDEO_CodingVP9,                /**< VP9 */
	NV_VIDEO_CodingHEVC,               /**< H.265/HEVC */
	NV_VIDEO_CodingMJPEG,              /**< MJPEG (via NvJPEGDecoder, not V4L2) */
} nvCodingType;

//Simple width/height pair (used for the optional decoder resize target).
typedef struct _NVSIZE{
	unsigned int width;
	unsigned int height;
}nvSize;

//Encoder creation parameters, filled in by the caller (the FFmpeg wrapper
//maps AVCodecContext fields / AVOptions onto these) and consumed once by
//nvmpi_create_encoder(). Numeric profile/level values follow FFmpeg's
//FF_PROFILE_H264_*/level_idc conventions and are translated to the
//corresponding V4L2 enums inside the encoder.
typedef struct _NVENCPARAM{
	unsigned int width;
	unsigned int height;
	unsigned int profile;          //FFmpeg-style H.264 profile id (66/77/100)
	unsigned int level;            //H.264 level_idc style value (e.g. 41 = 4.1)
	unsigned int bitrate;          //target bitrate in bit/s
	unsigned int peak_bitrate;     //peak bitrate for VBR mode (0 = derive from bitrate)
	char enableLossless;           //non-zero: constant QP 0 / High 4:4:4 (H.264 only)
	char mode_vbr;                 //non-zero: VBR rate control instead of CBR
	char insert_spspps_idr;        //non-zero: repeat SPS/PPS at every IDR frame
	char insert_vui;               //non-zero: embed VUI timing_info (fps) in the bitstream
	nvPixFormat pixFormat;         //raw input layout: NV_PIX_YUV420 (default) or NV_PIX_NV12
	unsigned int iframe_interval;  //I-frame period in frames
	unsigned int idr_interval;     //IDR period in frames
	unsigned int fps_n;            //framerate numerator
	unsigned int fps_d;            //framerate denominator
	int capture_num;               //number of V4L2 buffers per plane (output & capture)
	unsigned int max_b_frames;     //number of B frames between references (H.264 only)
	unsigned int refs;             //number of reference frames (0 = encoder default)
	unsigned int qmax;             //max quantizer (0 = unset; applied only with qmin)
	unsigned int qmin;             //min quantizer (0 = unset; applied only with qmax)
	unsigned int hw_preset_type;   //speed/quality preset: 1=ultrafast .. 4=slow
	unsigned int vbv_buffer_size; //virtual buffer size of the encoder
	nvCodingType codingType;       //NV_VIDEO_CodingH264 or NV_VIDEO_CodingHEVC
	int max_perf;                  //non-zero: lift NVENC clock governor (max clocks)
	unsigned int poc_type;         //H.264 picture order count type (0=default, 2=low-latency)
	unsigned int wait_timeout;     //blocking-wait timeout in ms (0 = default 500ms)
	char enable_cabac;             //non-zero: enable CABAC entropy coding (H.264 only; default CAVLC)
	char insert_aud;               //non-zero: insert Access Unit Delimiter NALs
	int use_dmabuf;                //non-zero: encoder OUTPUT plane uses V4L2_MEMORY_DMABUF,
	                               //enabling zero-copy frame input via nvmpi_encoder_put_frame_fd()
} nvEncParam;

//Decoder creation parameters, consumed once by nvmpi_create_decoder().
typedef struct _NVDECPARAM{
	int frame_pool_size;     //number of decoded frames buffered before the user must read
	nvCodingType codingType; //input bitstream codec
	nvPixFormat pixFormat;   //requested output pixel layout
	nvSize resized;          //optional hw scaling target; {0,0} keeps stream resolution
	unsigned int chunk_size; //bytes per compressed-input V4L2 buffer; 0 = default (10 MiB).
	                         //One input packet (access unit) must fit in one chunk.
	int max_perf;            //non-zero: lift NVDEC clock governor (max clocks)
	int disable_dpb;         //non-zero: skip decoded-picture-buffer reordering (low-latency)
	int wait_timeout;        //blocking wait timeout in ms (0 = use default 500ms)
	unsigned int width;      //stream width from container headers (0 = unknown/use resolution-change events)
	unsigned int height;     //stream height from container headers (0 = unknown/use resolution-change events)
} nvDecParam;

//Compressed packet exchanged across the API boundary.
//Decoder direction: caller owns 'payload' (it is memcpy'd into a V4L2
//buffer inside put_packet, so it may be freed on return).
//Encoder direction: packets are pooled. The wrapper pre-allocates them
//(payload typically NVMPI_ENC_CHUNK_SIZE bytes), queues them as "empty"
//via qEmptyPacket, and receives them back "filled" from get_packet.
typedef struct _NVPACKET{
	unsigned long flags;        //encoder sets keyframe flag (matches AV_PKT_FLAG_KEY)
	unsigned long payload_size; //valid bytes in payload
	unsigned char *payload;     //bitstream data
	unsigned long  pts;         //presentation timestamp in microseconds
	//NVMPI_pkt pointer. used by encoder
	void* privData;
} nvPacket;

//Raw planar frame exchanged across the API boundary. The payload pointers
//always reference caller-owned memory: the decoder memcpy's decoded data
//into them (get_frame) and the encoder memcpy's out of them (put_frame),
//so no plane buffer ever changes ownership.
typedef struct _NVFRAME{
	unsigned long flags;
	unsigned long payload_size[3]; //per-plane byte size
	unsigned char *payload[3];     //per-plane data pointers (caller-owned)
	unsigned int linesize[3];      //per-plane stride in bytes
	nvPixFormat type;
	unsigned int width;
	unsigned int height;
	time_t timestamp;              //microseconds (carried via the V4L2 buffer timestamp)
}nvFrame;

//Release callback for borrowed DMA-BUF fds returned by get_frame_fd.
//The caller MUST invoke this callback (passing the opaque pointer) when
//done with the fd — it returns the underlying frame buffer to the
//decoder's internal pool so it can be reused for future frames. If the
//caller needs the fd to outlive the callback, it must dup() the fd
//before calling back. Failure to invoke the callback starves the pool
//and stalls decoding.
typedef void (*nvmpi_frame_release_cb)(void *opaque);

#ifdef __cplusplus
extern "C" {
#endif

	//Create a decoder context: opens the V4L2 decoder device, sets up the
	//bitstream (OUTPUT) plane and spawns the internal capture thread.
	//Returns NULL-checked opaque handle; param is read once and not retained.
	nvmpictx* nvmpi_create_decoder(nvDecParam* param);

	//Feed one compressed packet (Annex-B for H.264/HEVC). May block waiting
	//for a free V4L2 OUTPUT-plane buffer once all buffers are in flight.
	//A packet with payload_size==0 signals end-of-stream (starts flushing).
	//Returns 0 on success, -1 on dequeue failure, -2 on queue failure,
	//-3 when the packet exceeds chunk_size (invalid input — the packet is
	//dropped; smaller packets continue to be accepted).
	int nvmpi_decoder_put_packet(nvmpictx* ctx, nvPacket* packet);

	//Retrieve one decoded frame by copying it into the caller-provided
	//frame->payload planes (which must already be allocated with matching
	//linesize). When wait is false: non-blocking, returns -1 immediately
	//if no frame is ready. When wait is true: blocks up to wait_timeout
	//milliseconds (configurable via nvDecParam.wait_timeout, default
	//500ms). Returns -1 on timeout or shutdown.
	int nvmpi_decoder_get_frame(nvmpictx* ctx, nvFrame* frame,bool wait);

	//Reset the decoder pipeline for seek / stream restart: stops the
	//capture thread, drains in-flight frames, and restarts. The caller
	//MUST re-prime extradata (SPS/PPS) after this call so the hardware
	//decoder can reconfigure its capture plane. Always returns 0.
	int nvmpi_decoder_flush(nvmpictx* ctx);

	//Stop the capture thread, free all DMA buffers/pools and destroy ctx.
	//The handle is invalid afterwards. Always returns 0.
	int nvmpi_decoder_close(nvmpictx* ctx);

	//--- JPEG decoder (synchronous, NvJPEGDecoder-backed, no V4L2) ---

	//Create a JPEG decoder context backed by the Tegra NVJPG engine.
	//No V4L2 device, no capture thread — decoding is synchronous per frame.
	//Returns NULL on failure (NvJPEGDecoder creation error).
	nvmpictx* nvmpi_create_jpeg_decoder(void);

	//Decode one JPEG frame. payload_size==0 signals EOS (no decode, sets
	//internal eos flag). Returns 0 on success, -1 on decode error or
	//unsupported JPEG format (progressive). The decoded frame is queued
	//internally for retrieval via nvmpi_jpeg_decoder_get_frame().
	int nvmpi_jpeg_decoder_put_packet(nvmpictx* ctx, nvPacket* packet);

	//Retrieve one decoded frame. When wait is true: blocks up to 500ms
	//for a frame. Returns -1 when no frame is available (timeout or EOS).
	int nvmpi_jpeg_decoder_get_frame(nvmpictx* ctx, nvFrame* frame, bool wait);

	//Destroy the JPEG decoder context and free all DMA buffers.
	int nvmpi_jpeg_decoder_close(nvmpictx* ctx);

	//--- JPEG encoder (synchronous, NvJPEGEncoder-backed, no V4L2) ---

	//Create a JPEG encoder context backed by the Tegra NVJPG engine.
	//No V4L2 device, no DQ thread — encoding is synchronous per frame.
	//quality: libjpeg quality 1-100 (clamped). Default 85 if out of range.
	//Returns NULL on failure. On modules without NVJPG encode capability
	//(e.g. Orin Nano), createJPEGEncoder returns NULL — the FFmpeg wrapper
	//reports a clear error suggesting -c:v mjpeg software fallback.
	nvmpictx* nvmpi_create_jpeg_encoder(int quality);

	//Encode one YUV420 frame to JPEG. frame==NULL signals EOS.
	//Returns 0 on success, -1 on error. The encoded JPEG is staged
	//internally for retrieval via nvmpi_jpeg_encoder_get_packet().
	int nvmpi_jpeg_encoder_put_frame(nvmpictx* ctx, nvFrame* frame);

	//Retrieve the encoded JPEG packet. Copies JPEG data into
	//packet->payload (caller must pre-allocate). Returns 0 on success,
	//-1 when no packet is ready, -2 on EOS with no packet.
	//packet->flags is set to 1 (keyframe) for every JPEG frame.
	int nvmpi_jpeg_encoder_get_packet(nvmpictx* ctx, nvPacket* packet);

	//Destroy the JPEG encoder context and free all resources.
	int nvmpi_jpeg_encoder_close(nvmpictx* ctx);

	//Create an encoder context: opens the V4L2 encoder device, programs
	//profile/level/rate-control from param, sets up both planes and starts
	//the capture-plane dequeue thread. Returns opaque handle.
	//NOTE: the caller must still fill the packet pool (qEmptyPacket) before
	//encoded output can be delivered.
	nvmpictx* nvmpi_create_encoder(nvEncParam* param);
	//add frame to encoder
	//Copies the frame planes into a V4L2 OUTPUT-plane buffer and queues it.
	//May block waiting for a free buffer. frame==NULL sends EOS and puts the
	//encoder into flushing mode. Returns 0 on success, negative on error or
	//if already flushing.
	int nvmpi_encoder_put_frame(nvmpictx* ctx, nvFrame* frame);
	//get filled packet from encoder
	//When wait=false: non-blocking, returns -1 immediately if no packet ready.
	//When wait=true: blocks up to wait_timeout_ms using the pool's CV-based
	//tiered wait. Returns -1 on timeout or shutdown.
	//While flushing: blocks until a packet arrives or EOS (-2 = EOS).
	//On success the caller owns the packet until it re-queues it with
	//nvmpi_encoder_qEmptyPacket().
	int nvmpi_encoder_get_packet(nvmpictx* ctx, nvPacket** packet, bool wait);
	//get empty packet with allocated buffer from encoder packet pool
	//Non-blocking; returns -1 when the empty pool is exhausted. Used by the
	//wrapper to drain/teardown the pool.
	int nvmpi_encoder_dqEmptyPacket(nvmpictx* ctx, nvPacket** packet);
	//add empty packet with allocated buffer  to  encoder packet pool
	//Hands a caller-allocated packet (payload buffer included) to the pool;
	//the encoder's capture thread fills it later. The pool only stores the
	//pointer — the caller remains responsible for eventually freeing it.
	void nvmpi_encoder_qEmptyPacket(nvmpictx* ctx, nvPacket* packet);
	//Force the encoder to produce an IDR frame for the next queued input.
	//Calls NvVideoEncoder::forceIDR() which sets
	//V4L2_CID_MPEG_MFC51_VIDEO_FORCE_FRAME_TYPE. May be called at any time
	//while the encoder is running. Returns 0 on success, -1 on error.
	int nvmpi_encoder_force_idr(nvmpictx* ctx);
	//Change the encoder target bitrate mid-stream (adaptive bitrate).
	//Calls NvVideoEncoder::setBitrate() which updates
	//V4L2_CID_MPEG_VIDEO_BITRATE. Takes effect from the next encoded frame.
	//Returns 0 on success, -1 on error.
	int nvmpi_encoder_set_bitrate(nvmpictx* ctx, unsigned int bitrate);
	//flush encoder (mid-stream reset)
	//Stops DQ thread, STREAMOFF both planes, drains packet pool, resets
	//flushing/EOS state, STREAMON, re-queues capture buffers, restarts DQ
	//thread. Encoding can resume immediately after this call.
	int nvmpi_encoder_flush(nvmpictx* ctx);
	//close encoder
	//Stops the dequeue thread and frees the context. Packets still held in
	//the pools are NOT freed here — drain them first (see FFmpeg wrapper).
	int nvmpi_encoder_close(nvmpictx* ctx);

	//--- DMA-BUF fd passthrough (zero-copy path) ---

	//Retrieve one decoded frame as a borrowed DMA-BUF fd instead of copying
	//pixel data into caller-owned memory (zero-copy alternative to
	//nvmpi_decoder_get_frame). The fd points to a pitch-linear NV12/YUV420
	//buffer that has already been VIC-transformed from the decoder's
	//block-linear capture plane. The caller receives:
	//  *dmabuf_fd  — the DMA-BUF file descriptor (borrowed, not dup'd)
	//  *width, *height, *pitch — buffer geometry
	//  *timestamp  — presentation timestamp in microseconds
	//  *release    — callback the caller MUST invoke when done with the fd
	//  *opaque     — cookie to pass to the release callback
	//The fd is valid only until the release callback is invoked. If the
	//caller needs it longer (e.g. for encoder input), it must dup() first.
	//When wait is false: non-blocking, returns -1 when no frame is ready.
	//When wait is true: blocks up to wait_timeout_ms (default 500ms).
	//Returns 0 on success, -1 on no frame / timeout.
	int nvmpi_decoder_get_frame_fd(nvmpictx* ctx,
		int *dmabuf_fd, int *width, int *height, int *pitch,
		int64_t *timestamp,
		nvmpi_frame_release_cb *release, void **opaque, bool wait);

	//Submit one raw frame to the encoder via DMA-BUF fd (zero-copy
	//alternative to nvmpi_encoder_put_frame). The fd must reference an
	//NV12 DMA-BUF surface with pitch-linear layout and matching
	//dimensions. The encoder accesses the buffer directly via
	//V4L2_MEMORY_DMABUF — no CPU memcpy. Requires use_dmabuf=1 in
	//nvEncParam at encoder creation time. dmabuf_fd==−1 signals EOS.
	//Returns 0 on success, negative on error or if already flushing.
	int nvmpi_encoder_put_frame_fd(nvmpictx* ctx,
		int dmabuf_fd, int width, int height, int pitch,
		int64_t timestamp);

	//Allocate a pitch-linear NV12 DMA-BUF surface suitable for passing
	//to nvmpi_encoder_put_frame_fd. The surface is allocated with
	//NvBufSurfaceTag_VIDEO_CONVERT so the encoder's VIC hardware can
	//access it. Returns 0 on success and writes the dmabuf fd to
	//*dmabuf_fd. On failure returns -1 and *dmabuf_fd is set to -1.
	int nvmpi_surface_alloc(unsigned int width, unsigned int height,
		int *dmabuf_fd);

	//Destroy a surface previously allocated with nvmpi_surface_alloc.
	//Safe to call with dmabuf_fd == -1 (no-op). Returns 0 on success,
	//-1 on NvBufferDestroy failure.
	int nvmpi_surface_destroy(int dmabuf_fd);

	//Copy NV12 frame data from an unregistered DMA-BUF fd (e.g. dup'd
	//from decoder DRM_PRIME output) into a registered NvBufSurface fd.
	//Uses mmap + CPU copy + DMA-BUF sync. dst_fd must have been
	//allocated via nvmpi_surface_alloc at matching dimensions.
	//src_pitch = source row stride in bytes (from DRM descriptor).
	int nvmpi_surface_copy_from_dmabuf(int dst_fd, int src_fd,
		unsigned int width, unsigned int height,
		unsigned int src_pitch);

	//--- VIC hardware transform (standalone scale/CSC) ---

	//Opaque VIC context handle. Created by nvmpi_vic_create(), destroyed
	//by nvmpi_vic_close(). Holds the VIC session binding and cached
	//transform parameters. Must not be shared across threads — each
	//thread needs its own context (VIC session is thread-local).
	typedef struct nvmpi_vic_ctx nvmpi_vic_ctx;

	//Create a VIC transform context: binds the VIC engine for the
	//calling thread via NvBufSurfTransformSetSessionParams (NvUtils) or
	//NvBufferSessionCreate (legacy). Returns NULL on failure.
	//Allocated here; freed in nvmpi_vic_close().
	nvmpi_vic_ctx *nvmpi_vic_create(void);

	//Perform a VIC hardware scale/CSC transform between two DMA-BUF
	//surfaces. Both fds must reference valid NV12 DMA-BUF buffers
	//(e.g. from nvmpi_surface_alloc or decoder output). The VIC engine
	//handles scaling, block-linear ↔ pitch-linear conversion, and
	//color-space conversion in hardware with zero CPU copies.
	//Returns 0 on success, -1 on error.
	int nvmpi_vic_transform(nvmpi_vic_ctx *ctx,
		int src_fd, int dst_fd,
		unsigned int src_w, unsigned int src_h,
		unsigned int dst_w, unsigned int dst_h);

	//Destroy a VIC context and release the VIC session. Safe to call
	//with NULL (no-op). Freed here; allocated in nvmpi_vic_create().
	void nvmpi_vic_close(nvmpi_vic_ctx *ctx);

#ifdef __cplusplus
}
#endif

#endif /*__NVMPI_H__*/
