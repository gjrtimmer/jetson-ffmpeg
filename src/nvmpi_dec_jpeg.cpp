/*
 * nvmpi_dec_jpeg.cpp — hardware MJPEG/JPEG decoder via NvJPEGDecoder.
 *
 * This is a synchronous, per-frame decode path using the Tegra NVJPG engine.
 * It does NOT use V4L2 M2M — there is no capture thread, no resolution-change
 * events, and no OUTPUT/CAPTURE plane negotiation.
 *
 * Lifecycle:
 *   nvmpi_create_jpeg_decoder()       — create NvJPEGDecoder + frame pool
 *   nvmpi_jpeg_decoder_put_packet()   — decode one JPEG → VIC transform → pool
 *   nvmpi_jpeg_decoder_get_frame()    — pop from pool, copy to caller
 *   nvmpi_jpeg_decoder_close()        — destroy everything
 *
 * The frame pool and VIC transform follow the same pattern as the V4L2 decoder
 * (see nvmpi_dec_api.cpp / nvmpi_dec_init.cpp) but are simplified: no capture
 * thread, no dynamic resolution events (resolution is detected from each
 * decoded frame).
 *
 * Companion file:
 *   nvmpi_dec_jpeg_ctx.cpp — nvmpictx_jpeg context methods (frame pool,
 *                            transform param setup) and the jpegCopyToFrame/
 *                            jpegIsProgressive helpers used below.
 *
 * Allocated by nvmpi_create_jpeg_decoder(); freed by nvmpi_jpeg_decoder_close().
 */
#include "nvmpi_dec_jpeg_internal.h"
#include <pthread.h>

/* Defined in nvmpi_vic.cpp — serializes NvBufSurfTransform calls. */
extern pthread_mutex_t nvmpi_transform_mutex;

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

/*
 * Create a JPEG decoder context.
 *
 * Opens the Tegra NVJPG engine via NvJPEGDecoder::createJPEGDecoder().
 * No V4L2 device is used — JPEG decode is backed by libnvjpeg (patched
 * libjpeg-8b with TEGRA_ACCELERATE).
 *
 * The frame pool is NOT allocated here — it is lazily allocated on the
 * first put_packet when the image dimensions become known.
 *
 * Returns NULL on NvJPEGDecoder creation failure.
 *
 * Allocated here; freed by nvmpi_jpeg_decoder_close().
 */
nvmpictx* nvmpi_create_jpeg_decoder(void)
{
	nvmpictx_jpeg *ctx = new nvmpictx_jpeg();
	memset(ctx->frame_linesize, 0, sizeof(ctx->frame_linesize));
	memset(ctx->frame_height, 0, sizeof(ctx->frame_height));
	memset(ctx->frame_linedatasize, 0, sizeof(ctx->frame_linedatasize));

	/* Allocated here; freed in nvmpi_jpeg_decoder_close() via delete. */
	ctx->jpegdec = NvJPEGDecoder::createJPEGDecoder("jpegdec");
	if (!ctx->jpegdec) {
		NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "jpeg", "NvJPEGDecoder creation failed");
		delete ctx;
		return NULL;
	}

	/* Allocated here; freed in nvmpi_jpeg_decoder_close() via delete. */
	ctx->framePool = new NVMPI_bufPool<nvmpi_frame_buffer*>();

#ifndef WITH_NVUTILS
	/* Legacy path: create NvBuffer session for VIC transforms. */
	ctx->session = NvBufferSessionCreate();
#endif

	/* Cast to opaque C API handle (nvmpictx* from nvmpi.h). */
	return reinterpret_cast<nvmpictx*>(ctx);
}

/*
 * Decode one JPEG frame synchronously.
 *
 * Flow:
 *   1. Validate input and check for EOS (payload_size == 0).
 *   2. Scan for progressive JPEG (SOF2) — reject with error.
 *   3. decodeToFd() — NVJPG engine decodes to block-linear DMA fd.
 *   4. On resolution change: reallocate frame pool.
 *   5. VIC transform: block-linear → pitch-linear destination buffer.
 *   6. Queue filled buffer for get_frame().
 *
 * Returns 0 on success, -1 on error.
 */
