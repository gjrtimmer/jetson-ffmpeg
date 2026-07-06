/*
 * nvmpi_dec_jpeg_ctx.cpp — JPEG decoder context helpers (NvJPEGDecoder-backed).
 *
 * Companion to nvmpi_dec_jpeg.cpp (the public API: create, put_packet,
 * get_frame, close). This file owns the nvmpictx_jpeg context methods and
 * the two former file-local helpers that the API functions call into:
 *
 *   - initFramePool()/deinitFramePool()  — allocate/free the pitch-linear
 *                                          VIC destination DMA buffers.
 *   - updateFrameSizeParams()            — cache dst_dma buffer plane
 *                                          geometry for the memcpy in
 *                                          jpegCopyToFrame().
 *   - updateTransformParams()            — cache VIC src/dst rects for
 *                                          pass-through (no scaling).
 *   - jpegCopyToFrame()                  — copy a decoded DMA buffer into
 *                                          the caller's nvFrame planes.
 *   - jpegIsProgressive()                — SOF2 marker scan; progressive
 *                                          JPEG is rejected (HW decodes
 *                                          baseline only).
 *
 * jpegCopyToFrame() and jpegIsProgressive() are declared (non-static) in
 * nvmpi_dec_jpeg_internal.h because nvmpi_dec_jpeg.cpp calls them from a
 * separate translation unit.
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
		NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "jpeg", "initFramePool: frame_pool_size=%d is invalid", frame_pool_size);
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
			NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "jpeg", "failed to allocate DMA buffer %d/%d", i, frame_pool_size);
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
		NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "jpeg", "NvBufferGetParams failed");
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

int jpegCopyToFrame(nvmpictx_jpeg* ctx, nvmpi_frame_buffer *buf, nvFrame* frame)
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
			NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "jpeg", "NvBufferMap failed");
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

bool jpegIsProgressive(const unsigned char *data, unsigned long size)
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
