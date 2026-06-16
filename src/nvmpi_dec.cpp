#include "nvmpi_dec_internal.h"

//Map the colorspace/quantization reported by the decoder on its CAPTURE
//plane to the matching NvBuffer NV12 color format variant (BT.601/709/2020,
//standard vs extended luma range). Used when allocating the CAPTURE-plane
//DMA buffers so the subsequent VIC transform interprets colors correctly.
NvBufferColorFormat getNvColorFormatFromV4l2Format(v4l2_format &format, bool want_10bit)
{
#ifdef WITH_NVUTILS
	//10-bit (P010) output: pick the matching NV12_10LE colorimetry variant.
	//These constants exist only in the NvUtils API, which is why P010 is
	//gated on WITH_NVUTILS at the decoder init layer.
	if (want_10bit)
	{
		switch (format.fmt.pix_mp.colorspace)
		{
			case V4L2_COLORSPACE_REC709:
				return (format.fmt.pix_mp.quantization == V4L2_QUANTIZATION_DEFAULT)
					? NvBufferColorFormat_NV12_10LE_709 : NvBufferColorFormat_NV12_10LE_709_ER;
			case V4L2_COLORSPACE_BT2020:
				return NvBufferColorFormat_NV12_10LE_2020;
			case V4L2_COLORSPACE_SMPTE170M:
			default:
				return (format.fmt.pix_mp.quantization == V4L2_QUANTIZATION_DEFAULT)
					? NvBufferColorFormat_NV12_10LE : NvBufferColorFormat_NV12_10LE_ER;
		}
	}
#else
	(void)want_10bit;
#endif
	NvBufferColorFormat ret_cf = NvBufferColorFormat_NV12;
	switch (format.fmt.pix_mp.colorspace)
	{
		case V4L2_COLORSPACE_SMPTE170M:
			if (format.fmt.pix_mp.quantization == V4L2_QUANTIZATION_DEFAULT)
			{
				// "Decoder colorspace ITU-R BT.601 with standard range luma (16-235)"
				ret_cf = NvBufferColorFormat_NV12;
			}
			else
			{
				//"Decoder colorspace ITU-R BT.601 with extended range luma (0-255)";
				ret_cf = NvBufferColorFormat_NV12_ER;
			}
			break;
		case V4L2_COLORSPACE_REC709:
			if (format.fmt.pix_mp.quantization == V4L2_QUANTIZATION_DEFAULT)
			{
				//"Decoder colorspace ITU-R BT.709 with standard range luma (16-235)";
				ret_cf = NvBufferColorFormat_NV12_709;
			}
			else
			{
				//"Decoder colorspace ITU-R BT.709 with extended range luma (0-255)";
				ret_cf = NvBufferColorFormat_NV12_709_ER;
			}
			break;
		case V4L2_COLORSPACE_BT2020:
			{
				//"Decoder colorspace ITU-R BT.2020";
				ret_cf = NvBufferColorFormat_NV12_2020;
			}
			break;
		default:
			if (format.fmt.pix_mp.quantization == V4L2_QUANTIZATION_DEFAULT)
			{
				//"Decoder colorspace ITU-R BT.601 with standard range luma (16-235)";
				ret_cf = NvBufferColorFormat_NV12;
			}
			else
			{
				//"Decoder colorspace ITU-R BT.601 with extended range luma (0-255)";
				ret_cf = NvBufferColorFormat_NV12_ER;
			}
			break;
	}
	return ret_cf;
}


