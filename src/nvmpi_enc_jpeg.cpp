/*
 * nvmpi_enc_jpeg.cpp — hardware MJPEG/JPEG encoder via NvJPEGEncoder.
 *
 * This is a synchronous, per-frame encode path using the Tegra NVJPG engine.
 * It does NOT use V4L2 M2M — there is no DQ thread, no OUTPUT/CAPTURE plane
 * negotiation, and no packet pool.
 *
 * Lifecycle:
 *   nvmpi_create_jpeg_encoder()         — create NvJPEGEncoder
 *   nvmpi_jpeg_encoder_put_frame()      — copy YUV420 → DMA buf → encodeFromFd
 *   nvmpi_jpeg_encoder_get_packet()     — retrieve staged JPEG output
 *   nvmpi_jpeg_encoder_close()          — destroy everything
 *
 * The encoder is simpler than the decoder: no frame pool, no VIC transform.
 * The caller's YUV420 frame is memcpy'd into a pitch-linear DMA buffer
 * (which encodeFromFd expects), then the NVJPG engine produces baseline JPEG.
 *
 * Hardware constraint: Orin Nano has no NVJPG encode capability. The
 * NvJPEGEncoder::createJPEGEncoder() call returns NULL on such modules,
 * which we propagate as a clean creation failure.
 *
 * Allocated by nvmpi_create_jpeg_encoder(); freed by nvmpi_jpeg_encoder_close().
 */
#include "nvmpi_enc_jpeg_internal.h"

/* ------------------------------------------------------------------ */
/* Input DMA buffer management                                         */
/* ------------------------------------------------------------------ */

/*
 * Allocate a pitch-linear YUV420 DMA buffer at width×height.
 * This is the input surface for encodeFromFd. A single buffer suffices
 * because encoding is synchronous — we fill it, encode, then reuse.
 *
 * Allocated here; freed by freeInputBuffer().
 */
bool nvmpictx_jpegenc::allocInputBuffer(uint32_t w, uint32_t h)
{
	/* Free any existing buffer first (resolution change path). */
	freeInputBuffer();

	NvBufferCreateParams params;
	memset(&params, 0, sizeof(params));
	params.width = w;
	params.height = h;
	params.layout = NvBufferLayout_Pitch;
	/* encodeFromFd supports YUV420 and NV12; we use YUV420 (planar). */
	params.colorFormat = NvBufferColorFormat_YUV420;
#ifdef WITH_NVUTILS
	params.memType = NVBUF_MEM_SURFACE_ARRAY;
	params.memtag = NvBufSurfaceTag_VIDEO_CONVERT;
#else
	params.payloadType = NvBufferPayload_SurfArray;
	params.nvbuf_tag = NvBufferTag_VIDEO_ENC;
#endif

	if (!input_buf.alloc(params)) {
		std::cerr << "[libnvmpi][jpegenc][E]: failed to allocate input DMA buffer "
		          << w << "x" << h << std::endl;
		return false;
	}

	input_buf_allocated = true;
	width = w;
	height = h;

	updatePlaneParams();
	return true;
}

/*
 * Free the input DMA buffer. Safe to call when not allocated.
 * Allocated by allocInputBuffer(); freed here.
 */
void nvmpictx_jpegenc::freeInputBuffer()
{
	if (input_buf_allocated) {
		input_buf.destroy();
		input_buf_allocated = false;
	}
	num_planes = 0;
	memset(plane_pitch, 0, sizeof(plane_pitch));
	memset(plane_height, 0, sizeof(plane_height));
	memset(plane_linedata, 0, sizeof(plane_linedata));
}

/*
 * Query actual plane geometry from the allocated input buffer and cache it.
 * These values drive the memcpy from the caller's nvFrame into the DMA
 * buffer in put_frame.
 */
void nvmpictx_jpegenc::updatePlaneParams()
{
	if (!input_buf_allocated) return;

#ifdef WITH_NVUTILS
	NvBufSurfacePlaneParams parm;
	NvBufSurfaceParams surf_params = input_buf.dst_dma_surface->surfaceList[0];
	parm = surf_params.planeParams;
#else
	NvBufferParams parm;
	int ret = NvBufferGetParams(input_buf.dst_dma_fd, &parm);
	if (ret < 0) {
		std::cerr << "[libnvmpi][jpegenc][E]: NvBufferGetParams failed"
		          << std::endl;
		return;
	}
#endif

	num_planes = parm.num_planes;
	for (unsigned int i = 0; i < num_planes; i++)
	{
		plane_pitch[i] = parm.pitch[i];
		plane_height[i] = parm.height[i];
#ifdef WITH_NVUTILS
		plane_linedata[i] = parm.width[i] * parm.bytesPerPix[i];
#else
		/* Legacy API: YUV420 planar — all planes are 1 byte per pixel. */
		plane_linedata[i] = parm.width[i];
#endif
	}
}

