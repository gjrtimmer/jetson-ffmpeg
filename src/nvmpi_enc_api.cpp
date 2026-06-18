/*
 * nvmpi_enc_api.cpp — public encoder API of libnvmpi (layer 1).
 *
 * Implements the encoder half of the public C API in include/nvmpi.h on top
 * of NVIDIA's V4L2 NvVideoEncoder sample class. Consumed by the FFmpeg
 * wrapper in ffmpeg/dev/common/libavcodec/nvmpi_enc.c. Encodes H.264/HEVC.
 *
 * V4L2 encoder model (M2M device, two queues — note the naming is from the
 * device's point of view, mirrored relative to the decoder):
 *   - OUTPUT plane  = input side: raw YUV frames go in.
 *   - CAPTURE plane = output side: encoded bitstream comes out.
 *
 * Data flow / threading:
 *   user thread:      nvmpi_encoder_put_frame() copies the caller's planes
 *                     into an OUTPUT-plane buffer and queues it (blocks in
 *                     dqBuffer once all buffers are in flight).
 *   DQ thread:        NvVideoEncoder's capture-plane dequeue thread invokes
 *                     encoder_capture_plane_dq_callback() per encoded
 *                     buffer; the callback copies the bitstream into an
 *                     "empty" nvPacket from pktPool and publishes it as
 *                     "filled", then re-queues the V4L2 buffer.
 *   user thread:      nvmpi_encoder_get_packet() pops filled packets;
 *                     the caller returns them via qEmptyPacket().
 *
 * The nvPacket pool itself is filled by the caller (the FFmpeg wrapper
 * allocates packets backed by AVPacket buffers) — libnvmpi never allocates
 * or frees packet memory.
 *
 * Companion files:
 *   nvmpi_enc_internal.h  — shared includes, defines, nvmpictx struct,
 *                           and forward declarations.
 *   nvmpi_enc_output.cpp  — encoder_capture_plane_dq_callback and
 *                           setup_output_dmabuf (OUTPUT-plane DMA path).
 */
#include "nvmpi_enc_internal.h"