//(Re)initialize the decoder CAPTURE plane after a resolution-change event:
//set the negotiated plane format, allocate numberCaptureBuffers block-linear
//NV12 DMA buffers sized to the coded resolution, REQBUFS them as
//V4L2_MEMORY_DMABUF, start streaming and enqueue every buffer so the
//decoder can start writing. Called from the capture thread only.
void nvmpictx::initDecoderCapturePlane(v4l2_format &format)
{
	int ret=0;
	int32_t minimumDecoderCaptureBuffers;
	NvBufferCreateParams cParams;
	memset(&cParams, 0, sizeof(cParams));
	
	ret=dec->setCapturePlaneFormat(format.fmt.pix_mp.pixelformat,format.fmt.pix_mp.width,format.fmt.pix_mp.height);
	TEST_ERROR(ret < 0, "Error in setting decoder capture plane format", ret);

	dec->getMinimumCapturePlaneBuffers(minimumDecoderCaptureBuffers);
	TEST_ERROR(ret < 0, "Error while getting value of minimum capture plane buffers",ret);

	/* Request (min + extra) buffers, export and map buffers. */
	numberCaptureBuffers = minimumDecoderCaptureBuffers + 5;

	//Block-linear layout matches what the hw decoder writes natively; the
	//buffers are converted to pitch-linear later by the VIC transform.
	cParams.colorFormat = getNvColorFormatFromV4l2Format(format, out_pixfmt == NV_PIX_P010);
	cParams.width = coded_width;
	cParams.height = coded_height;
	cParams.layout = NvBufferLayout_BlockLinear;
#ifdef WITH_NVUTILS
	cParams.memType = NVBUF_MEM_SURFACE_ARRAY;
	cParams.memtag = NvBufSurfaceTag_VIDEO_DEC;

	//NvUtils path: allocate all buffers in one call, then resolve the
	//NvBufSurface view of each fd for use with NvBufSurfTransform.
	ret = NvBufSurf::NvAllocate(&cParams, numberCaptureBuffers, dmaBufferFileDescriptor);
	TEST_ERROR(ret < 0, "Failed to create buffers", error);
	for (int index = 0; index < numberCaptureBuffers; index++)
	{
		ret = NvBufSurfaceFromFd(dmaBufferFileDescriptor[index], (void**)(&(dmaBufferSurface[index])));
		TEST_ERROR(ret < 0, "Failed to get surface for buffer", ret);
	}
#else
	//legacy nvbuf_utils path: allocate the buffers one by one
	cParams.payloadType = NvBufferPayload_SurfArray;
	cParams.nvbuf_tag = NvBufferTag_VIDEO_DEC;

	for (int index = 0; index < numberCaptureBuffers; index++)
	{
		ret = NvBufferCreateEx(&dmaBufferFileDescriptor[index], &cParams);
		TEST_ERROR(ret < 0, "Failed to create buffers", ret);
	}
#endif

    /* Request buffers on decoder capture plane. Refer ioctl VIDIOC_REQBUFS */
	dec->capture_plane.reqbufs(V4L2_MEMORY_DMABUF, numberCaptureBuffers);
	TEST_ERROR(ret < 0, "Error in decoder capture plane streamon", ret);

    /* Decoder capture plane STREAMON. Refer ioctl VIDIOC_STREAMON */
	dec->capture_plane.setStreamStatus(true);
	TEST_ERROR(ret < 0, "Error in decoder capture plane streamon", ret);

	/* Enqueue all the empty decoder capture plane buffers. */
	for (uint32_t i = 0; i < dec->capture_plane.getNumBuffers(); i++)
	{
		struct v4l2_buffer v4l2_buf;
		struct v4l2_plane planes[MAX_PLANES];

		memset(&v4l2_buf, 0, sizeof(v4l2_buf));
		memset(planes, 0, sizeof(planes));

		v4l2_buf.index = i;
		v4l2_buf.m.planes = planes;
		v4l2_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		v4l2_buf.memory = V4L2_MEMORY_DMABUF;
		v4l2_buf.m.planes[0].m.fd = dmaBufferFileDescriptor[i];

		ret = dec->capture_plane.qBuffer(v4l2_buf, NULL);
		TEST_ERROR(ret < 0, "Error Qing buffer at output plane", ret);
	}
	
	return;
}

