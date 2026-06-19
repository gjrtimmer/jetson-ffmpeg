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
 * (see nvmpi_dec_api.cpp) but are simplified: no capture thread, no dynamic
 * resolution events (resolution is detected from each decoded frame).
 *
 * Allocated by nvmpi_create_jpeg_decoder(); freed by nvmpi_jpeg_decoder_close().
 */
#include "nvmpi_dec_jpeg_internal.h"

/* ------------------------------------------------------------------ */
/* Frame pool management (mirrors nvmpi_dec_api.cpp patterns)          */
/* ------------------------------------------------------------------ */

/*
 * Allocate pitch-linear DMA buffers for the VIC transform output.
 * These are the destination buffers that hold decoded YUV420 frames.
 * Allocated by nvmpi_create_jpeg_decoder(); freed by nvmpi_jpeg_decoder_close().
 */
void nvmpictx_jpeg::initFramePool(uint32_t width, uint32_t height)
{
	if (frame_pool_size <= 0) {
		std::cerr << "[libnvmpi][jpeg][E]: initFramePool: frame_pool_size="
		          << frame_pool_size << " is invalid" << std::endl;
		return;
	}

	/* Tear down existing pool on resolution change. */
	if (!allocatedFrameBufs.empty()) {
		deinitFramePool();
	}

	NvBufferCreateParams input_params;
	memset(&input_params, 0, sizeof(input_params));
	input_params.width = width;
	input_params.height = height;
	input_params.layout = NvBufferLayout_Pitch;
	/* JPEG always decodes to YUV420; VIC transforms to planar YUV420. */
	input_params.colorFormat = NvBufferColorFormat_YUV420;
#ifdef WITH_NVUTILS
	input_params.memType = NVBUF_MEM_SURFACE_ARRAY;
	input_params.memtag = NvBufSurfaceTag_VIDEO_CONVERT;
#else
	input_params.payloadType = NvBufferPayload_SurfArray;
	input_params.nvbuf_tag = NvBufferTag_VIDEO_DEC;
#endif

	for (int i = 0; i < frame_pool_size; i++)
	{
		nvmpi_frame_buffer* fb = new nvmpi_frame_buffer();
		if (!fb->alloc(input_params)) {
			/* Allocation failure: clean up what we have. */
			std::cerr << "[libnvmpi][jpeg][E]: failed to allocate DMA buffer "
			          << i << "/" << frame_pool_size << std::endl;
			delete fb;
			break;
		}
		allocatedFrameBufs.push_back(fb);
		framePool->qEmptyBuf(fb);
	}

	last_width = width;
	last_height = height;
}

/*
 * Destroy all frame buffers and drain both pool queues.
 * Safe to call on an empty pool. Uses the allocatedFrameBufs vector
 * (not the queues) to ensure no buffer is missed.
 * Allocated by initFramePool(); freed here.
 */
void nvmpictx_jpeg::deinitFramePool()
{
	while (framePool->dqEmptyBuf()) {}
	while (framePool->dqFilledBuf()) {}

	for (auto* fb : allocatedFrameBufs)
	{
		fb->destroy();
		delete fb;
	}
	allocatedFrameBufs.clear();
}

/*
 * Query actual plane geometry from one allocated buffer and cache it.
 * These values drive the memcpy in get_frame. Uses peekEmptyBuf() —
 * safe only when no consumer is running.
 * Caller must ensure framePool has at least one buffer.
 */
void nvmpictx_jpeg::updateFrameSizeParams()
{
	nvmpi_frame_buffer* fb = framePool->peekEmptyBuf();
	if (!fb) return;

#ifdef WITH_NVUTILS
	NvBufSurfacePlaneParams parm;
	NvBufSurfaceParams dst_params = fb->dst_dma_surface->surfaceList[0];
	parm = dst_params.planeParams;
#else
	NvBufferParams parm;
	int ret = NvBufferGetParams(fb->dst_dma_fd, &parm);
	if (ret < 0) {
		std::cerr << "[libnvmpi][jpeg][E]: NvBufferGetParams failed" << std::endl;
		return;
	}
#endif

	num_planes = parm.num_planes;
	for (unsigned int i = 0; i < num_planes; i++)
	{
		frame_linesize[i] = parm.pitch[i];
		frame_height[i] = parm.height[i];
#ifdef WITH_NVUTILS
		frame_linedatasize[i] = parm.width[i] * parm.bytesPerPix[i];
#else
		/* Legacy API: YUV420 planar — all planes are 1 byte per pixel. */
		frame_linedatasize[i] = parm.width[i];
#endif
	}
}