//Public API: create and start an encoder.
//Copies/translates all nvEncParam settings into the context, then programs
//the V4L2 encoder: plane formats, bitrate & rate control, profile/level,
//presets, GOP structure, QP range, SPS/PPS insertion, framerate; sets up
//both planes, STREAMONs them, starts the capture DQ thread and pre-queues
//all empty CAPTURE buffers. Returns the new context (setup errors are only
//logged). The caller still must populate the packet pool afterwards.
nvmpictx* nvmpi_create_encoder(nvEncParam* param)
{
	int ret;
	log_level = LOG_LEVEL_INFO;
	//log_level = LOG_LEVEL_DEBUG;
	nvmpictx *ctx=new nvmpictx;
	ctx->index=0;
	ctx->width=param->width;
	ctx->height=param->height;
	ctx->enableLossless=false;
	ctx->bitrate=param->bitrate;
	ctx->ratecontrol = V4L2_MPEG_VIDEO_BITRATE_MODE_CBR;
	ctx->idr_interval = param->idr_interval;
	ctx->fps_n = param->fps_n;
	ctx->fps_d = param->fps_d;
	ctx->iframe_interval = param->iframe_interval;
	ctx->pktPool = new NVMPI_bufPool<nvPacket*>();
	ctx->enable_extended_colorformat=false;
	ctx->packets_num=param->capture_num;
#if (OUTPLANE_MEMTYPE == OUTPLANE_MEMTYPE_DMA)
	ctx->output_plane_fd = new int[ctx->packets_num];
#endif
	ctx->qmax=param->qmax;
	ctx->qmin=param->qmin;
	ctx->num_b_frames=param->max_b_frames;
	ctx->num_reference_frames=param->refs;
	ctx->insert_sps_pps_at_idr=(param->insert_spspps_idr==1)?true:false;
	ctx->insert_vui=(param->insert_vui!=0)?true:false;
	ctx->capPlaneGotEOS.store(false, std::memory_order_relaxed);
	ctx->flushing.store(false, std::memory_order_relaxed);
	ctx->blocking_mode = true; //TODO non-blocking mode support
	ctx->max_perf = param->max_perf;
	ctx->poc_type = param->poc_type;
	ctx->vbv_buffer_size = param->vbv_buffer_size;

	//Profile mapping: the caller passes FFmpeg-style H.264 profile ids
	//(numeric values of FF_PROFILE_H264_*); translate to V4L2 enums.
	//Unknown/unset values fall back to Main.
	switch(param->profile)
	{
		case 77://FF_PROFILE_H264_MAIN
			ctx->profile=V4L2_MPEG_VIDEO_H264_PROFILE_MAIN;
			break;
		case 66://FF_PROFILE_H264_BASELINE
			ctx->profile=V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE;
			break;
		case 100://FF_PROFILE_H264_HIGH
			ctx->profile=V4L2_MPEG_VIDEO_H264_PROFILE_HIGH;
			break;

		default:
			ctx->profile=V4L2_MPEG_VIDEO_H264_PROFILE_MAIN;
			break;
	}

	//Level mapping: level_idc style values (10*level, e.g. 41 = 4.1) to
	//V4L2 enums; defaults to 5.1 (also for the "auto"/0 option value).
	switch(param->level)
	{
		case 10:
			ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_1_0;
			break;
		case 11:
			ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_1_1;
			break;
		case 12:
			ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_1_2;
			break;
		case 13:
			ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_1_3;
			break;
		case 20:
			ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_2_0;
			break;
		case 21:
			ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_2_1;
			break;
		case 22:
			ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_2_2;
			break;
		case 30:
			ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_3_0;
			break;
		case 31:
			ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_3_1;
			break;
		case 32:
			ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_3_2;
			break;
		case 40:
			ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_4_0;
			break;
		case 41:
			ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_4_1;
			break;
		case 42:
			ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_4_2;
			break;
		case 50:
			ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_5_0;
			break;
		case 51:
			ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_5_1;
			break;
		default:
			ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_5_1;
			break;
	}

	//HW preset mapping: 1=ultrafast .. 4=slow (default medium); trades
	//encoding speed against quality inside the hw encoder.
	switch(param->hw_preset_type)
	{
		case 1:
			ctx->hw_preset_type = V4L2_ENC_HW_PRESET_ULTRAFAST;
			break;
		case 2:
			ctx->hw_preset_type = V4L2_ENC_HW_PRESET_FAST;
			break;
		case 3:
			ctx->hw_preset_type = V4L2_ENC_HW_PRESET_MEDIUM;
			break;
		case 4:
			ctx->hw_preset_type = V4L2_ENC_HW_PRESET_SLOW;
			break;
		default:
			ctx->hw_preset_type = V4L2_ENC_HW_PRESET_MEDIUM;
			break;
	}

	if(param->enableLossless)
		ctx->enableLossless=true;

	//rate control: CBR by default, VBR when requested
	if(param->mode_vbr)
		ctx->ratecontrol=V4L2_MPEG_VIDEO_BITRATE_MODE_VBR;

	if(param->codingType==NV_VIDEO_CodingH264)
	{
		ctx->encoder_pixfmt=V4L2_PIX_FMT_H264;
	}else if(param->codingType==NV_VIDEO_CodingHEVC)
	{
		ctx->encoder_pixfmt=V4L2_PIX_FMT_H265;
	}
	if(ctx->blocking_mode)
	{
		ctx->enc.reset(NvVideoEncoder::createVideoEncoder("enc0"));
	}
	else
	{
		ctx->enc.reset(NvVideoEncoder::createVideoEncoder("enc0", O_NONBLOCK));
	}
	/* Factory returns NULL on failure; bail out instead of continuing
	 * with a half-initialized context (TEST_ERROR only logged). */
	if (!ctx->enc)
	{
		std::cerr << "Could not create encoder" << std::endl;
		delete ctx;
		return NULL;
	}

	//CAPTURE plane carries the compressed bitstream; each buffer is sized
	//NVMPI_ENC_CHUNK_SIZE — the same constant the wrapper uses for packets
	ret = ctx->enc->setCapturePlaneFormat(ctx->encoder_pixfmt, ctx->width,ctx->height, NVMPI_ENC_CHUNK_SIZE);

	TEST_ERROR(ret < 0, "Could not set output plane format", ret);

	//pick the raw input format. Priority: lossless (YUV444M, set below) >
	//HEVC Main10 (10-bit P010M) > requested 8-bit layout (NV12M vs YUV420M).
	//NV12 only affects the 8-bit default branch; it never overrides the
	//10-bit or lossless paths.
	switch (ctx->profile)
	{
		case V4L2_MPEG_VIDEO_H265_PROFILE_MAIN10:
			ctx->raw_pixfmt = V4L2_PIX_FMT_P010M;
			break;
		case V4L2_MPEG_VIDEO_H265_PROFILE_MAIN:
		default:
			ctx->raw_pixfmt = (param->pixFormat == NV_PIX_NV12) ? V4L2_PIX_FMT_NV12M : V4L2_PIX_FMT_YUV420M;
	}

	//lossless H.264 requires the High 4:4:4 Predictive profile and YUV444
	//input; otherwise use the raw format chosen above
	if (ctx->enableLossless && param->codingType == NV_VIDEO_CodingH264)
	{
		ctx->profile = V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_444_PREDICTIVE;
		ret = ctx->enc->setOutputPlaneFormat(V4L2_PIX_FMT_YUV444M, ctx->width,ctx->height);
	}
	else
	{
		ret = ctx->enc->setOutputPlaneFormat(ctx->raw_pixfmt, ctx->width,ctx->height);
	}

	TEST_ERROR(ret < 0, "Could not set output plane format", ret);

	ret = ctx->enc->setBitrate(ctx->bitrate);
	TEST_ERROR(ret < 0, "Could not set encoder bitrate", ret);

	if(ctx->vbv_buffer_size)
	{
		/* Set virtual buffer size value for encoder */
		ret = ctx->enc->setVirtualBufferSize(ctx->vbv_buffer_size);
		TEST_ERROR(ret < 0, "Could not set virtual buffer size", ret);
	}

	ret=ctx->enc->setHWPresetType(ctx->hw_preset_type);
	TEST_ERROR(ret < 0, "Could not set encoder HW Preset Type", ret);

	if(ctx->num_reference_frames)
	{
		ret = ctx->enc->setNumReferenceFrames(ctx->num_reference_frames);
		TEST_ERROR(ret < 0, "Could not set num reference frames", ret);
	}

	if(ctx->num_b_frames != (uint32_t) -1 && param->codingType == NV_VIDEO_CodingH264)
	{
		ret = ctx->enc->setNumBFrames(ctx->num_b_frames);
		TEST_ERROR(ret < 0, "Could not set number of B Frames", ret);
	}


	if(param->codingType == NV_VIDEO_CodingH264 || param->codingType == NV_VIDEO_CodingHEVC)
	{
		ret = ctx->enc->setProfile(ctx->profile);
		TEST_ERROR(ret < 0, "Could not set encoder profile", ret);
	}

	if(param->codingType== NV_VIDEO_CodingH264)
	{
		ret = ctx->enc->setLevel(ctx->level);
		TEST_ERROR(ret < 0, "Could not set encoder level", ret);
	}


	//Rate control: lossless mode pins QP to 0 (no rate control); otherwise
	//apply CBR/VBR, and for VBR also program a peak bitrate.
	if (ctx->enableLossless)
	{
		ret = ctx->enc->setConstantQp(0);
		TEST_ERROR(ret < 0, "Could not set encoder constant qp=0", ret);

	}
	else
	{
		ret = ctx->enc->setRateControlMode(ctx->ratecontrol);
		TEST_ERROR(ret < 0, "Could not set encoder rate control mode", ret);

		if (ctx->ratecontrol == V4L2_MPEG_VIDEO_BITRATE_MODE_VBR)
		{
			uint32_t peak_bitrate;
			//TODO log warning?
			//peak must be >= target; derive 1.2x target if unset/invalid
			if (ctx->peak_bitrate < ctx->bitrate)
				peak_bitrate = 1.2f * ctx->bitrate;
			else
				peak_bitrate = ctx->peak_bitrate;
			ret = ctx->enc->setPeakBitrate(peak_bitrate);
			TEST_ERROR(ret < 0, "Could not set encoder peak bitrate", ret);
		}
	}

	ret = ctx->enc->setIDRInterval(ctx->idr_interval);
	TEST_ERROR(ret < 0, "Could not set encoder IDR interval", ret);

	//same QP range applied to I, P and B frames
	if(ctx->qmax>0 ||ctx->qmin >0){
		ctx->enc->setQpRange(ctx->qmin, ctx->qmax, ctx->qmin,ctx->qmax, ctx->qmin, ctx->qmax);
	}
	ret = ctx->enc->setIFrameInterval(ctx->iframe_interval);
	TEST_ERROR(ret < 0, "Could not set encoder I-Frame interval", ret);

	if(ctx->max_perf)
	{
		ret = ctx->enc->setMaxPerfMode(1);
		TEST_ERROR(ret < 0, "Error while setting encoder to max perf", ret);
	}

	if(ctx->poc_type)
	{
		ret = ctx->enc->setPocType(ctx->poc_type);
		TEST_ERROR(ret < 0, "Error while setting encoder poc_type", ret);
	}

	if(ctx->insert_sps_pps_at_idr){
		ret = ctx->enc->setInsertSpsPpsAtIdrEnabled(true);
		TEST_ERROR(ret < 0, "Could not set insertSPSPPSAtIDR", ret);
	}

	//VUI carries timing_info (fps) in the SPS so players/muxers report the
	//correct frame rate. Independent of SPS/PPS-at-IDR. Must be set after
	//setFormat (done above) and before STREAMON; the fps value comes from
	//setFrameRate below, which the firmware reads together with this flag.
	if(ctx->insert_vui){
		ret = ctx->enc->setInsertVuiEnabled(true);
		TEST_ERROR(ret < 0, "Could not set insertVUI", ret);
	}

	ret = ctx->enc->setFrameRate(ctx->fps_n, ctx->fps_d);
	TEST_ERROR(ret < 0, "Could not set framerate", ret);

	//ret = ctx->enc->output_plane.setupPlane(V4L2_MEMORY_USERPTR, ctx->packets_num, false, true);
#if (OUTPLANE_MEMTYPE == OUTPLANE_MEMTYPE_MMAP)
	ret = ctx->enc->output_plane.setupPlane(V4L2_MEMORY_MMAP, ctx->packets_num, true, false);
#else
	ret = setup_output_dmabuf(ctx,ctx->packets_num); //V4L2_MEMORY_DMABUF
#endif
	TEST_ERROR(ret < 0, "Could not setup output plane", ret);

	ret = ctx->enc->capture_plane.setupPlane(V4L2_MEMORY_MMAP, ctx->packets_num, true, false);
	TEST_ERROR(ret < 0, "Could not setup capture plane", ret);

	ret = ctx->enc->subscribeEvent(V4L2_EVENT_EOS,0,0);
	TEST_ERROR(ret < 0, "Could not subscribe EOS event", ret);

	ret = ctx->enc->output_plane.setStreamStatus(true);
	TEST_ERROR(ret < 0, "Error in output plane streamon", ret);

	ret = ctx->enc->capture_plane.setStreamStatus(true);
	TEST_ERROR(ret < 0, "Error in capture plane streamon", ret);

	if(ctx->blocking_mode)
	{
		ctx->enc->capture_plane.setDQThreadCallback(encoder_capture_plane_dq_callback);
		ctx->enc->capture_plane.startDQThread(ctx);
	}
    else
    {
		/*
        sem_init(&ctx->pollthread_sema, 0, 0);
        sem_init(&ctx->encoderthread_sema, 0, 0);
        // Set encoder poll thread for non-blocking io mode
        pthread_create(&ctx->enc_pollthread, NULL, encoder_pollthread_fcn, ctx);
        pthread_setname_np(ctx->enc_pollthread, "EncPollThread");
        cout << "Created the PollThread and Encoder Thread \n";
        */
    }

	// Enqueue all the empty capture plane buffers
	for (uint32_t i = 0; i < ctx->enc->capture_plane.getNumBuffers(); i++){
		struct v4l2_buffer v4l2_buf;
		struct v4l2_plane planes[MAX_PLANES];
		memset(&v4l2_buf, 0, sizeof(v4l2_buf));
		memset(planes, 0, MAX_PLANES * sizeof(struct v4l2_plane));

		v4l2_buf.index = i;
		v4l2_buf.m.planes = planes;

		ret = ctx->enc->capture_plane.qBuffer(v4l2_buf, NULL);
		TEST_ERROR(ret < 0, "Error while queueing buffer at capture plane", ret);

	}

	return ctx;
}

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
			cerr << "Error while mapping buffer at output plane" << endl;
		}