//Tear down the CAPTURE plane: STREAMOFF, unmap/release the V4L2 buffers
//(deinitPlane issues REQBUFS with count 0) and destroy our DMA buffers.
//Called on resolution change (before re-init) and from nvmpi_decoder_close.
void nvmpictx::deinitDecoderCapturePlane()
{
	if (numberCaptureBuffers == 0)
		return;

	int ret = 0;
	dec->capture_plane.setStreamStatus(false);
	// bypass deinitPlane() — it calls waitForDQThread() which touches MMAPI
	// DQ thread state the decoder never uses (we use std::thread instead);
	// for DMABUF the only work deinitPlane() does beyond that is reqbufs(0)
	dec->capture_plane.reqbufs(V4L2_MEMORY_DMABUF, 0);
	for (int index = 0; index < numberCaptureBuffers; index++)
	{
		if (dmaBufferFileDescriptor[index] >= 0)
		{
			ret = NvBufferDestroy(dmaBufferFileDescriptor[index]);
			TEST_ERROR(ret < 0, "Failed to Destroy NvBuffer", ret);
			dmaBufferFileDescriptor[index] = -1;
		}
	}
	numberCaptureBuffers = 0;
	return;
}

//Query the actual plane geometry (count, pitch, height, bytes-per-line) of
//one freshly allocated dst_dma frame buffer and cache it in the context.
//These values drive the memcpy in copyNvBufToFrame(); the allocator may
//choose pitches different from width, so they must be read back, not
//assumed. Uses peekEmptyBuf() — safe only because no consumer runs yet.
void nvmpictx::updateFrameSizeParams()
{
	//it's safe when called from respondToResolutionEvent() after initFramePool()
	NVMPI_frameBuf* fb = framePool->peekEmptyBuf();
#ifdef WITH_NVUTILS
	NvBufSurfacePlaneParams parm;
	NvBufSurfaceParams dst_dma_surface_params;
	dst_dma_surface_params = fb->dst_dma_surface->surfaceList[0];
	parm = dst_dma_surface_params.planeParams;
#else
	NvBufferParams parm;
	int ret = NvBufferGetParams(fb->dst_dma_fd, &parm);
	TEST_ERROR(ret < 0, "Failed to get dst dma buf params", ret);
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
	//if(bufNumber <= 0) return false; //TODO log msg //TODO check if it's already allocated and deinit first
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
		NVMPI_frameBuf* fb = new NVMPI_frameBuf();
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

//TODO: accept in nvmpi_create_decoder input stream params (width and height, etc...) from ffmpeg.
//Public API: create and start a decoder.
//Opens the V4L2 decoder device, subscribes to resolution-change events,
//configures the OUTPUT (bitstream) plane with USERPTR buffers and starts
//streaming on it, then spawns the capture thread. The CAPTURE plane and
//frame pool are NOT set up here — that happens on the capture thread once
//the first resolution-change event reveals the stream geometry.
//Returns the new context (errors are currently only logged, not returned).
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
		std::cerr << "[libnvmpi][E]: P010 (10-bit) decode requires the NvUtils buffer API (JetPack 5+); not available in this build." << std::endl;
		return NULL;
	}
