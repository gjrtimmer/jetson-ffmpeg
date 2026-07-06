/*
 * nvmpi_dec_init.cpp — decoder creation/initialization (libnvmpi).
 *
 * Implements nvmpi_create_decoder() from the public C API in include/nvmpi.h
 * on top of NVIDIA's V4L2 NvVideoDecoder sample class, plus the destination
 * frame-pool and per-frame VIC transform parameter setup it depends on.
 *
 * This file owns:
 *   - updateFrameSizeParams()       — cache dst_dma buffer plane geometry.
 *   - updateBufferTransformParams() — cache VIC src/dst rects and filter.
 *   - deinitFramePool()/initFramePool() — free/allocate the pitch-linear
 *                                     destination DMA buffers (VIC output).
 *   - nvmpi_create_decoder()        — open the V4L2 decoder device, configure
 *                                     the OUTPUT plane, start the capture
 *                                     thread.
 *
 * The CAPTURE plane and (for streams whose dimensions are unknown at create
 * time) the frame pool are NOT fully set up here — that happens on the
 * capture thread once the first resolution-change event fires (see
 * nvmpi_dec_capture.cpp: respondToResolutionEvent()).
 *
 * Companion files:
 *   nvmpi_dec_internal.h  — shared includes, defines, nvmpictx struct,
 *                           and forward declarations.
 *   nvmpi_dec_api.cpp     — runtime API: put_packet, get_frame,
 *                           get_frame_fd, copyNvBufToFrame, flush, close.
 *   nvmpi_dec_capture.cpp — capture-thread loop and resolution-change
 *                           handling.
 *   nvmpi_dec_planes.cpp  — CAPTURE-plane allocation/teardown helpers.
 */
#include "nvmpi_dec_internal.h"

//Query the actual plane geometry (count, pitch, height, bytes-per-line) of
//one freshly allocated dst_dma frame buffer and cache it in the context.
//These values drive the memcpy in copyNvBufToFrame(); the allocator may
//choose pitches different from width, so they must be read back, not
//assumed. Uses peekEmptyBuf() — safe only because no consumer runs yet.
void nvmpictx::updateFrameSizeParams()
{
	//it's safe when called from respondToResolutionEvent() after initFramePool()
	nvmpi_frame_buffer* fb = framePool->peekEmptyBuf();
#ifdef WITH_NVUTILS
	NvBufSurfacePlaneParams parm;
	NvBufSurfaceParams dst_dma_surface_params;
	dst_dma_surface_params = fb->dst_dma_surface->surfaceList[0];
	parm = dst_dma_surface_params.planeParams;
#else
	NvBufferParams parm;
	int ret = NvBufferGetParams(fb->dst_dma_fd, &parm);
	if (ret < 0) {
		NVMPI_LOG(NVMPI_LOG_ERROR, "Failed to get dst dma buf params (code=%d)", ret);
		return;
	}
#endif

	num_planes = parm.num_planes;
	for(unsigned int i=0; i<num_planes; i++)
	{
		frame_linesize[i] = parm.pitch[i];
		frame_height[i] = parm.height[i];
#ifdef WITH_NVUTILS
		frame_linedatasize[i] = parm.width[i] * parm.bytesPerPix[i]; //valid only for nvutils
#else
		//legacy API lacks bytesPerPix: the interleaved UV plane of NV12
		//variants carries 2 bytes per pixel, everything else 1.
		if(i == 1 && (parm.pixel_format == NvBufferColorFormat_NV12 ||
				parm.pixel_format == NvBufferColorFormat_NV16 ||
				parm.pixel_format == NvBufferColorFormat_NV24 ||
				parm.pixel_format == NvBufferColorFormat_NV12_ER ||
				parm.pixel_format == NvBufferColorFormat_NV12_709 ||
				parm.pixel_format == NvBufferColorFormat_NV12_709_ER ||
				parm.pixel_format == NvBufferColorFormat_NV12_2020))
		{
			frame_linedatasize[i] = parm.width[i] * 2;
		}
		else
		{
			frame_linedatasize[i] = parm.width[i];
		}
#endif
	}

	return;
}