/* ------------------------------------------------------------------ */
/* Copy caller's nvFrame into the DMA input buffer.                    */
/* Reverse of the decoder's jpegCopyToFrame — writes into DMA instead  */
/* of reading from it.                                                 */
/* ------------------------------------------------------------------ */

static int jpegCopyFromFrame(nvmpictx_jpegenc *ctx, const nvFrame *frame)
{
	int ret;

	for (unsigned int plane = 0; plane < ctx->num_planes; plane++)
	{
		char *dataDst;
#ifdef WITH_NVUTILS
		NvBufSurface *nvbuf_surf = ctx->input_buf.dst_dma_surface;
		ret = NvBufSurfaceMap(nvbuf_surf, 0, plane, NVBUF_MAP_READ_WRITE);
		if (ret != 0) {
			std::cerr << "[libnvmpi][jpegenc][E]: NvBufSurfaceMap failed plane="
			          << plane << std::endl;
			return ret;
		}
		dataDst = (char *)nvbuf_surf->surfaceList[0].mappedAddr.addr[plane];
#else
		void *pdst_data;
		ret = NvBufferMemMap(ctx->input_buf.dst_dma_fd, plane,
		                     NvBufferMem_Read_Write, &pdst_data);
		if (ret != 0) {
			std::cerr << "[libnvmpi][jpegenc][E]: NvBufferMemMap failed plane="
			          << plane << std::endl;
			return ret;
		}
		dataDst = (char *)pdst_data;
#endif

		const char *dataSrc = (const char *)frame->payload[plane];
		unsigned int srcStride = frame->linesize[plane];
		unsigned int dstStride = ctx->plane_pitch[plane];
		/* Copy only the valid data width — avoid writing past the frame's
		 * actual data if the DMA buffer pitch is wider. */
		unsigned int copySz = ctx->plane_linedata[plane];

		/* Bounds check: do not read beyond what the caller provided. */
		if (srcStride < copySz)
			copySz = srcStride;

		for (unsigned int i = 0; i < ctx->plane_height[plane]; i++)
		{
			memcpy(dataDst, dataSrc, copySz);
			dataDst += dstStride;
			dataSrc += srcStride;
		}

		/* Sync the written data to hardware before encodeFromFd reads it. */
#ifdef WITH_NVUTILS
		NvBufSurfaceSyncForDevice(nvbuf_surf, 0, plane);
		NvBufSurfaceUnMap(nvbuf_surf, 0, plane);
#else
		NvBufferMemSyncForDevice(ctx->input_buf.dst_dma_fd, plane, &pdst_data);
		NvBufferMemUnMap(ctx->input_buf.dst_dma_fd, plane, &pdst_data);
#endif
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

/*
 * Create a JPEG encoder context.
 *
 * Opens the Tegra NVJPG encode engine via NvJPEGEncoder::createJPEGEncoder().
 * No V4L2 device is used — JPEG encode is backed by libnvjpeg (patched
 * libjpeg-8b with TEGRA_ACCELERATE).
 *
 * The input DMA buffer is NOT allocated here — it is lazily allocated on
 * the first put_frame when the frame dimensions become known.
 *
 * Returns NULL on NvJPEGEncoder creation failure. This is the expected
 * path on Orin Nano which has no NVJPG encode capability (NVIDIA-confirmed:
 * Keylost#43). The FFmpeg wrapper reports a clear error with fallback
 * suggestion when this returns NULL.
 *
 * quality: libjpeg quality 1-100. Clamped to [1, 100].
 *
 * Allocated here; freed by nvmpi_jpeg_encoder_close().
 */
nvmpictx* nvmpi_create_jpeg_encoder(int quality)
{
	nvmpictx_jpegenc *ctx = new nvmpictx_jpegenc();
	memset(ctx->plane_pitch, 0, sizeof(ctx->plane_pitch));
	memset(ctx->plane_height, 0, sizeof(ctx->plane_height));
	memset(ctx->plane_linedata, 0, sizeof(ctx->plane_linedata));

	/* Clamp quality to valid libjpeg range. */
	if (quality < 1) quality = 1;
	if (quality > 100) quality = 100;
	ctx->quality = quality;

	/* Allocated here; freed in nvmpi_jpeg_encoder_close() via delete.
	 * Returns NULL on Orin Nano (no NVJPG encode engine). */
	ctx->jpegenc = NvJPEGEncoder::createJPEGEncoder("jpegenc");
	if (!ctx->jpegenc) {
		std::cerr << "[libnvmpi][jpegenc][E]: NvJPEGEncoder creation failed. "
		          << "This module may not have NVJPG encode capability "
		          << "(e.g. Orin Nano). Use software mjpeg encoder as fallback."
		          << std::endl;
		delete ctx;
		return NULL;
	}

	/* Cast to opaque C API handle (nvmpictx* from nvmpi.h). */
	return reinterpret_cast<nvmpictx*>(ctx);
}

/*
 * Encode one YUV420 frame to JPEG synchronously.
 *
 * Flow:
 *   1. Validate input; NULL frame signals EOS.
 *   2. On resolution change: (re)allocate input DMA buffer.
 *   3. Copy caller's nvFrame planes into the DMA buffer.
 *   4. Call encodeFromFd() — NVJPG engine produces baseline JPEG.
 *   5. Stage the JPEG output for retrieval via get_packet().
 *
 * Returns 0 on success, -1 on error.
 */
int nvmpi_jpeg_encoder_put_frame(nvmpictx *handle, nvFrame *frame)
{
	if (!handle)
		return -1;

	nvmpictx_jpegenc *ctx = reinterpret_cast<nvmpictx_jpegenc*>(handle);

	/* EOS marker: NULL frame signals end of stream. */
	if (!frame) {
		ctx->eos.store(true);
		return 0;
	}

	if (ctx->eos.load())
		return -1;

	/* Validate frame dimensions against hardware limits. */
	if (frame->width == 0 || frame->height == 0 ||
	    frame->width > NVJPEG_ENC_MAX_DIM || frame->height > NVJPEG_ENC_MAX_DIM) {
		std::cerr << "[libnvmpi][jpegenc][E]: frame dimensions "
		          << frame->width << "x" << frame->height
		          << " out of range (max " << NVJPEG_ENC_MAX_DIM << ")"
		          << std::endl;
		return -1;
	}

	/* Validate that frame planes are non-NULL. */
	if (!frame->payload[0] || !frame->payload[1] || !frame->payload[2]) {
		std::cerr << "[libnvmpi][jpegenc][E]: null frame plane pointer"
		          << std::endl;
		return -1;
	}

	/* Drop any un-consumed previous output. The FFmpeg wrapper should
	 * always call get_packet between put_frame calls, but guard against
	 * misuse. */
	if (ctx->out_buf) {
		free(ctx->out_buf);
		ctx->out_buf = nullptr;
		ctx->out_buf_size = 0;
		ctx->packet_ready = false;
	}

	/* ---- (Re)allocate input DMA buffer on resolution change ---- */
	if (frame->width != ctx->width || frame->height != ctx->height) {
		if (!ctx->allocInputBuffer(frame->width, frame->height)) {
			std::cerr << "[libnvmpi][jpegenc][E]: input buffer allocation failed"
			          << std::endl;
			return -1;
		}
	}

	/* ---- Copy caller's frame into DMA buffer ---- */
	int ret = jpegCopyFromFrame(ctx, frame);
	if (ret != 0) {
		std::cerr << "[libnvmpi][jpegenc][E]: frame copy to DMA buffer failed"
		          << std::endl;
		return -1;
	}

	/* ---- Hardware encode ---- */

	/* Pre-allocate output buffer for encodeFromFd.
	 * The Tegra-patched libjpeg passes out_buf_size to
	 * jpeg_set_hardware_acceleration_parameters_enc() as the HW DMA output
	 * buffer size.  When out_buf_size is 0 the HW path allocates a zero-size
	 * buffer and segfaults when the encoder writes into it (observed on
	 * Orin at ≥640x480).  Pre-allocating a worst-case buffer avoids this;
	 * libjpeg will reallocate if the estimate is too small.
	 *
	 * Worst-case JPEG: uncompressed YUV420 = w*h*1.5, plus JPEG headers.
	 * We use w*h*2 + 65536 as a safe upper bound.
	 *
	 * Integer overflow protection: width/height validated against
	 * NVJPEG_ENC_MAX_DIM (16384) above, so the product fits uint64. */
	unsigned long jpeg_alloc = (unsigned long)ctx->width * ctx->height * 2 + 65536;
	unsigned char *jpeg_buf = (unsigned char *)malloc(jpeg_alloc);
	if (!jpeg_buf) {
		std::cerr << "[libnvmpi][jpegenc][E]: failed to allocate JPEG output buffer ("
		          << jpeg_alloc << " bytes)" << std::endl;
		return -1;
	}
	/* Zero-initialize — prevent information leak if encode partially fills
	 * the buffer and the caller reads beyond the valid JPEG data. */
	memset(jpeg_buf, 0, jpeg_alloc);
	unsigned long jpeg_size = jpeg_alloc;

	/* encodeFromFd: the Tegra NVJPG engine encodes from the DMA fd.
	 * out_buf/out_buf_size are in/out — libjpeg may reallocate if the
	 * pre-allocated buffer is too small.  Caller must free() the final
	 * *out_buf pointer (which may differ from the original allocation).
	 *
	 * Parameters:
	 *   fd          — DMA buffer fd containing YUV420 frame
	 *   color_space — JCS_YCbCr (YUV420, the only supported format)
	 *   out_buf     — pointer to output buffer (pre-allocated, may be reallocated)
	 *   out_buf_size — in: buffer capacity; out: actual JPEG size
	 *   quality     — libjpeg quality 1-100
	 */
	ret = ctx->jpegenc->encodeFromFd(ctx->input_buf.dst_dma_fd,
	                                 JCS_YCbCr,
	                                 &jpeg_buf, jpeg_size,
	                                 ctx->quality);
	if (ret < 0) {
		std::cerr << "[libnvmpi][jpegenc][E]: encodeFromFd failed" << std::endl;
		if (jpeg_buf) {
			/* Zero out before freeing — may contain partial frame data. */
			memset(jpeg_buf, 0, jpeg_size);
			free(jpeg_buf);
		}
		return -1;
	}

	/* Stage output for get_packet. */
	ctx->out_buf = jpeg_buf;
	ctx->out_buf_size = jpeg_size;
	ctx->out_pts = frame->timestamp;
	ctx->packet_ready = true;

	return 0;
}

/*
 * Retrieve the encoded JPEG packet.
 *
 * Copies the staged JPEG output into the caller's nvPacket and frees
 * the internal buffer. Returns 0 on success, -1 if no packet is ready,
 * -2 on EOS with no packet.
 */
int nvmpi_jpeg_encoder_get_packet(nvmpictx *handle, nvPacket *packet)
{
	if (!handle || !packet)
		return -1;

	nvmpictx_jpegenc *ctx = reinterpret_cast<nvmpictx_jpegenc*>(handle);

	if (!ctx->packet_ready) {
		if (ctx->eos.load())
			return -2;
		return -1;
	}

	/* Copy JPEG data to caller's packet buffer. */
	packet->payload_size = ctx->out_buf_size;
	if (packet->payload && ctx->out_buf_size > 0) {
		memcpy(packet->payload, ctx->out_buf, ctx->out_buf_size);
	}
	packet->pts = ctx->out_pts;
	/* JPEG frames are always keyframes. */
	packet->flags = 1;

	/* Free the internal JPEG buffer.
	 * Zero out before freeing — encoded frame data. */
	if (ctx->out_buf) {
		memset(ctx->out_buf, 0, ctx->out_buf_size);
		free(ctx->out_buf);
		ctx->out_buf = nullptr;
	}
	ctx->out_buf_size = 0;
	ctx->packet_ready = false;

	return 0;
}

/*
 * Destroy the JPEG encoder context and free all resources.
 *
 * Teardown order:
 *   1. Set EOS.
 *   2. Free any staged output buffer.
 *   3. Free input DMA buffer.
 *   4. Delete NvJPEGEncoder and context.
 *
 * The handle is invalid after this call.
 */
int nvmpi_jpeg_encoder_close(nvmpictx *handle)
{
	if (!handle)
		return -1;

	nvmpictx_jpegenc *ctx = reinterpret_cast<nvmpictx_jpegenc*>(handle);

	ctx->eos.store(true);

	/* Free staged output buffer.
	 * Zero out before freeing — may contain encoded frame data. */
	if (ctx->out_buf) {
		memset(ctx->out_buf, 0, ctx->out_buf_size);
		free(ctx->out_buf);
		ctx->out_buf = nullptr;
		ctx->out_buf_size = 0;
	}

	/* Free input DMA buffer.
	 * Allocated by allocInputBuffer(); freed here. */
	ctx->freeInputBuffer();

	/* Allocated by nvmpi_create_jpeg_encoder() via createJPEGEncoder();
	 * freed here. Destructor calls jpeg_destroy_compress. */
	if (ctx->jpegenc) {
		delete ctx->jpegenc;
		ctx->jpegenc = nullptr;
	}

	delete ctx;
	return 0;
}
