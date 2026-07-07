/*
 * nvmpi_enc_input.cpp — encoder frame submission (layer 1).
 *
 * Implements the frame-copy helper and both frame-submission variants of
 * the public C API in include/nvmpi.h on top of NVIDIA's V4L2 NvVideoEncoder
 * sample class. Consumed by the FFmpeg wrapper in
 * ffmpeg/dev/common/libavcodec/nvmpi_enc.c.
 *
 * V4L2 encoder model (M2M device, two queues — note the naming is from the
 * device's point of view, mirrored relative to the decoder):
 *   - OUTPUT plane  = input side: raw YUV frames go in.
 *   - CAPTURE plane = output side: encoded bitstream comes out.
 *
 * Data flow / threading:
 *   user thread:      nvmpi_encoder_put_frame() (or the DMA-BUF variant,
 *                     nvmpi_encoder_put_frame_fd()) copies or maps the
 *                     caller's frame into an OUTPUT-plane buffer and queues
 *                     it (blocks in dqBuffer once all buffers are in flight).
 *   DQ thread:        NvVideoEncoder's capture-plane dequeue thread invokes
 *                     encoder_capture_plane_dq_callback() (nvmpi_enc_output.cpp)
 *                     per encoded buffer.
 *
 * Companion files:
 *   nvmpi_enc_internal.h  — shared includes, defines, nvmpictx struct,
 *                           and forward declarations.
 *   nvmpi_enc_init.cpp    — nvmpi_create_encoder() (device open, format
 *                           negotiation, plane/stream setup).
 *   nvmpi_enc_output.cpp  — encoder_capture_plane_dq_callback and
 *                           setup_output_dmabuf (OUTPUT-plane DMA path).
 */
#include "nvmpi_enc_internal.h"

//Copy the caller's raw frame planes into a V4L2 OUTPUT-plane NvBuffer.
//Line-by-line memcpy because the caller's linesize and the hw buffer's
//stride generally differ; sets bytesused per plane so the driver knows the
//payload extent. The source frame is untouched (caller keeps ownership).
int copyFrameToNvBuf(nvFrame* frame, NvBuffer& buffer)
{
	uint32_t i, j;
	char *dataDst;
	char *dataSrc;
	for (i = 0; i < buffer.n_planes; i++)
	{
		NvBuffer::NvBufferPlane &plane = buffer.planes[i];
		unsigned int &frameLineSize = frame->linesize[i];
		size_t copySz = plane.fmt.bytesperpixel * plane.fmt.width;
		dataDst = (char *) plane.data;
		dataSrc = (char *) frame->payload[i];
		plane.bytesused = 0;
		for (j = 0; j < plane.fmt.height; j++)
		{
			memcpy(dataDst, dataSrc, copySz);
			dataDst += plane.fmt.stride;
			dataSrc += frameLineSize;
		}
		plane.bytesused = plane.fmt.stride * plane.fmt.height;
	}
	return 0;
}