int nvmpi_jpeg_decoder_put_packet(nvmpictx* handle, nvPacket* packet)
{
	if (!handle || !packet)
		return -1;

	nvmpictx_jpeg *ctx = reinterpret_cast<nvmpictx_jpeg*>(handle);

	/* EOS marker: zero-size packet signals end of stream. */
	if (packet->payload_size == 0) {
		ctx->eos.store(true);
		return 0;
	}

	if (!packet->payload) {
		NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "jpeg", "null payload with non-zero size");
		return -1;
	}

	if (ctx->eos.load())
		return -1;

	/* Reject progressive JPEG (SOF2) — NVJPG engine supports baseline only. */
	if (jpegIsProgressive(packet->payload, packet->payload_size)) {
		NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "jpeg", "progressive JPEG not supported by hardware decoder (SOF2 marker detected)");
		return -1;
	}

	/* ---- Hardware decode ---- */
	int decode_fd = -1;
	uint32_t pixfmt = 0, width = 0, height = 0;

	int ret = ctx->jpegdec->decodeToFd(
		decode_fd,
		packet->payload,
		packet->payload_size,
		pixfmt, width, height);

	if (ret != 0) {
		NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "jpeg", "decodeToFd failed");
		return -1;
	}

	/* Validate dimensions against hardware limits. */
	if (width == 0 || height == 0 || width > NVJPEG_MAX_DIM || height > NVJPEG_MAX_DIM) {
		NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "jpeg", "decoded dimensions %ux%u out of range", width, height);
		return -1;
	}

	/* ---- Frame pool (re)allocation on resolution change ---- */
	if (width != ctx->last_width || height != ctx->last_height) {
		ctx->initFramePool(width, height);
		if (ctx->allocatedFrameBufs.empty()) {
			NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "jpeg", "frame pool allocation failed");
			return -1;
		}
		ctx->updateFrameSizeParams();
		ctx->updateTransformParams(width, height);
	}

	/* ---- VIC transform: block-linear decode_fd → pitch-linear pool buf ---- */
	nvmpi_frame_buffer* fb = ctx->framePool->dqEmptyBuf(
		std::chrono::milliseconds(500));
	if (!fb) {
		/* Backpressure: consumer hasn't drained frames fast enough. */
		NVMPI_LOG_SUB(NVMPI_LOG_WARN, "jpeg", "frame pool exhausted, dropping frame");
		return -1;
	}

	/* VIC transform: block-linear (NVJPG output) → pitch-linear (our pool).
	 * decode_fd is owned by libnvjpeg internals — do NOT destroy it.
	 * Lock the global transform mutex for consistency with the capture
	 * thread path (nvmpi_dec_capture.cpp) — JPEG decode is synchronous
	 * so contention is rare, but a VIC filter running on the same stream
	 * would interleave calls from the filter thread. */
#ifdef WITH_NVUTILS
	/* Need NvBufSurface view of the decode fd for NvBufSurfTransform. */
	NvBufSurface *decode_surface = NULL;
	ret = NvBufSurfaceFromFd(decode_fd, (void**)&decode_surface);
	if (ret < 0) {
		NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "jpeg", "NvBufSurfaceFromFd failed");
		ctx->framePool->qEmptyBuf(fb);
		return -1;
	}
	pthread_mutex_lock(&nvmpi_transform_mutex);
	NvBufSurfTransformSetSessionParams(&(ctx->session));
	ret = NvBufSurfTransform(decode_surface, fb->dst_dma_surface,
	                         &(ctx->transform_params));
	pthread_mutex_unlock(&nvmpi_transform_mutex);
#else
	pthread_mutex_lock(&nvmpi_transform_mutex);
	ret = NvBufferTransform(decode_fd, fb->dst_dma_fd,
	                        &(ctx->transform_params));
	pthread_mutex_unlock(&nvmpi_transform_mutex);
#endif
	if (ret != 0) {
		NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "jpeg", "VIC transform failed");
		ctx->framePool->qEmptyBuf(fb);
		return -1;
	}

	/* Carry pts through. */
	fb->timestamp = packet->pts;

	/* Hand the filled frame to the consumer (get_frame). */
	ctx->framePool->qFilledBuf(fb);

	return 0;
}

/*
 * Retrieve one decoded JPEG frame.
 *
 * Pops from the filled pool, copies pixel data to the caller's nvFrame
 * planes, and recycles the pool buffer.
 *
 * When wait is true: blocks up to 500ms. Returns -1 on timeout or EOS.
 */
int nvmpi_jpeg_decoder_get_frame(nvmpictx* handle, nvFrame* frame, bool wait)
{
	if (!handle || !frame)
		return -1;

	nvmpictx_jpeg *ctx = reinterpret_cast<nvmpictx_jpeg*>(handle);

	nvmpi_frame_buffer* fb;

	if (wait)
		fb = ctx->framePool->dqFilledBuf(std::chrono::milliseconds(500));
	else
		fb = ctx->framePool->dqFilledBuf();

	if (!fb)
		return -1;

	int ret = jpegCopyToFrame(ctx, fb, frame);
	frame->timestamp = fb->timestamp;
	frame->width = ctx->last_width;
	frame->height = ctx->last_height;
	frame->type = ctx->out_pixfmt;

	ctx->framePool->qEmptyBuf(fb);
	return ret;
}

/*
 * Destroy the JPEG decoder context and free all resources.
 *
 * Teardown order:
 *   1. Set EOS and shutdown pool (unblock any blocked get_frame).
 *   2. Destroy VIC session (legacy path).
 *   3. Destroy frame pool DMA buffers.
 *   4. Delete pool, NvJPEGDecoder, and context.
 *
 * The handle is invalid after this call.
 */
int nvmpi_jpeg_decoder_close(nvmpictx* handle)
{
	if (!handle)
		return -1;

	nvmpictx_jpeg *ctx = reinterpret_cast<nvmpictx_jpeg*>(handle);

	ctx->eos.store(true);

	/* Unblock any consumer waiting in get_frame. */
	if (ctx->framePool)
		ctx->framePool->shutdown();

#ifndef WITH_NVUTILS
	/* Destroy VIC transform session (legacy buffer API only).
	 * Allocated in nvmpi_create_jpeg_decoder(); freed here. */
	NvBufferSessionDestroy(ctx->session);
#endif

	/* Destroy all DMA frame buffers.
	 * Allocated by initFramePool(); freed here. */
	if (ctx->framePool) {
		ctx->deinitFramePool();
		delete ctx->framePool;
		ctx->framePool = nullptr;
	}

	/* Allocated by nvmpi_create_jpeg_decoder() via createJPEGDecoder();
	 * freed here. Destructor calls jpeg_destroy_decompress. */
	if (ctx->jpegdec) {
		delete ctx->jpegdec;
		ctx->jpegdec = nullptr;
	}

	delete ctx;
	return 0;
}
