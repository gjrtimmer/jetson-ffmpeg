/*
 * nvmpi_jpegdec_internal.h — JPEG decoder context (NvJPEGDecoder-backed).
 *
 * Unlike the V4L2 video decoder (nvmpi_dec_internal.h), JPEG decoding is
 * synchronous per-frame via NvJPEGDecoder::decodeToFd. There is no capture
 * thread, no V4L2 M2M device, no resolution-change events.
 *
 * The decoded DMA fd from decodeToFd is block-linear; a VIC transform
 * converts it to a pitch-linear nvmpi_frame_buffer, which is then queued
 * to the frame pool for retrieval via nvmpi_jpeg_decoder_get_frame().
 *
 * Allocated by nvmpi_create_jpeg_decoder(); freed by nvmpi_jpeg_decoder_close().
 */
#pragma once

#include "nvmpi.h"
#include "NvJpegDecoder.h"
#include "nvUtils2NvBuf.h"
#include "NVMPI_bufPool.hpp"
#include "nvmpi_frame_buffer.hpp"
#include <vector>
#include <iostream>
#include <atomic>
#include <cstring>

/* Maximum dimension the NVJPG engine supports (baseline JPEG). */
#define NVJPEG_MAX_DIM 16384

/* Default frame pool size for JPEG decoder. Smaller than video because
 * JPEG decode is synchronous — frames are produced one at a time. */
#define JPEG_FRAME_POOL_SIZE 4

/* JPEG SOF markers for format detection. */
#define JPEG_MARKER_SOF0 0xC0  /* Baseline DCT (supported) */
#define JPEG_MARKER_SOF2 0xC2  /* Progressive DCT (NOT supported by HW) */

/* Reuse the error-reporting macro from the V4L2 decoder. */
#define JPEG_ERROR(condition, message)    \
	if (condition)                    \
{                                         \
	std::cerr << "[libnvmpi][jpeg] " << message << std::endl; \
}

/*
 * JPEG decoder context.
 *
 * Named nvmpictx_jpeg (not nvmpictx) to avoid linker-level ODR collisions
 * with the V4L2 decoder's nvmpictx — both TUs are linked into libnvmpi.so
 * and identically-named member functions would produce duplicate symbols.
 *
 * The C API in nvmpi.h declares an opaque 'nvmpictx*'. The JPEG API
 * functions cast between nvmpictx* and nvmpictx_jpeg* at the boundary.
 */
struct nvmpictx_jpeg
{
	/* Allocated by nvmpi_create_jpeg_decoder(); freed in close(). */
	NvJPEGDecoder *jpegdec{nullptr};

	/* Set on EOS (zero-size packet) or fatal error. */
	std::atomic<bool> eos{false};

	/* Last decoded dimensions — used to detect resolution changes that
	 * require frame pool reallocation. */
	uint32_t last_width{0};
	uint32_t last_height{0};

	/* Output pixel format — JPEG always produces YUV420 via VIC. */
	nvPixFormat out_pixfmt{NV_PIX_YUV420};

	/* Frame pool: producer = put_packet (synchronous), consumer = get_frame. */
	NVMPI_bufPool<nvmpi_frame_buffer*>* framePool{nullptr};
	std::vector<nvmpi_frame_buffer*> allocatedFrameBufs;
	int frame_pool_size{JPEG_FRAME_POOL_SIZE};

	/* Cached plane geometry of the destination DMA buffers (filled by
	 * updateFrameSizeParams after pool allocation). Drives the memcpy
	 * in get_frame. */
	unsigned int num_planes{0};
	unsigned int frame_linesize[3];    /* pitch of each plane */
	unsigned int frame_height[3];      /* number of lines per plane */
	unsigned int frame_linedatasize[3]; /* valid data bytes per line */

	/* VIC transform parameters. */
#ifdef WITH_NVUTILS
	NvBufSurfTransformConfigParams session;
#else
	NvBufferSession session;
#endif
	NvBufferTransformParams transform_params;
	NvBufferRect src_rect, dest_rect;

	/* Allocate frame_pool_size pitch-linear DMA buffers at width×height
	 * and seed the pool's empty queue. */
	void initFramePool(uint32_t width, uint32_t height);

	/* Destroy all frame buffers and drain both pool queues. */
	void deinitFramePool();

	/* Read actual plane geometry from one allocated buffer. */
	void updateFrameSizeParams();

	/* Set up VIC transform rects for width×height pass-through. */
	void updateTransformParams(uint32_t width, uint32_t height);
};