//Public API: submit one raw frame for encoding (or EOS when frame==NULL).
//Buffer acquisition mirrors the decoder's put_packet: use each OUTPUT-plane
//buffer once by index, then block in dqBuffer(-1) until the encoder frees
//one. The frame is copied in, the pts (microseconds) goes into the V4L2
//timestamp, caches are synced for device access, and the buffer is queued.
//frame==NULL queues a zero-byte buffer = V4L2 EOS signal and sets
//ctx->flushing. Returns 0 on success, -2 if already flushing, negative on
//encoder error.
int nvmpi_encoder_put_frame(nvmpictx* ctx,nvFrame* frame)
{
	if(ctx->flushing.load(std::memory_order_acquire)) return -2;

	int ret;
	struct v4l2_buffer v4l2_buf;
	struct v4l2_plane planes[MAX_PLANES];
	NvBuffer *nvBuffer;

	memset(&v4l2_buf, 0, sizeof(v4l2_buf));
	memset(planes, 0, sizeof(planes));

	v4l2_buf.m.planes = planes;

	if(ctx->enc->isInError())
		return -1;

	if(ctx->index < ctx->enc->output_plane.getNumBuffers())
	{
		nvBuffer=ctx->enc->output_plane.getNthBuffer(ctx->index);
		v4l2_buf.index = ctx->index;
		ctx->index++;

#if (OUTPLANE_MEMTYPE == OUTPLANE_MEMTYPE_DMA)
		v4l2_buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
		v4l2_buf.memory = V4L2_MEMORY_DMABUF;
		// Map output plane buffer for memory type DMABUF.
		ret = ctx->enc->output_plane.mapOutputBuffers(v4l2_buf, ctx->output_plane_fd[v4l2_buf.index]);
		if (ret < 0)
		{
			NVMPI_LOG(NVMPI_LOG_ERROR, "Error while mapping buffer at output plane");
		}
#endif
	}
	else
	{
		/* Reclaim a used OUTPUT-plane buffer. In blocking mode (default),
		 * dqBuffer waits indefinitely (-1) until the encoder frees one.
		 * In non-blocking mode, timeout=0 returns immediately — if no
		 * buffer is available, return NVMPI_ERR_EAGAIN so the caller can
		 * drain encoded packets and retry. */
		int dq_timeout = ctx->blocking_mode ? -1 : 0;
		ret = ctx->enc->output_plane.dqBuffer(v4l2_buf, &nvBuffer, NULL, dq_timeout);
		if (ret < 0)
		{
			if (!ctx->blocking_mode) {
				/* Non-blocking: no buffer available right now */
				return NVMPI_ERR_EAGAIN;
			}
			NVMPI_LOG(NVMPI_LOG_ERROR, "Error DQing buffer at output plane");
			return -1;
		}
	}

	if(frame)
	{
		//normal frame: copy planes in and carry the pts on the buffer
		copyFrameToNvBuf(frame, *nvBuffer);
		v4l2_buf.flags |= V4L2_BUF_FLAG_TIMESTAMP_COPY;
		v4l2_buf.timestamp.tv_usec = frame->timestamp % 1000000;
		v4l2_buf.timestamp.tv_sec = frame->timestamp / 1000000;
	}
	else
	{
		//send EOS and flush
		//(a buffer queued with bytesused==0 tells the encoder to drain)
		ctx->flushing.store(true, std::memory_order_release);
		v4l2_buf.m.planes[0].m.userptr = 0;
		v4l2_buf.m.planes[0].bytesused = v4l2_buf.m.planes[1].bytesused = v4l2_buf.m.planes[2].bytesused = 0;
	}

	//needed for V4L2_MEMORY_MMAP and V4L2_MEMORY_DMABUF
	//flush CPU caches so the hw encoder sees the bytes just written
	for (uint32_t j = 0 ; j < nvBuffer->n_planes; j++)
	{
#ifdef WITH_NVUTILS
		NvBufSurface *nvbuf_surf = 0;
		ret = NvBufSurfaceFromFd (nvBuffer->planes[j].fd, (void**)(&nvbuf_surf));
		if (ret < 0)
		{
			NVMPI_LOG(NVMPI_LOG_ERROR, "Error while NvBufSurfaceFromFd");
		}
		ret = NvBufSurfaceSyncForDevice (nvbuf_surf, 0, j);
		if (ret < 0)
		{
			NVMPI_LOG(NVMPI_LOG_ERROR, "Error while NvBufSurfaceSyncForDevice at output plane for V4L2_MEMORY_DMABUF");
		}
#else
		ret = NvBufferMemSyncForDevice (nvBuffer->planes[j].fd, j, (void **)&nvBuffer->planes[j].data);
		if (ret < 0)
		{
			NVMPI_LOG(NVMPI_LOG_ERROR, "Error while NvBufferMemSyncForDevice at output plane for V4L2_MEMORY_DMABUF");
		}
#endif
	}

	//for V4L2_MEMORY_DMABUF only
#if (OUTPLANE_MEMTYPE == OUTPLANE_MEMTYPE_DMA)
	for (uint32_t j = 0; j < nvBuffer->n_planes; j++)
	{
		v4l2_buf.m.planes[j].bytesused = nvBuffer->planes[j].bytesused;
	}
#endif

	ret = ctx->enc->output_plane.qBuffer(v4l2_buf, NULL);
	if (ret < 0) {
		NVMPI_LOG(NVMPI_LOG_ERROR, "Error while queueing buffer at output plane (code=%d)", ret);
		return ret;
	}

	return 0;
}