/*
 * Set up VIC transform rects for pass-through (no scaling).
 * JPEG output is block-linear; VIC converts to pitch-linear.
 */
void nvmpictx_jpeg::updateTransformParams(uint32_t width, uint32_t height)
{
	src_rect.top = 0;
	src_rect.left = 0;
	src_rect.width = width;
	src_rect.height = height;
	dest_rect.top = 0;
	dest_rect.left = 0;
	dest_rect.width = width;
	dest_rect.height = height;

	memset(&transform_params, 0, sizeof(transform_params));
	transform_params.transform_flag = NVBUFFER_TRANSFORM_FILTER;
	transform_params.transform_filter = NvBufSurfTransformInter_Nearest;
#ifdef WITH_NVUTILS
	transform_params.src_rect = &src_rect;
	transform_params.dst_rect = &dest_rect;
#else
	transform_params.src_rect = src_rect;
	transform_params.dst_rect = dest_rect;
	transform_params.session = session;
#endif
}

/* ------------------------------------------------------------------ */
/* Copy decoded pixels from a DMA buffer to the caller's nvFrame.      */
/* Same pattern as the V4L2 decoder's copyNvBufToFrame().              */
/* ------------------------------------------------------------------ */

static int jpegCopyToFrame(nvmpictx_jpeg* ctx, nvmpi_frame_buffer *buf, nvFrame* frame)
{
	int ret;

	for (unsigned int plane = 0; plane < ctx->num_planes; plane++)
	{
		char *dataSrc;
#ifdef WITH_NVUTILS
		NvBufSurface *nvbuf_surf = buf->dst_dma_surface;
		ret = NvBufSurfaceMap(nvbuf_surf, 0, plane, NVBUF_MAP_READ_WRITE);
		NvBufSurfaceSyncForCpu(nvbuf_surf, 0, plane);
		dataSrc = (char *)nvbuf_surf->surfaceList[0].mappedAddr.addr[plane];
#else
		void *psrc_data;
		ret = NvBufferMemMap(buf->dst_dma_fd, plane, NvBufferMem_Read_Write, &psrc_data);
		NvBufferMemSyncForCpu(buf->dst_dma_fd, plane, &psrc_data);
		dataSrc = (char *)psrc_data;
#endif
		if (ret != 0) {
			std::cerr << "[libnvmpi][jpeg][E]: NvBufferMap failed" << std::endl;
			return ret;
		}

		char *dataDst = (char *)frame->payload[plane];
		unsigned int dstStride = frame->linesize[plane];
		unsigned int srcStride = ctx->frame_linesize[plane];
		unsigned int copySz = ctx->frame_linedatasize[plane];

		for (unsigned int i = 0; i < ctx->frame_height[plane]; i++)
		{
			memcpy(dataDst, dataSrc, copySz);
			dataDst += dstStride;
			dataSrc += srcStride;
		}

#ifdef WITH_NVUTILS
		NvBufSurfaceUnMap(nvbuf_surf, 0, plane);
#else
		NvBufferMemUnMap(buf->dst_dma_fd, plane, &psrc_data);
#endif
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Scan JPEG bitstream for SOF markers to detect progressive format.    */
/* Returns true if a progressive marker (SOF2, 0xFFC2) is found.       */
/* Scans the entire packet — SOF can appear after APP/DQT/DHT markers. */
/* ------------------------------------------------------------------ */

static bool jpegIsProgressive(const unsigned char *data, unsigned long size)
{
	/* Walk the JPEG marker structure. Each marker is 0xFF followed by
	 * a type byte. Markers with payloads (except RST and SOI/EOI) have
	 * a 2-byte big-endian length after the type byte. */
	unsigned long pos = 0;
	while (pos + 1 < size)
	{
		if (data[pos] != 0xFF) {
			pos++;
			continue;
		}
		/* Skip padding 0xFF bytes. */
		while (pos + 1 < size && data[pos + 1] == 0xFF)
			pos++;
		if (pos + 1 >= size) break;

		unsigned char marker = data[pos + 1];
		pos += 2;

		/* SOF2 = progressive DCT — not supported by NVJPG engine. */
		if (marker == JPEG_MARKER_SOF2)
			return true;

		/* SOF0 = baseline — supported, stop scanning. */
		if (marker == JPEG_MARKER_SOF0)
			return false;

		/* SOS (0xDA) = start of scan — SOF must have appeared by now. */
		if (marker == 0xDA)
			return false;

		/* Skip marker payload (2-byte big-endian length). */
		if (marker != 0x00 && marker != 0x01 &&
		    !(marker >= 0xD0 && marker <= 0xD9))
		{
			if (pos + 1 >= size) break;
			unsigned int len = (data[pos] << 8) | data[pos + 1];
			if (len < 2) break;
			pos += len;
		}
	}
	return false;
}

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
		std::cerr << "[libnvmpi][jpeg][E]: NvJPEGDecoder creation failed"
		          << std::endl;
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
		std::cerr << "[libnvmpi][jpeg][E]: null payload with non-zero size"
		          << std::endl;
		return -1;
	}

	if (ctx->eos.load())
		return -1;

	/* Reject progressive JPEG (SOF2) — NVJPG engine supports baseline only. */
	if (jpegIsProgressive(packet->payload, packet->payload_size)) {
		std::cerr << "[libnvmpi][jpeg][E]: progressive JPEG not supported by "
		          << "hardware decoder (SOF2 marker detected)" << std::endl;
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
		std::cerr << "[libnvmpi][jpeg][E]: decodeToFd failed" << std::endl;
		return -1;
	}

	/* Validate dimensions against hardware limits. */
	if (width == 0 || height == 0 || width > NVJPEG_MAX_DIM || height > NVJPEG_MAX_DIM) {
		std::cerr << "[libnvmpi][jpeg][E]: decoded dimensions " << width
		          << "x" << height << " out of range" << std::endl;
		return -1;
	}

	/* ---- Frame pool (re)allocation on resolution change ---- */
	if (width != ctx->last_width || height != ctx->last_height) {
		ctx->initFramePool(width, height);
		if (ctx->allocatedFrameBufs.empty()) {
			std::cerr << "[libnvmpi][jpeg][E]: frame pool allocation failed"
			          << std::endl;
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
		std::cerr << "[libnvmpi][jpeg][W]: frame pool exhausted, dropping frame"
		          << std::endl;
		return -1;
	}

	/* VIC transform: block-linear (NVJPG output) → pitch-linear (our pool).
	 * decode_fd is owned by libnvjpeg internals — do NOT destroy it. */
#ifdef WITH_NVUTILS
	/* Need NvBufSurface view of the decode fd for NvBufSurfTransform. */
	NvBufSurface *decode_surface = NULL;
	ret = NvBufSurfaceFromFd(decode_fd, (void**)&decode_surface);
	if (ret < 0) {
		std::cerr << "[libnvmpi][jpeg][E]: NvBufSurfaceFromFd failed"
		          << std::endl;
		ctx->framePool->qEmptyBuf(fb);
		return -1;
	}
	ret = NvBufSurfTransform(decode_surface, fb->dst_dma_surface,
	                         &(ctx->transform_params));
#else
	ret = NvBufferTransform(decode_fd, fb->dst_dma_fd,
	                        &(ctx->transform_params));
#endif
	if (ret != 0) {
		std::cerr << "[libnvmpi][jpeg][E]: VIC transform failed" << std::endl;
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