//Build the cached VIC transform parameters used for every decoded frame:
//src rect = coded (cropped) decoder resolution, dst rect = user-visible
//output resolution (downscale happens here when the resize option is set).
//The two APIs differ slightly: NvUtils takes rect pointers, the legacy API
//embeds the rects and needs the explicit session handle.
void nvmpictx::updateBufferTransformParams()
{
	src_rect.top = 0;
	src_rect.left = 0;
	src_rect.width = coded_width;
	src_rect.height = coded_height;
	dest_rect.top = 0;
	dest_rect.left = 0;
	dest_rect.width = output_width;
	dest_rect.height = output_height;

	memset(&transform_params,0,sizeof(transform_params));
	transform_params.transform_flag = NVBUFFER_TRANSFORM_FILTER;
	transform_params.transform_flip = NvBufferTransform_None;
	transform_params.transform_filter = NvBufferTransform_Filter_Smart;
	//ctx->transform_params.transform_filter = NvBufSurfTransformInter_Nearest;
#ifdef WITH_NVUTILS
	transform_params.src_rect = &src_rect;
	transform_params.dst_rect = &dest_rect;
#else
	transform_params.src_rect = src_rect;
	transform_params.dst_rect = dest_rect;
	transform_params.session = session;
#endif
}

//Destroy all frame buffers tracked by allocatedFrameBufs and drain both
//pool queues. Uses the vector instead of the queues so that buffers
//temporarily outside both queues (e.g. held by a concurrent get_frame
//call during resolution change) are still freed.
void nvmpictx::deinitFramePool()
{
	while(framePool->dqEmptyBuf()) {}
	while(framePool->dqFilledBuf()) {}

	for (auto* fb : allocatedFrameBufs)
	{
		fb->destroy();
		delete fb;
	}
	allocatedFrameBufs.clear();
}

//Allocate frame_pool_size pitch-linear DMA buffers at the output resolution
//and user-requested pixel format, and seed the pool's "empty" queue with
//them. These are the destination buffers of the VIC transform — distinct
//from the block-linear CAPTURE-plane buffers the decoder writes into.
void nvmpictx::initFramePool()
{
	/* Validate pool size — zero or negative would allocate nothing, causing
	 * downstream dqEmptyBuf() to block forever or return null. */
	if (frame_pool_size <= 0) {
		NVMPI_LOG(NVMPI_LOG_ERROR, "initFramePool: frame_pool_size=%d is invalid, skipping allocation", frame_pool_size);
		return;
	}

	/* If the pool is already populated (e.g. initFramePool called without a
	 * prior deinitFramePool), tear down the existing buffers first to prevent
	 * leaking DMA buffer FDs. deinitFramePool is safe on an empty pool. */
	if (!allocatedFrameBufs.empty()) {
		NVMPI_LOG(NVMPI_LOG_WARN, "initFramePool: pool already allocated (%zu buffers), deinitializing first to prevent DMA fd leak", allocatedFrameBufs.size());
		deinitFramePool();
	}

	//VIC destination format: NV12 or planar YUV420 (8-bit), or NV12_10LE
	//(P010, 10-bit). The 10-bit branch is NvUtils-only — out_pixfmt can
	//never be NV_PIX_P010 on legacy builds (rejected at decoder init), but
	//the constant must still be compiled out there.
	NvBufferColorFormat cFmt = out_pixfmt==NV_PIX_NV12?NvBufferColorFormat_NV12: NvBufferColorFormat_YUV420;
#ifdef WITH_NVUTILS
	if(out_pixfmt==NV_PIX_P010) cFmt = NvBufferColorFormat_NV12_10LE;
#endif

	NvBufferCreateParams input_params;
	memset(&input_params, 0, sizeof(input_params));
	/* Create PitchLinear output buffer for transform. */
	input_params.width = output_width;
	input_params.height = output_height;
	input_params.layout = NvBufferLayout_Pitch;
	input_params.colorFormat = cFmt;
#ifdef WITH_NVUTILS
	input_params.memType = NVBUF_MEM_SURFACE_ARRAY;
	input_params.memtag = NvBufSurfaceTag_VIDEO_CONVERT;
#else
	input_params.payloadType = NvBufferPayload_SurfArray;
	input_params.nvbuf_tag = NvBufferTag_VIDEO_DEC;
#endif

	for(int i=0;i<frame_pool_size;i++)
	{
		nvmpi_frame_buffer* fb = new nvmpi_frame_buffer();
		if(!fb->alloc(input_params)) { delete fb; break; }
		allocatedFrameBufs.push_back(fb);
		framePool->qEmptyBuf(fb);
	}
	return;
}