#endif

	nvmpictx* ctx=new nvmpictx();

	ctx->dec = NvVideoDecoder::createVideoDecoder("dec0");
	TEST_ERROR(!ctx->dec, "Could not create decoder",ret);

	ret=ctx->dec->subscribeEvent(V4L2_EVENT_RESOLUTION_CHANGE, 0, 0);
	TEST_ERROR(ret < 0, "Could not subscribe to V4L2_EVENT_RESOLUTION_CHANGE", ret);
	
	ctx->frame_pool_size = param->frame_pool_size;
	ctx->max_perf = param->max_perf;
	ctx->disable_dpb = param->disable_dpb;

	//0 keeps the default; out-of-range values (including garbage from
	//callers built against an older nvDecParam layout) fall back too.
	if(param->chunk_size >= 65536 && param->chunk_size <= 64u*1024*1024)
		ctx->chunk_size = param->chunk_size;
	else if(param->chunk_size != 0)
		std::cerr << "[libnvmpi][W]: chunk_size " << param->chunk_size
		          << " out of range [65536, 67108864]; using default " << ctx->chunk_size << std::endl;

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

	TEST_ERROR(ret < 0, "Could not set output plane format", ret);

	//input mode 0 = feed complete NAL units / frames per buffer
	ret = ctx->dec->setFrameInputMode(0);
	TEST_ERROR(ret < 0, "Error in decoder setFrameInputMode for NALU", ret);
	
	if(ctx->disable_dpb)
	{
		ret = ctx->dec->disableDPB();
		TEST_ERROR(ret < 0, "Error in decoder disableDPB", ret);
	}

	if(ctx->max_perf)
	{
		ret = ctx->dec->setMaxPerfMode(1);
		TEST_ERROR(ret < 0, "Error while setting decoder to max perf", ret);
	}

	//10 USERPTR buffers on the OUTPUT plane; packet data is memcpy'd into
	//them in nvmpi_decoder_put_packet()
	ret = ctx->dec->output_plane.setupPlane(V4L2_MEMORY_USERPTR, 10, false, true);
	TEST_ERROR(ret < 0, "Error while setting up output plane", ret);

	ctx->dec->output_plane.setStreamStatus(true);
	TEST_ERROR(ret < 0, "Error in output plane stream on", ret);

	ctx->out_pixfmt=param->pixFormat;
	ctx->resized = param->resized;
	ctx->framePool = new NVMPI_bufPool<NVMPI_frameBuf*>();
	ctx->eos=false;
	ctx->index=0;
	for(int index=0;index<MAX_BUFFERS;index++)
		ctx->dmaBufferFileDescriptor[index]=-1;
	ctx->numberCaptureBuffers=0;
	ctx->dec_capture_loop = std::thread(dec_capture_loop_fcn,ctx);
	pthread_setname_np(ctx->dec_capture_loop.native_handle(), "dec_capture");

	return ctx;
}

//Public API: feed one compressed packet to the decoder OUTPUT plane.
//Buffer acquisition strategy: first use each of the plane's buffers once
//(by index), afterwards block in dqBuffer(-1 = infinite) until the decoder
//releases one. The packet payload is memcpy'd, so the caller keeps
//ownership of it. pts (microseconds) is carried in the V4L2 timestamp.
//A zero-sized payload is the EOS marker and flips ctx->eos.
//Returns 0 on success, -1 on dequeue failure, -2 on queue failure,
//-3 when the packet exceeds chunk_size (packet dropped, decoder usable).
int nvmpi_decoder_put_packet(nvmpictx* ctx,nvPacket* packet)
{
	int ret;
	struct v4l2_buffer v4l2_buf;
	struct v4l2_plane planes[MAX_PLANES];
	NvBuffer *nvBuffer;

	//reject packets larger than the V4L2 input buffers before dequeuing
	//anything — copying would overflow the plane buffer. Distinct return
	//code: callers must treat this as invalid input data, not as a
	//transient/hardware failure (the FFmpeg wrapper maps it accordingly).
	if (packet->payload_size > ctx->chunk_size)
	{
		std::cerr << "[libnvmpi][E]: input packet (" << packet->payload_size
		          << " bytes) exceeds chunk_size (" << ctx->chunk_size
		          << "); dropping. Increase the chunk_size option." << std::endl;
		return -3;
	}

	memset(&v4l2_buf, 0, sizeof(v4l2_buf));
	memset(planes, 0, sizeof(planes));

	v4l2_buf.m.planes = planes;

	if (ctx->index < (int)ctx->dec->output_plane.getNumBuffers())
	{
		nvBuffer = ctx->dec->output_plane.getNthBuffer(ctx->index);
		v4l2_buf.index = ctx->index;
		ctx->index++;
	}
	else
	{
		ret = ctx->dec->output_plane.dqBuffer(v4l2_buf, &nvBuffer, NULL, -1);
		if (ret < 0)
		{
			cerr << "Error DQing buffer at output plane" << std::endl;
			return -1;
		}
	}

	memcpy(nvBuffer->planes[0].data,packet->payload,packet->payload_size);
	nvBuffer->planes[0].bytesused=packet->payload_size;
	v4l2_buf.m.planes[0].bytesused = nvBuffer->planes[0].bytesused;

	v4l2_buf.flags |= V4L2_BUF_FLAG_TIMESTAMP_COPY;
	v4l2_buf.timestamp.tv_sec = packet->pts / 1000000;
	v4l2_buf.timestamp.tv_usec = packet->pts % 1000000;

	ret = ctx->dec->output_plane.qBuffer(v4l2_buf, NULL);
	if (ret < 0)
	{
		std::cerr << "Error Qing buffer at output plane" << std::endl;
		ctx->index--;
		return -2;
	}

	if (v4l2_buf.m.planes[0].bytesused == 0)
	{
		ctx->eos=true;
		//std::cout << "Input file read complete" << std::endl; //TODO log it
	}

	return 0;
}

