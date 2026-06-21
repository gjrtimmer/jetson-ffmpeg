/*
 * nvmpi_enc_jpeg_internal.h — JPEG encoder context (NvJPEGEncoder-backed).
 *
 * Unlike the V4L2 video encoder (nvmpi_enc.cpp), JPEG encoding is
 * synchronous per-frame via NvJPEGEncoder::encodeFromFd. There is no
 * DQ thread, no V4L2 M2M device, no packet pool.
 *
 * The caller's YUV420 frame is copied into a pitch-linear DMA buffer,
 * then encodeFromFd produces a JPEG bitstream in a malloc'd buffer.
 * The output is staged in an internal packet for retrieval via
 * nvmpi_jpeg_encoder_get_packet().
 *
 * Allocated by nvmpi_create_jpeg_encoder(); freed by nvmpi_jpeg_encoder_close().
 */
#pragma once

#include "nvmpi.h"
#include "NvJpegEncoder.h"
#include "nvUtils2NvBuf.h"
#include "nvmpi_frame_buffer.hpp"
#include "nvmpi_log.h"
#include <atomic>
#include <cstring>

/* Maximum dimension the NVJPG engine supports (baseline JPEG). */
#define NVJPEG_ENC_MAX_DIM 16384

/* Default JPEG quality when none is specified (libjpeg scale 1-100). */
#define NVJPEG_ENC_DEFAULT_QUALITY 85

/*
 * JPEG encoder context.
 *
 * Named nvmpictx_jpegenc (not nvmpictx) to avoid linker-level ODR
 * collisions with the V4L2 encoder/decoder and JPEG decoder contexts
 * — all TUs are linked into libnvmpi.so.
 *
 * The C API in nvmpi.h declares an opaque 'nvmpictx*'. The JPEG encoder
 * API functions cast between nvmpictx* and nvmpictx_jpegenc* at the
 * boundary.
 */
struct nvmpictx_jpegenc
{
	/* Allocated by nvmpi_create_jpeg_encoder(); freed in close(). */
	NvJPEGEncoder *jpegenc{nullptr};

	/* Set on EOS (NULL frame) or fatal error. */
	std::atomic<bool> eos{false};

	/* JPEG quality 1-100 (libjpeg scale). */
	int quality{NVJPEG_ENC_DEFAULT_QUALITY};

	/* Current frame dimensions — set on first put_frame, updated on
	 * resolution change. Controls DMA buffer (re)allocation. */
	uint32_t width{0};
	uint32_t height{0};

	/* Single reusable DMA buffer for the encoder input. Allocated on
	 * first put_frame (or reallocated on resolution change).
	 * Allocated by allocInputBuffer(); freed by freeInputBuffer(). */
	nvmpi_frame_buffer input_buf;
	bool input_buf_allocated{false};

	/* Cached plane geometry of the input DMA buffer (filled after
	 * allocation). Drives the memcpy from the caller's nvFrame into
	 * the DMA buffer. */
	unsigned int num_planes{0};
	unsigned int plane_pitch[3];     /* stride in bytes per plane */
	unsigned int plane_height[3];    /* number of lines per plane */
	unsigned int plane_linedata[3];  /* valid data bytes per line */

	/* Staged output: encodeFromFd produces a malloc'd JPEG buffer.
	 * Held here until get_packet copies it out, then freed. */
	unsigned char *out_buf{nullptr};
	unsigned long out_buf_size{0};
	unsigned long out_pts{0};
	bool packet_ready{false};

	/* Allocate (or reallocate) the input DMA buffer for width×height YUV420.
	 * Returns true on success. */
	bool allocInputBuffer(uint32_t w, uint32_t h);

	/* Free the input DMA buffer. Safe to call when not allocated. */
	void freeInputBuffer();

	/* Query plane geometry from the allocated input buffer and cache it. */
	void updatePlaneParams();
};