//Handle V4L2_EVENT_RESOLUTION_CHANGE (also fired once at stream start when
//the decoder has parsed the headers): query the new format/crop, rebuild
//the CAPTURE plane and the destination frame pool, and refresh the cached
//transform/copy parameters. Runs on the capture thread.

//Public API: create and start a decoder.
//Opens the V4L2 decoder device, subscribes to resolution-change events,
//configures the OUTPUT (bitstream) plane with USERPTR buffers and starts
//streaming on it, then spawns the capture thread. The CAPTURE plane and
//frame pool are NOT set up here — that happens on the capture thread once
//the first resolution-change event reveals the stream geometry.
//Returns the new context, or NULL on failure (V4L2 device unavailable,
//ioctl error, or unsupported configuration).
nvmpictx* nvmpi_create_decoder(nvDecParam* param)
{
	int ret;
	log_level = LOG_LEVEL_INFO;

#ifndef WITH_NVUTILS
	//P010 (10-bit) output uses the NvBufSurface NV12_10LE color formats, which
	//exist only in the NvUtils API (JetPack 5+). The legacy nvbuf_utils build
	//cannot honour it — fail loudly rather than emit corrupt 8-bit frames.
	if(param->pixFormat == NV_PIX_P010)
	{
		NVMPI_LOG(NVMPI_LOG_ERROR, "P010 (10-bit) decode requires the NvUtils buffer API (JetPack 5+); not available in this build.");
		return NULL;
	}
#endif

	nvmpictx* ctx=new nvmpictx();

	/* Open the V4L2 decoder device. Retry up to 3 times with 100ms
	 * backoff. Before each factory call, probe the device node ourselves
	 * to capture errno — MMAPI's NvV4l2Element discards it, making EBUSY
	 * indistinguishable from ENODEV at the factory level. See #37.
	 *
	 * Device paths from MMAPI source (NvVideoDecoder.cpp):
	 *   primary: /dev/nvhost-nvdec   (nvhost driver, JetPack 4.x)
	 *   fallback: /dev/v4l2-nvdec    (V4L2 kernel driver, JetPack 5+) */
	for (int attempt = 0; attempt < 3; attempt++) {
		/* Probe: resolve the same device path MMAPI would use. */
		const char *dev_path = NULL;
		if (access("/dev/nvhost-nvdec", F_OK) == 0)
			dev_path = "/dev/nvhost-nvdec";
		else if (access("/dev/v4l2-nvdec", F_OK) == 0)
			dev_path = "/dev/v4l2-nvdec";

		if (dev_path) {
			int probe_fd = open(dev_path, O_RDWR);
			if (probe_fd < 0) {
				int saved_errno = errno;
				if (saved_errno == ENODEV || saved_errno == ENXIO) {
					/* Device node exists but driver not loaded or hw absent —
					 * permanent failure, no point retrying. */
					NVMPI_LOG(NVMPI_LOG_ERROR,
						  "Decoder device %s unavailable (errno=%d: %s)",
						  dev_path, saved_errno, strerror(saved_errno));
					delete ctx;
					return NULL;
				}
				if (saved_errno == EACCES) {
					NVMPI_LOG(NVMPI_LOG_ERROR,
						  "Decoder device %s: permission denied", dev_path);
					delete ctx;
					return NULL;
				}
				if (saved_errno == EBUSY) {
					NVMPI_LOG(NVMPI_LOG_WARN,
						  "Decoder device %s busy (EBUSY), retrying (%d/3)...",
						  dev_path, attempt + 1);
					usleep(100000);
					continue;
				}
				/* Other errno (EIO, etc.) — log and fall through to factory. */
				NVMPI_LOG(NVMPI_LOG_WARN,
					  "Decoder device probe failed (errno=%d: %s), trying factory...",
					  saved_errno, strerror(saved_errno));
			} else {
				close(probe_fd);
			}
		} else {
			NVMPI_LOG(NVMPI_LOG_ERROR,
				  "No decoder device node found (/dev/nvhost-nvdec or /dev/v4l2-nvdec)");
			delete ctx;
			return NULL;
		}

		ctx->dec = NvVideoDecoder::createVideoDecoder("dec0");
		if (ctx->dec) break;
		if (attempt < 2) {
			NVMPI_LOG(NVMPI_LOG_WARN, "Decoder factory returned NULL, retrying (%d/3)...", attempt + 1);
			usleep(100000);
		}
	}
	if (!ctx->dec)
	{
		NVMPI_LOG(NVMPI_LOG_ERROR, "Could not create decoder after 3 attempts");
		delete ctx;
		return NULL;
	}

	ret=ctx->dec->subscribeEvent(V4L2_EVENT_RESOLUTION_CHANGE, 0, 0);
	if (ret < 0)
	{
		NVMPI_LOG(NVMPI_LOG_ERROR, "Could not subscribe to V4L2_EVENT_RESOLUTION_CHANGE");
		delete ctx->dec;
		delete ctx;
		return NULL;
	}

	ctx->frame_pool_size = param->frame_pool_size;
	ctx->max_perf = param->max_perf;
	ctx->disable_dpb = param->disable_dpb;

	/* 0 = use default (500ms); valid range [50, 5000]; anything else = warn + default. */
	if (param->wait_timeout >= 50 && param->wait_timeout <= 5000)
		ctx->wait_timeout_ms = param->wait_timeout;
	else if (param->wait_timeout != 0)
		NVMPI_LOG(NVMPI_LOG_WARN, "wait_timeout %d out of range [50, 5000]; using default %d", param->wait_timeout, ctx->wait_timeout_ms);

	//0 keeps the default; out-of-range values (including garbage from
	//callers built against an older nvDecParam layout) fall back too.
	if(param->chunk_size >= 65536 && param->chunk_size <= 64u*1024*1024)
		ctx->chunk_size = param->chunk_size;
	else if(param->chunk_size != 0)
		NVMPI_LOG(NVMPI_LOG_WARN, "chunk_size %u out of range [65536, 67108864]; using default %u", param->chunk_size, ctx->chunk_size);

	//map the API codec enum to the V4L2 compressed pixel format fourcc
	switch(param->codingType)
	{
		case NV_VIDEO_CodingH264:
			ctx->decoder_pixfmt=V4L2_PIX_FMT_H264;
			break;
		case NV_VIDEO_CodingHEVC:
			ctx->decoder_pixfmt=V4L2_PIX_FMT_H265;
			break;
		case NV_VIDEO_CodingMPEG4:
			ctx->decoder_pixfmt=V4L2_PIX_FMT_MPEG4;
			break;
		case NV_VIDEO_CodingMPEG2:
			ctx->decoder_pixfmt=V4L2_PIX_FMT_MPEG2;
			break;
		case NV_VIDEO_CodingVP8:
			ctx->decoder_pixfmt=V4L2_PIX_FMT_VP8;
			break;
		case NV_VIDEO_CodingVP9:
			ctx->decoder_pixfmt=V4L2_PIX_FMT_VP9;
			break;
		default:
			ctx->decoder_pixfmt=V4L2_PIX_FMT_H264;
			break;
	}

	//OUTPUT plane: compressed input, buffers sized ctx->chunk_size
	ret=ctx->dec->setOutputPlaneFormat(ctx->decoder_pixfmt, ctx->chunk_size);
	if (ret < 0)
	{
		NVMPI_LOG(NVMPI_LOG_ERROR, "Could not set output plane format");
		delete ctx->dec;
		delete ctx;
		return NULL;
	}

	//input mode 0 = feed complete NAL units / frames per buffer
	ret = ctx->dec->setFrameInputMode(0);
	if (ret < 0)
	{
		NVMPI_LOG(NVMPI_LOG_ERROR, "Error in decoder setFrameInputMode");
		delete ctx->dec;
		delete ctx;
		return NULL;
	}

	if(ctx->disable_dpb)
	{
		ret = ctx->dec->disableDPB();
		if (ret < 0) {
			NVMPI_LOG(NVMPI_LOG_ERROR, "Error in decoder disableDPB (code=%d)", ret);
			delete ctx->dec;
			delete ctx;
			return NULL;
		}
	}

	if(ctx->max_perf)
	{
		ret = ctx->dec->setMaxPerfMode(1);
		if (ret < 0) {
			NVMPI_LOG(NVMPI_LOG_ERROR, "Error while setting decoder to max perf (code=%d)", ret);
			delete ctx->dec;
			delete ctx;
			return NULL;
		}
	}

	//10 USERPTR buffers on the OUTPUT plane; packet data is memcpy'd into
	//them in nvmpi_decoder_put_packet()
	ret = ctx->dec->output_plane.setupPlane(V4L2_MEMORY_USERPTR, 10, false, true);
	if (ret < 0)
	{
		NVMPI_LOG(NVMPI_LOG_ERROR, "Error while setting up output plane");
		delete ctx->dec;
		delete ctx;
		return NULL;
	}

	/* Capture the return value — previous code tested stale ret from
	 * setupPlane(), silently missing setStreamStatus failures. */
	ret = ctx->dec->output_plane.setStreamStatus(true);
	if (ret < 0)
	{
		NVMPI_LOG(NVMPI_LOG_ERROR, "Error in output plane stream on");
		ctx->dec->output_plane.deinitPlane();
		delete ctx->dec;
		delete ctx;
		return NULL;
	}

	ctx->out_pixfmt=param->pixFormat;
	ctx->resized = param->resized;
	ctx->hint_width = param->width;
	ctx->hint_height = param->height;
	ctx->framePool = new NVMPI_bufPool<nvmpi_frame_buffer*>();
	/* Shared flag for DRM_PRIME frame release callbacks — stays valid
	 * after decoder close because frame refs hold shared_ptr copies.
	 * Allocated here; set to false in nvmpi_decoder_close(). */
	ctx->pool_alive = std::make_shared<std::atomic<bool>>(true);
	ctx->frames_checked_out = std::make_shared<std::atomic<int>>(0);
	ctx->eos.store(false);
	ctx->index=0;
	for(int index=0;index<MAX_BUFFERS;index++)
		ctx->dmaBufferFileDescriptor[index]=-1;
	ctx->numberCaptureBuffers=0;

	/* Pre-allocate the frame pool when container-reported dimensions are
	 * available. This reduces first-frame latency by having destination
	 * DMA buffers ready before the capture thread's resolution-change
	 * event fires. If the actual stream dimensions differ, the capture
	 * thread rebuilds the pool at the correct size via respondToResolutionEvent.
	 * The resize target takes precedence when set. */
	if (ctx->hint_width > 0 && ctx->hint_height > 0)
	{
		ctx->output_width = ctx->resized.width ? ctx->resized.width : ctx->hint_width;
		ctx->output_height = ctx->resized.height ? ctx->resized.height : ctx->hint_height;
		ctx->initFramePool();
		ctx->updateFrameSizeParams();
		NVMPI_LOG(NVMPI_LOG_INFO, "pre-allocated frame pool at %ux%u from container hints",
		          ctx->output_width, ctx->output_height);
	}

	ctx->dec_capture_loop = std::thread(dec_capture_loop_fcn,ctx);
	pthread_setname_np(ctx->dec_capture_loop.native_handle(), "dec_capture");

	return ctx;
}