//Copy one filled DMA frame buffer into the caller's nvFrame planes.
//Per plane: map the dmabuf for CPU access, sync caches (device -> CPU),
//then copy line by line because the source pitch (hw-chosen) and the
//destination linesize (caller-chosen) usually differ; only
//frame_linedatasize valid bytes per line are copied. Unmaps when done.
//frame->payload[] must point at sufficiently large caller-owned memory.
int copyNvBufToFrame(nvmpictx* ctx, NVMPI_frameBuf *nvmpiBuf, nvFrame* frame)
{
	int ret;
	char *dataDst;
	char *dataSrc;

	for(unsigned int plane=0; plane<ctx->num_planes; plane++)
	{
#ifdef WITH_NVUTILS
		NvBufSurface *nvbuf_surf = nvmpiBuf->dst_dma_surface;
		ret = NvBufSurfaceMap(nvbuf_surf, 0, plane, NVBUF_MAP_READ_WRITE);
		NvBufSurfaceSyncForCpu (nvbuf_surf, 0, plane);
		dataSrc = (char *)nvbuf_surf->surfaceList[0].mappedAddr.addr[plane];
#else
		int dmabuf_fd = nvmpiBuf->dst_dma_fd;
		void *psrc_data;
		ret = NvBufferMemMap(dmabuf_fd, plane, NvBufferMem_Read_Write, &psrc_data);
		NvBufferMemSyncForCpu(dmabuf_fd, plane, &psrc_data);
		dataSrc = (char *)psrc_data;
#endif
		if(ret != 0)
		{
			fprintf(stderr, "NvBufferMap failed \n");
			return ret;
		}
		
		dataDst = (char *)frame->payload[plane];
		unsigned int &dstFrameLineSize = frame->linesize[plane];
		unsigned int &srcFrameLineSize = ctx->frame_linesize[plane];
		unsigned int &copySz = ctx->frame_linedatasize[plane];
		
		for (unsigned int i = 0; i < ctx->frame_height[plane]; i++)
		{
			memcpy(dataDst, dataSrc, copySz);
			dataDst += dstFrameLineSize;
			dataSrc += srcFrameLineSize;
		}
		
#ifdef WITH_NVUTILS
		NvBufSurfaceUnMap(nvbuf_surf, 0, plane);
#else
		NvBufferMemUnMap(dmabuf_fd, plane, &psrc_data);
#endif
	}
    return 0;
}