#endif
	}
	else
	{
		/*TODO move it to another thread or make this call non-blocking.
		 * DQBuffer could take considerable time. e.g. when encoder performance is 20fps and all output_plane is busy
		 * it could take up to 1000/20=50ms... latency
		 */
		ret = ctx->enc->output_plane.dqBuffer(v4l2_buf, &nvBuffer, NULL, -1);
		if (ret < 0)
		{
			cerr << "Error DQing buffer at output plane" << std::endl;
			return false;
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
			cerr << "Error while NvBufSurfaceFromFd" << endl;
		}
		ret = NvBufSurfaceSyncForDevice (nvbuf_surf, 0, j);
		if (ret < 0)
		{
			cerr << "Error while NvBufSurfaceSyncForDevice at output plane for V4L2_MEMORY_DMABUF" << endl;
		}
#else
		ret = NvBufferMemSyncForDevice (nvBuffer->planes[j].fd, j, (void **)&nvBuffer->planes[j].data);
		if (ret < 0)
		{
			cerr << "Error while NvBufferMemSyncForDevice at output plane for V4L2_MEMORY_DMABUF" << endl;
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
	TEST_ERROR(ret < 0, "Error while queueing buffer at output plane", ret);

	return 0;
}

//Public API: take an empty packet out of the pool (e.g. to free or replace
//it). Non-blocking: -1 when the empty queue is exhausted. The caller
//assumes ownership of the returned packet.
int nvmpi_encoder_dqEmptyPacket(nvmpictx* ctx,nvPacket** packet)
{
	nvPacket* pkt = ctx->pktPool->dqEmptyBuf();
	if(!pkt) return -1;
	*packet = pkt;
	return 0;
}

//Public API: donate an empty, caller-allocated packet to the pool so the
//capture DQ callback can fill it. The pool stores only the pointer; the
//caller remains responsible for the packet's eventual deallocation.
void nvmpi_encoder_qEmptyPacket(nvmpictx* ctx,nvPacket* packet)
{
	ctx->pktPool->qEmptyBuf(packet);
	return;
}

//Public API: fetch the next encoded packet.
//While encoding: non-blocking, -1 when nothing is ready (wrapper maps this
//to EAGAIN). While flushing: poll every 1ms until either a packet arrives
//(0) or the DQ callback saw the EOS buffer and no packet remains (-2).
//On success the caller holds the packet until re-queueing it via
//nvmpi_encoder_qEmptyPacket().
int nvmpi_encoder_get_packet(nvmpictx* ctx,nvPacket** packet)
{
	nvPacket* pkt = ctx->pktPool->dqFilledBuf();

	if(!pkt)
	{
		if(!ctx->flushing.load(std::memory_order_acquire)) return -1;
		bool wait = true;
		while(wait)
		{
			pkt = ctx->pktPool->dqFilledBuf();
			if(pkt || ctx->capPlaneGotEOS.load(std::memory_order_acquire)) wait = false;
			else std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		if(!pkt) return -2; //if got eos
	}

	*packet = pkt;
	return 0;
}

//Public API: stop the encoder and free the context.
//Stops/joins the capture DQ thread, releases self-allocated OUTPUT-plane
//dmabufs (DMA mode only), then destroys the device and the (by now empty)
//packet pool. Packets still inside the pool are NOT freed here — the
//FFmpeg wrapper drains and frees them first (nvmpienc_deinitPktPool).
int nvmpi_encoder_close(nvmpictx* ctx)
{
	if(ctx->blocking_mode)
	{
		ctx->enc->capture_plane.stopDQThread();
		ctx->enc->capture_plane.waitForDQThread(1000);
	}
	else
	{
		//sem_destroy(&ctx.pollthread_sema);
		//sem_destroy(&ctx.encoderthread_sema);
	}

#if (OUTPLANE_MEMTYPE == OUTPLANE_MEMTYPE_DMA)
	int ret;
    if(ctx->enc)
    {
        for (uint32_t i = 0; i < ctx->enc->output_plane.getNumBuffers(); i++)
        {
            // Unmap output plane buffer for memory type DMABUF.
            ret = ctx->enc->output_plane.unmapOutputBuffers(i, ctx->output_plane_fd[i]);
            if (ret < 0)
            {
                cerr << "Error while unmapping buffer at output plane" << endl;
            }

            ret = NvBufSurf::NvDestroy(ctx->output_plane_fd[i]);
            ctx->output_plane_fd[i] = -1;
            if(ret < 0)
            {
                cerr << "Failed to Destroy NvBuffer\n" << endl;
                return ret;
            }
        }
    }
    delete[] ctx->output_plane_fd;
    #endif

	/* unique_ptr: release the NvVideoEncoder after the DQ thread is
	 * joined and all V4L2 buffers are unmapped. */
	ctx->enc.reset();
	delete ctx->pktPool;
	delete ctx;
	return 0;
}
