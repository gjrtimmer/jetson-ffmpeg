/*
 * nvmpi_enc_init.cpp — encoder creation/initialization (layer 1).
 *
 * Implements nvmpi_create_encoder() from the public C API in include/nvmpi.h
 * on top of NVIDIA's V4L2 NvVideoEncoder sample class. Consumed by the
 * FFmpeg wrapper in ffmpeg/dev/common/libavcodec/nvmpi_enc.c.
 *
 * V4L2 encoder model (M2M device, two queues — note the naming is from the
 * device's point of view, mirrored relative to the decoder):
 *   - OUTPUT plane  = input side: raw YUV frames go in.
 *   - CAPTURE plane = output side: encoded bitstream comes out.
 *
 * This file owns the device-open, format-negotiation, and plane/stream
 * setup sequence. Frame submission (nvmpi_encoder_put_frame[_fd]) lives in
 * nvmpi_enc_input.cpp; packet retrieval and control API (get_packet,
 * force_idr, set_bitrate, flush, close) live in nvmpi_enc_api.cpp.
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
	/* Always allocate the output-plane fd array: used by both compile-time
	 * DMA mode (OUTPLANE_MEMTYPE_DMA) and runtime external DMABUF mode
	 * (dmabuf_external). Initialized to -1 so cleanup knows which slots
	 * were filled. Freed in nvmpi_encoder_close(). */
	ctx->output_plane_fd = new int[ctx->packets_num];
	for (uint32_t i = 0; i < ctx->packets_num; i++)
		ctx->output_plane_fd[i] = -1;
	ctx->qmax=param->qmax;
	ctx->qmin=param->qmin;
	ctx->num_b_frames=param->max_b_frames;
	ctx->num_reference_frames=param->refs;
	ctx->insert_sps_pps_at_idr=(param->insert_spspps_idr==1)?true:false;
	ctx->insert_vui=(param->insert_vui!=0)?true:false;
	ctx->insert_aud=(param->insert_aud!=0)?true:false;
	ctx->enable_cabac=(param->enable_cabac!=0)?true:false;
	ctx->capPlaneGotEOS.store(false, std::memory_order_relaxed);
	ctx->flushing.store(false, std::memory_order_relaxed);
	/* Non-blocking mode: put_frame returns NVMPI_ERR_EAGAIN instead of
	 * blocking when no OUTPUT-plane buffer is available. Default (0) =
	 * blocking, preserving backward compatibility with existing callers. */
	ctx->blocking_mode = !param->nonblocking;
	ctx->dmabuf_external = (param->use_dmabuf != 0);
	ctx->max_perf = param->max_perf;
	ctx->poc_type = param->poc_type;
	ctx->vbv_buffer_size = param->vbv_buffer_size;

	/* Blocking-wait timeout: 0 = default (500ms), otherwise clamp to
	 * [100, 5000]ms — same range as the decoder (issue #10). */
	if (param->wait_timeout > 0)
		ctx->wait_timeout_ms = std::max(100u, std::min(param->wait_timeout, 5000u));
	else
		ctx->wait_timeout_ms = 500;

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
	//V4L2 enums. Both H.264 and HEVC share the same level_idc convention
	//from the FFmpeg wrapper; the correct V4L2 enum family is selected by
	//codingType. Default: 5.1 (also for the "auto"/0 option value).
	if (param->codingType == NV_VIDEO_CodingH264)
	{
		switch(param->level)
		{
			case 10: ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_1_0; break;
			case 11: ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_1_1; break;
			case 12: ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_1_2; break;
			case 13: ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_1_3; break;
			case 20: ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_2_0; break;
			case 21: ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_2_1; break;
			case 22: ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_2_2; break;
			case 30: ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_3_0; break;
			case 31: ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_3_1; break;
			case 32: ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_3_2; break;
			case 40: ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_4_0; break;
			case 41: ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_4_1; break;
			case 42: ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_4_2; break;
			case 50: ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_5_0; break;
			case 51: ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_5_1; break;
			/* H.264 levels 5.2–6.2: added in linux/v4l2-controls.h from kernel
			 * 5.17+. JetPack 5.x (kernel 5.10) only defines up to 5.1; guard
			 * so builds succeed on older headers — falls through to default. */
#ifdef V4L2_MPEG_VIDEO_H264_LEVEL_5_2
			case 52: ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_5_2; break;
#endif
#ifdef V4L2_MPEG_VIDEO_H264_LEVEL_6_0
			case 60: ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_6_0; break;
#endif
#ifdef V4L2_MPEG_VIDEO_H264_LEVEL_6_1
			case 61: ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_6_1; break;
#endif
#ifdef V4L2_MPEG_VIDEO_H264_LEVEL_6_2
			case 62: ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_6_2; break;
#endif
			default: ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_5_1; break;
		}
	}
	else if (param->codingType == NV_VIDEO_CodingHEVC)
	{
		/* HEVC uses the same level_idc convention (10*level) from the
		 * FFmpeg wrapper. Tegra's V4L2 extensions define per-tier enums
		 * (v4l2_mpeg_video_h265_level in v4l2_nv_extensions.h); default
		 * to Main Tier since FFmpeg's -level does not distinguish tiers. */
		switch(param->level)
		{
			case 10: ctx->level=V4L2_MPEG_VIDEO_H265_LEVEL_1_0_MAIN_TIER; break;
			case 20: ctx->level=V4L2_MPEG_VIDEO_H265_LEVEL_2_0_MAIN_TIER; break;
			case 21: ctx->level=V4L2_MPEG_VIDEO_H265_LEVEL_2_1_MAIN_TIER; break;
			case 30: ctx->level=V4L2_MPEG_VIDEO_H265_LEVEL_3_0_MAIN_TIER; break;
			case 31: ctx->level=V4L2_MPEG_VIDEO_H265_LEVEL_3_1_MAIN_TIER; break;
			case 40: ctx->level=V4L2_MPEG_VIDEO_H265_LEVEL_4_0_MAIN_TIER; break;
			case 41: ctx->level=V4L2_MPEG_VIDEO_H265_LEVEL_4_1_MAIN_TIER; break;
			case 50: ctx->level=V4L2_MPEG_VIDEO_H265_LEVEL_5_0_MAIN_TIER; break;
			case 51: ctx->level=V4L2_MPEG_VIDEO_H265_LEVEL_5_1_MAIN_TIER; break;
			/* HEVC levels 5.2–6.2: NVIDIA extension in v4l2_nv_extensions.h.
			 * Present on JetPack 6+ (Orin); may be absent on older JetPack
			 * or non-Jetson builds — guard for portability. */
#ifdef V4L2_MPEG_VIDEO_H265_LEVEL_5_2_MAIN_TIER
			case 52: ctx->level=V4L2_MPEG_VIDEO_H265_LEVEL_5_2_MAIN_TIER; break;
#endif
#ifdef V4L2_MPEG_VIDEO_H265_LEVEL_6_0_MAIN_TIER
			case 60: ctx->level=V4L2_MPEG_VIDEO_H265_LEVEL_6_0_MAIN_TIER; break;
#endif
#ifdef V4L2_MPEG_VIDEO_H265_LEVEL_6_1_MAIN_TIER
			case 61: ctx->level=V4L2_MPEG_VIDEO_H265_LEVEL_6_1_MAIN_TIER; break;
#endif
#ifdef V4L2_MPEG_VIDEO_H265_LEVEL_6_2_MAIN_TIER
			case 62: ctx->level=V4L2_MPEG_VIDEO_H265_LEVEL_6_2_MAIN_TIER; break;
#endif
			default: ctx->level=V4L2_MPEG_VIDEO_H265_LEVEL_5_1_MAIN_TIER; break;
		}
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
	/* Open the V4L2 encoder device. Retry up to 3 times with 100ms
	 * backoff. Before each factory call, probe the device node ourselves
	 * to capture errno — MMAPI's NvV4l2Element discards it, making EBUSY
	 * indistinguishable from ENODEV at the factory level. See #37.
	 *
	 * Device paths from MMAPI source (NvVideoEncoder.cpp):
	 *   primary: /dev/nvhost-msenc   (nvhost driver, JetPack 4.x)
	 *   fallback: /dev/v4l2-nvenc    (V4L2 kernel driver, JetPack 5+) */
	for (int attempt = 0; attempt < 3; attempt++) {
		/* Probe: resolve the same device path MMAPI would use. */
		const char *dev_path = NULL;
		if (access("/dev/nvhost-msenc", F_OK) == 0)
			dev_path = "/dev/nvhost-msenc";
		else if (access("/dev/v4l2-nvenc", F_OK) == 0)
			dev_path = "/dev/v4l2-nvenc";

		if (dev_path) {
			int probe_fd = open(dev_path, O_RDWR);
			if (probe_fd < 0) {
				int saved_errno = errno;
				if (saved_errno == ENODEV || saved_errno == ENXIO) {
					/* Device node exists but driver not loaded or hw absent —
					 * permanent failure, no point retrying. */
					NVMPI_LOG(NVMPI_LOG_ERROR,
						  "Encoder device %s unavailable (errno=%d: %s)",
						  dev_path, saved_errno, strerror(saved_errno));
					goto cleanup;
				}
				if (saved_errno == EACCES) {
					NVMPI_LOG(NVMPI_LOG_ERROR,
						  "Encoder device %s: permission denied", dev_path);
					goto cleanup;
				}
				if (saved_errno == EBUSY) {
					NVMPI_LOG(NVMPI_LOG_WARN,
						  "Encoder device %s busy (EBUSY), retrying (%d/3)...",
						  dev_path, attempt + 1);
					usleep(100000);
					continue;
				}
				/* Other errno (EIO, etc.) — log and fall through to factory. */
				NVMPI_LOG(NVMPI_LOG_WARN,
					  "Encoder device probe failed (errno=%d: %s), trying factory...",
					  saved_errno, strerror(saved_errno));
			} else {
				/* Probe succeeded — device is accessible. Close the probe fd
				 * immediately and let the factory do the real open. */
				close(probe_fd);
			}
		} else {
			/* Neither device node exists — MMAPI factory will also fail. */
			NVMPI_LOG(NVMPI_LOG_ERROR,
				  "No encoder device node found (/dev/nvhost-msenc or /dev/v4l2-nvenc)");
			goto cleanup;
		}

		/* Always open encoder WITHOUT O_NONBLOCK — the flag applies to ALL
		 * V4L2 ioctls on the fd, including the CAPTURE-plane DQ thread's
		 * dqBuffer. O_NONBLOCK would prevent the DQ thread from blocking
		 * for encoded packets, causing a hang. Non-blocking behavior is
		 * implemented per-call via dqBuffer(timeout=0) on the OUTPUT
		 * plane only (see nvmpi_encoder_put_frame). */
		ctx->enc.reset(NvVideoEncoder::createVideoEncoder("enc0"));
		if (ctx->enc) break;
		if (attempt < 2) {
			NVMPI_LOG(NVMPI_LOG_WARN, "Encoder factory returned NULL, retrying (%d/3)...", attempt + 1);
			usleep(100000);
		}
	}
	if (!ctx->enc)
	{
		NVMPI_LOG(NVMPI_LOG_ERROR, "Could not create encoder after 3 attempts");
		goto cleanup;
	}

	/* CRITICAL FORMAT SETUP — format negotiation must succeed for the
	 * encoder to function. Unlike parameter-tuning calls (setBitrate,
	 * setProfile, etc.) which may fall back to defaults, a format failure
	 * means the V4L2 device is in an unusable state. Abort to cleanup. */

	//CAPTURE plane carries the compressed bitstream; each buffer is sized
	//NVMPI_ENC_CHUNK_SIZE — the same constant the wrapper uses for packets
	ret = ctx->enc->setCapturePlaneFormat(ctx->encoder_pixfmt, ctx->width,ctx->height, NVMPI_ENC_CHUNK_SIZE);
	if (ret < 0) {
		NVMPI_LOG(NVMPI_LOG_ERROR, "Could not set capture plane format");
		goto cleanup;
	}

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
	if (ret < 0) {
		NVMPI_LOG(NVMPI_LOG_ERROR, "Could not set output plane format");
		goto cleanup;
	}

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

	if(param->codingType == NV_VIDEO_CodingH264 || param->codingType == NV_VIDEO_CodingHEVC)
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
			/* peak must be >= target; derive 1.2x target if unset/invalid */
			if (ctx->peak_bitrate < ctx->bitrate) {
				peak_bitrate = 1.2f * ctx->bitrate;
				NVMPI_LOG(NVMPI_LOG_WARN, "peak_bitrate %u below bitrate %u, auto-corrected to %u",
				          ctx->peak_bitrate, ctx->bitrate, peak_bitrate);
			} else
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

	/* CABAC entropy coding (H.264 only): CABAC achieves ~10-15% better
	 * compression than CAVLC at the cost of slightly higher decode
	 * complexity. setCABAC() wraps V4L2_CID_MPEG_VIDEO_H264_ENTROPY_MODE;
	 * must be called after setFormat. Ignored for HEVC (always CABAC). */
	if (ctx->enable_cabac && param->codingType == NV_VIDEO_CodingH264) {
		ret = ctx->enc->setCABAC(true);
		TEST_ERROR(ret < 0, "Could not enable CABAC", ret);
	}

	/* Access Unit Delimiter insertion: some transport streams and HLS
	 * workflows require AUD NALs. setInsertAudEnabled() wraps
	 * V4L2_CID_MPEG_VIDEO_H264_AUD_SAR_ENABLE. */
	if (ctx->insert_aud) {
		ret = ctx->enc->setInsertAudEnabled(true);
		TEST_ERROR(ret < 0, "Could not enable AUD insertion", ret);
	}

	ret = ctx->enc->setFrameRate(ctx->fps_n, ctx->fps_d);
	TEST_ERROR(ret < 0, "Could not set framerate", ret);

	/* CRITICAL PLANE/STREAM SETUP — if any of these fail the V4L2 device
	 * cannot operate; abort to cleanup instead of returning a broken ctx.
	 * Without this, the TEST_ERROR (log-only) macro allowed half-init
	 * encoders to reach the caller; under concurrent stress the resulting
	 * crashes leaked DMA memory and cascaded to all subsequent jobs. */

	//ret = ctx->enc->output_plane.setupPlane(V4L2_MEMORY_USERPTR, ctx->packets_num, false, true);
	if (ctx->dmabuf_external) {
		/* External DMA-BUF mode: OUTPUT plane uses V4L2_MEMORY_DMABUF.
		 * Internal NV12 VIDEO_ENC surfaces are allocated lazily in
		 * put_frame_fd on first use of each slot — this defers NVMM
		 * allocation until after any temporary encoder (extradata
		 * extraction) is destroyed, avoiding NVMM exhaustion on
		 * memory-constrained devices (Orin Nano). */
		ret = ctx->enc->output_plane.setupPlane(V4L2_MEMORY_DMABUF, ctx->packets_num, false, false);
	} else {
#if (OUTPLANE_MEMTYPE == OUTPLANE_MEMTYPE_MMAP)
		ret = ctx->enc->output_plane.setupPlane(V4L2_MEMORY_MMAP, ctx->packets_num, true, false);
#else
		ret = setup_output_dmabuf(ctx,ctx->packets_num); //V4L2_MEMORY_DMABUF
#endif
	}
	if (ret < 0) {
		NVMPI_LOG(NVMPI_LOG_ERROR, "Could not setup output plane");
		goto cleanup;
	}

	ret = ctx->enc->capture_plane.setupPlane(V4L2_MEMORY_MMAP, ctx->packets_num, true, false);
	if (ret < 0) {
		NVMPI_LOG(NVMPI_LOG_ERROR, "Could not setup capture plane");
		goto cleanup;
	}

	ret = ctx->enc->subscribeEvent(V4L2_EVENT_EOS,0,0);
	if (ret < 0) {
		NVMPI_LOG(NVMPI_LOG_ERROR, "Could not subscribe EOS event");
		goto cleanup;
	}

	ret = ctx->enc->output_plane.setStreamStatus(true);
	if (ret < 0) {
		NVMPI_LOG(NVMPI_LOG_ERROR, "Error in output plane streamon");
		goto cleanup;
	}

	ret = ctx->enc->capture_plane.setStreamStatus(true);
	if (ret < 0) {
		NVMPI_LOG(NVMPI_LOG_ERROR, "Error in capture plane streamon");
		goto cleanup;
	}

	/* CAPTURE-plane DQ thread: always started regardless of blocking_mode.
	 * The DQ thread processes encoded output from the CAPTURE plane — needed
	 * in both blocking and non-blocking modes. Non-blocking only changes the
	 * OUTPUT-plane dqBuffer timeout (put_frame), not the capture path. */
	ctx->enc->capture_plane.setDQThreadCallback(encoder_capture_plane_dq_callback);
	ret = ctx->enc->capture_plane.startDQThread(ctx);
	if (ret < 0) {
		NVMPI_LOG(NVMPI_LOG_ERROR, "Could not start DQ thread");
		goto cleanup;
	}
	ctx->dq_thread_started = true;

	// Enqueue all the empty capture plane buffers
	for (uint32_t i = 0; i < ctx->enc->capture_plane.getNumBuffers(); i++){
		struct v4l2_buffer v4l2_buf;
		struct v4l2_plane planes[MAX_PLANES];
		memset(&v4l2_buf, 0, sizeof(v4l2_buf));
		memset(planes, 0, MAX_PLANES * sizeof(struct v4l2_plane));

		v4l2_buf.index = i;
		v4l2_buf.m.planes = planes;

		ret = ctx->enc->capture_plane.qBuffer(v4l2_buf, NULL);
		if (ret < 0) {
			NVMPI_LOG(NVMPI_LOG_ERROR, "Error while queueing buffer at capture plane (buf %u)", i);
			goto cleanup;
		}
	}

	return ctx;

	/* Cleanup on critical V4L2 setup failure. The DQ thread is only
	 * started after STREAMON; dq_thread_started gates the join.
	 * The NvVideoEncoder destructor (via unique_ptr reset) handles
	 * STREAMOFF + V4L2 buffer teardown. Freed here: enc (via reset),
	 * pktPool (allocated at line 60), and ctx itself. */
cleanup:
	if (ctx->dq_thread_started) {
		ctx->enc->capture_plane.stopDQThread();
		ctx->enc->capture_plane.waitForDQThread(1000);
	}
	ctx->enc.reset();
	delete ctx->pktPool;
	/* output_plane_fd entries initialized to -1 on alloc; only destroy
	 * fds that were successfully allocated (fd >= 0). */
	if (ctx->output_plane_fd) {
		for (uint32_t i = 0; i < ctx->packets_num; i++) {
			if (ctx->output_plane_fd[i] >= 0) {
				NvBufferDestroy(ctx->output_plane_fd[i]);
				ctx->output_plane_fd[i] = -1;
			}
		}
		delete[] ctx->output_plane_fd;
	}
	delete ctx;
	return NULL;
}