//Public API: zero-copy variant of nvmpi_encoder_put_frame.
//Submits a raw frame to the encoder via an external DMA-BUF fd instead
//of copying pixel data from caller-owned memory.
//
//On NvUtils (JetPack 5+), the external fd's content is copied into a
//pre-allocated internal buffer via NvBufSurfaceCopy, then the internal
//buffer (1:1 mapped to its V4L2 slot) is queued. This avoids NvMap
//handle degradation that occurs when external fds rotate across V4L2
//buffer slots (pool size != slot count). The internal buffers were
//allocated and mapped in nvmpi_create_encoder().
//
//On legacy JetPack 4 (no NvUtils), the external fd is passed directly
//to V4L2 (original behavior — NvBufSurfaceCopy not available).
//
//Requires use_dmabuf=1 in nvEncParam at encoder creation time
//(sets ctx->dmabuf_external, which configures the OUTPUT plane for
//V4L2_MEMORY_DMABUF instead of MMAP).
//
//The fd must reference a pitch-linear NV12 DMA-BUF surface with
//matching dimensions (e.g. from nvmpi_surface_alloc_for_enc or from
//the VIC filter's output pool). The encoder does NOT take ownership —
//the caller may release the fd after this call returns.
//
//dmabuf_fd == -1 signals EOS (same as frame==NULL in put_frame).
//Returns 0 on success, -2 if already flushing, negative on error.
int nvmpi_encoder_put_frame_fd(nvmpictx* ctx,
	int dmabuf_fd, int width, int height, int pitch,
	int64_t timestamp)
{
	(void)width;
	(void)height;
	(void)pitch;

	if (ctx->flushing.load(std::memory_order_acquire)) return -2;

	if (!ctx->dmabuf_external) {
		NVMPI_LOG(NVMPI_LOG_ERROR, "put_frame_fd called but encoder was not "
			  "created with use_dmabuf=1");
		return -1;
	}

	int ret;
	struct v4l2_buffer v4l2_buf;
	struct v4l2_plane planes[MAX_PLANES];

	memset(&v4l2_buf, 0, sizeof(v4l2_buf));
	memset(planes, 0, sizeof(planes));

	v4l2_buf.m.planes = planes;
	v4l2_buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	v4l2_buf.memory = V4L2_MEMORY_DMABUF;

	if (ctx->enc->isInError())
		return -1;

	/* Buffer acquisition: same index-then-dqBuffer pattern as put_frame.
	 * First N frames use sequential indices; after that, reclaim a used
	 * buffer. Blocking mode waits indefinitely; non-blocking returns
	 * NVMPI_ERR_EAGAIN when no buffer is available. */
	if (ctx->index < ctx->enc->output_plane.getNumBuffers())
	{
		v4l2_buf.index = ctx->index;
		ctx->index++;
	}
	else
	{
		NvBuffer *nvBuffer;
		int dq_timeout = ctx->blocking_mode ? -1 : 0;
		ret = ctx->enc->output_plane.dqBuffer(v4l2_buf, &nvBuffer, NULL, dq_timeout);
		if (ret < 0)
		{
			if (!ctx->blocking_mode) {
				/* Non-blocking: no buffer available right now */
				return NVMPI_ERR_EAGAIN;
			}
			NVMPI_LOG(NVMPI_LOG_ERROR, "Error DQing buffer at output plane (dmabuf mode)");
			return -1;
		}
	}

	/* Retrieve NvBuffer for this slot — needed for plane metadata
	 * (stride, height) to set bytesused on the V4L2 buffer. */
	NvBuffer *nvBuffer = ctx->enc->output_plane.getNthBuffer(v4l2_buf.index);
	int slot = v4l2_buf.index;
	int internal_fd = ctx->output_plane_fd[slot];

	if (dmabuf_fd >= 0)
	{
#ifdef WITH_NVUTILS
		/* Lazy-allocate internal surface for this slot on first use.
		 * Deferred from nvmpi_create_encoder to reduce peak NVMM memory
		 * — the extradata encoder (SPS/PPS extraction) is already
		 * destroyed by the time put_frame_fd is first called, so its
		 * MMAP buffers are free. On Orin Nano, pre-allocating all 10
		 * internal surfaces at creation time exhausts the NVMM heap
		 * before the VIC compute session can be established. */
		if (internal_fd < 0) {
			NvBufSurf::NvCommonAllocateParams cParams;
			memset(&cParams, 0, sizeof(cParams));
			cParams.width = ctx->width;
			cParams.height = ctx->height;
			cParams.layout = NVBUF_LAYOUT_PITCH;
			cParams.colorFormat = NVBUF_COLOR_FORMAT_NV12;
			cParams.memtag = NvBufSurfaceTag_VIDEO_ENC;
			cParams.memType = NVBUF_MEM_SURFACE_ARRAY;

			int fd = -1;
			ret = NvBufSurf::NvAllocate(&cParams, 1, &fd);
			if (ret < 0 || fd < 0) {
				NVMPI_LOG(NVMPI_LOG_ERROR,
					  "Failed to allocate internal DMABUF for slot %d", slot);
				return -1;
			}
			ctx->output_plane_fd[slot] = fd;
			internal_fd = fd;

			/* Map internal fd to V4L2 slot. mapOutputBuffers
			 * populates NvBuffer plane metadata (stride, data
			 * ptr for CPU copy) and maps the surface for
			 * read/write access. Uses a scratch v4l2_buffer
			 * to avoid mutating the actual qBuffer v4l2_buf. */
			struct v4l2_buffer map_buf;
			struct v4l2_plane map_planes[MAX_PLANES];
			memset(&map_buf, 0, sizeof(map_buf));
			memset(map_planes, 0, sizeof(map_planes));
			map_buf.index = slot;
			map_buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
			map_buf.memory = V4L2_MEMORY_DMABUF;
			map_buf.m.planes = map_planes;
			ret = ctx->enc->output_plane.mapOutputBuffers(map_buf, fd);
			if (ret < 0) {
				NVMPI_LOG(NVMPI_LOG_ERROR,
					  "Error mapping internal buffer to slot %d; "
					  "destroying leaked fd", slot);
				/* Rollback: destroy the just-allocated fd to prevent
				 * DMA-BUF / CMA leak. Reset slot to -1 so close()
				 * skips it and future put_frame_fd retries allocation. */
				NvBufSurf::NvDestroy(fd);
				ctx->output_plane_fd[slot] = -1;
				return -1;
			}

			/* Re-fetch NvBuffer — mapOutputBuffers populated plane data */
			nvBuffer = ctx->enc->output_plane.getNthBuffer(slot);
		}

		/* Try internal copy path: requires (a) internal buffer
		 * allocated for this slot, and (b) source fd registered in
		 * the NvBufSurface global table (NvBufSurfaceFromFd succeeds).
		 * VIC-allocated fds are always registered; decoder dup'd fds
		 * are not. On fallback, pass external fd directly to V4L2. */
		bool did_internal_copy = false;

		if (internal_fd >= 0)
		{
			NvBufSurface *src_surf = NULL;
			ret = NvBufSurfaceFromFd(dmabuf_fd, (void **)(&src_surf));
			if (ret == 0) {
				/* Source is registered — internal copy path.
				 * Copy external → internal via CPU memcpy.
				 * Internal buffer is pre-registered 1:1 with
				 * this V4L2 slot (same fd every time), matching
				 * NVIDIA's 01_video_encode DMABUF pattern.
				 * Avoids NvMap handle degradation from rotating
				 * external fds across slots.
				 *
				 * CPU memcpy instead of NvBufSurfaceCopy because
				 * the latter rejects surfaces with different
				 * allocation sizes even when pixel dimensions
				 * match (alignment metadata difference between
				 * VIC-allocated and encoder-allocated surfaces).
				 * For <=1080p at 30fps, overhead is negligible. */
				ret = NvBufSurfaceMap(src_surf, 0, -1, NVBUF_MAP_READ);
				if (ret < 0) {
					NVMPI_LOG(NVMPI_LOG_ERROR,
						  "NvBufSurfaceMap failed for fd=%d",
						  dmabuf_fd);
					return -1;
				}
				NvBufSurfaceSyncForCpu(src_surf, 0, -1);

				/* Copy plane-by-plane to handle stride diffs.
				 * Destination mapped by mapOutputBuffers during
				 * lazy alloc — nvBuffer->planes[j].data valid. */
				for (uint32_t j = 0; j < nvBuffer->n_planes; j++) {
					uint8_t *src_ptr = (uint8_t *)
						src_surf->surfaceList[0].mappedAddr.addr[j];
					uint8_t *dst_ptr = (uint8_t *)
						nvBuffer->planes[j].data;
					unsigned int src_pitch =
						src_surf->surfaceList[0].planeParams.pitch[j];
					unsigned int src_height =
						src_surf->surfaceList[0].planeParams.height[j];
					unsigned int dst_stride =
						nvBuffer->planes[j].fmt.stride;
					unsigned int copy_w =
						src_pitch < dst_stride ? src_pitch : dst_stride;

					for (unsigned int y = 0; y < src_height; y++)
						memcpy(dst_ptr + (size_t)y * dst_stride,
						       src_ptr + (size_t)y * src_pitch,
						       copy_w);

					nvBuffer->planes[j].bytesused =
						dst_stride * src_height;
				}

				NvBufSurfaceUnMap(src_surf, 0, -1);

				/* Flush CPU writes so encoder's VIC blit sees
				 * the copied data. */
				NvBufSurface *dst_surf = NULL;
				ret = NvBufSurfaceFromFd(internal_fd,
							 (void **)(&dst_surf));
				if (ret == 0)
					NvBufSurfaceSyncForDevice(dst_surf, 0, -1);

				/* Point V4L2 planes at the internal fd. */
				for (uint32_t j = 0; j < nvBuffer->n_planes; j++) {
					v4l2_buf.m.planes[j].m.fd = internal_fd;
					v4l2_buf.m.planes[j].bytesused =
						nvBuffer->planes[j].bytesused;
				}

				did_internal_copy = true;
			}
			/* else: source not registered (decoder dup'd fd) —
			 * fall through to direct pass-through below. */
		}

		if (!did_internal_copy)
#endif
		{
			/* Direct pass-through: pass external fd to V4L2.
			 * Used when: (a) no internal buffer (JetPack 4, CMA
			 * exhausted), or (b) source fd not registered in
			 * NvBufSurface table (decoder dup'd fds). */
			v4l2_buf.m.planes[0].m.fd = dmabuf_fd;
			v4l2_buf.m.planes[0].bytesused = 1;
			if (ctx->raw_pixfmt == V4L2_PIX_FMT_NV12M ||
			    ctx->raw_pixfmt == V4L2_PIX_FMT_YUV420M) {
				v4l2_buf.m.planes[1].m.fd = dmabuf_fd;
				v4l2_buf.m.planes[1].bytesused = 1;
			}
#ifdef WITH_NVUTILS
			{
				NvBufSurface *nvbuf_surf = NULL;
				ret = NvBufSurfaceFromFd(dmabuf_fd, (void **)(&nvbuf_surf));
				if (ret == 0)
					NvBufSurfaceSyncForDevice(nvbuf_surf, 0, -1);
			}
#else
			{
				void *dummy = NULL;
				NvBufferMemSyncForDevice(dmabuf_fd, 0, &dummy);
				NvBufferMemSyncForDevice(dmabuf_fd, 1, &dummy);
			}
#endif
		}

		v4l2_buf.flags |= V4L2_BUF_FLAG_TIMESTAMP_COPY;
		v4l2_buf.timestamp.tv_sec = timestamp / 1000000;
		v4l2_buf.timestamp.tv_usec = timestamp % 1000000;
	}
	else
	{
		/* EOS: queue a zero-byte buffer to tell the encoder to drain.
		 * Must still set an fd — DMABUF mode requires a valid fd even
		 * for EOS (NVIDIA 01_video_encode sample does the same). */
		ctx->flushing.store(true, std::memory_order_release);
		if (internal_fd >= 0) {
			for (uint32_t j = 0; j < nvBuffer->n_planes; j++)
				v4l2_buf.m.planes[j].m.fd = internal_fd;
		}
		v4l2_buf.m.planes[0].bytesused = 0;
		v4l2_buf.m.planes[1].bytesused = 0;
	}

	ret = ctx->enc->output_plane.qBuffer(v4l2_buf, NULL);
	if (ret < 0) {
		NVMPI_LOG(NVMPI_LOG_ERROR, "Error while queueing buffer at output plane (dmabuf mode, code=%d)", ret);
		return ret;
	}

	return 0;
}