//Public API: fetch the next decoded frame.
//Non-blocking: returns -1 immediately when no filled frame is queued (the
//'wait' parameter is currently unused). On success the frame data is
//copied into the caller's buffers, the pts is set, and the pool buffer is
//immediately recycled to the "empty" queue — nothing internal is exposed
//to the caller, so there is no lifetime to manage.
int nvmpi_decoder_get_frame(nvmpictx* ctx,nvFrame* frame,bool wait)
{
	(void)wait; //blocking mode not implemented yet — see TODO.md
	int ret;
	NVMPI_frameBuf* fb = ctx->framePool->dqFilledBuf();
	if(!fb) return -1;
	
	ret = copyNvBufToFrame(ctx, fb, frame);
	frame->timestamp=fb->timestamp;
	
	//return buffer to pool
	ctx->framePool->qEmptyBuf(fb);
	
	return ret;
}

//Public API: shut the decoder down and free everything.
//
//Reset the decoder pipeline without destroying it (seek / stream restart).
//
//Sequence:
//  1. Signal EOS and STREAMOFF capture — stops the capture thread.
//  2. Join the capture thread.
//  3. STREAMOFF output plane — aborts any in-flight V4L2 buffers.
//  4. Drain the frame pool (move filled frames back to the empty queue).
//  5. Reset EOS flag and output-plane buffer index.
//  6. STREAMON output plane.
//  7. Restart the capture thread — it will wait for the next
//     resolution-change event (triggered when the caller re-primes
//     extradata / SPS / PPS), then reinitialize the capture plane
//     and frame pool via respondToResolutionEvent().
//
//The caller MUST re-prime extradata after this call so the hardware
//decoder can reconfigure its capture plane (same path as initial setup).
int nvmpi_decoder_flush(nvmpictx* ctx)
{
	ctx->eos = true;
	ctx->dec->capture_plane.setStreamStatus(false);
	if (ctx->dec_capture_loop.joinable())
		ctx->dec_capture_loop.join();

	ctx->dec->output_plane.setStreamStatus(false);

	NVMPI_frameBuf* fb;
	while ((fb = ctx->framePool->dqFilledBuf()))
		ctx->framePool->qEmptyBuf(fb);

	ctx->eos = false;
	ctx->index = 0;

	ctx->dec->output_plane.setStreamStatus(true);

	ctx->dec_capture_loop = std::thread(dec_capture_loop_fcn, ctx);
	pthread_setname_np(ctx->dec_capture_loop.native_handle(), "dec_capture");

	return 0;
}

//Teardown order (each step depends on the previous):
//  1. Signal EOS and STREAMOFF capture plane — unblocks the capture thread.
//  2. Join the capture thread — guarantees no transforms are in flight.
//  3. Destroy the VIC transform session (legacy path only).
//  4. Stop the output (input-side) plane and release its V4L2 buffers.
//  5. Release CAPTURE-plane DMA buffers (decoder output side).
//  6. Destroy frame pool (VIC destination buffers) — safe because all
//     planes are stopped and no transforms can be pending.
//  7. Free the pool container, decoder device, and context.
//
//The handle is invalid after this call.
int nvmpi_decoder_close(nvmpictx* ctx)
{
	ctx->eos=true;
	ctx->dec->capture_plane.setStreamStatus(false);
	if (ctx->dec_capture_loop.joinable())
	{
		ctx->dec_capture_loop.join();
	}

#ifndef WITH_NVUTILS
	if (ctx->numberCaptureBuffers > 0)
		NvBufferSessionDestroy(ctx->session);
#endif

	ctx->dec->output_plane.setStreamStatus(false);
	ctx->dec->output_plane.deinitPlane();

	ctx->deinitDecoderCapturePlane();

	ctx->deinitFramePool();
	delete ctx->framePool;
	ctx->framePool = nullptr;

	delete ctx->dec;
	ctx->dec = nullptr;

	delete ctx;

	return 0;
}


