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
	ctx->blocking_mode = true; //TODO non-blocking mode support
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
					delete ctx;
					return NULL;
				}
				if (saved_errno == EACCES) {
					NVMPI_LOG(NVMPI_LOG_ERROR,
						  "Encoder device %s: permission denied", dev_path);
					delete ctx;
					return NULL;
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
			delete ctx;
			return NULL;
		}

		if (ctx->blocking_mode)
			ctx->enc.reset(NvVideoEncoder::createVideoEncoder("enc0"));
		else
			ctx->enc.reset(NvVideoEncoder::createVideoEncoder("enc0", O_NONBLOCK));
		if (ctx->enc) break;
		if (attempt < 2) {
			NVMPI_LOG(NVMPI_LOG_WARN, "Encoder factory returned NULL, retrying (%d/3)...", attempt + 1);
			usleep(100000);
		}
	}
	if (!ctx->enc)
	{
		NVMPI_LOG(NVMPI_LOG_ERROR, "Could not create encoder after 3 attempts");
		delete ctx;
		return NULL;
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

	if(ctx->blocking_mode)
	{
		ctx->enc->capture_plane.setDQThreadCallback(encoder_capture_plane_dq_callback);
		ret = ctx->enc->capture_plane.startDQThread(ctx);
		if (ret < 0) {
			NVMPI_LOG(NVMPI_LOG_ERROR, "Could not start DQ thread");
			goto cleanup;
		}
		ctx->dq_thread_started = true;
	}
    else
    {
		/*
        sem_init(&ctx->pollthread_sema, 0, 0);
        sem_init(&ctx->encoderthread_sema, 0, 0);
        // Set encoder poll thread for non-blocking io mode
        pthread_create(&ctx->enc_pollthread, NULL, encoder_pollthread_fcn, ctx);
        pthread_setname_np(ctx->enc_pollthread, "EncPollThread");
        NVMPI_LOG(NVMPI_LOG_DEBUG, "created poll thread and encoder thread");
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
		/*TODO move it to another thread or make this call non-blocking.
		 * DQBuffer could take considerable time. e.g. when encoder performance is 20fps and all output_plane is busy
		 * it could take up to 1000/20=50ms... latency
		 */
		ret = ctx->enc->output_plane.dqBuffer(v4l2_buf, &nvBuffer, NULL, -1);
		if (ret < 0)
		{
			NVMPI_LOG(NVMPI_LOG_ERROR, "Error DQing buffer at output plane");
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
	 * First N frames use sequential indices; after that, block in
	 * dqBuffer until the encoder frees a slot. */
	if (ctx->index < ctx->enc->output_plane.getNumBuffers())
	{
		v4l2_buf.index = ctx->index;
		ctx->index++;
	}
	else
	{
		NvBuffer *nvBuffer;
		ret = ctx->enc->output_plane.dqBuffer(v4l2_buf, &nvBuffer, NULL, -1);
		if (ret < 0)
		{
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
					  "Error mapping internal buffer to slot %d", slot);
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
//When wait=false: non-blocking, returns -1 immediately if no packet ready
//(maps to EAGAIN). When wait=true: blocks up to wait_timeout_ms using the
//pool's CV-based tiered wait. While flushing: poll every 1ms until either
//a packet arrives (0) or the DQ callback saw the EOS buffer (-2).
//On success the caller holds the packet until re-queueing it via
//nvmpi_encoder_qEmptyPacket().
int nvmpi_encoder_get_packet(nvmpictx* ctx, nvPacket** packet, bool wait)
{
	nvPacket* pkt;

	if (wait)
		pkt = ctx->pktPool->dqFilledBuf(
			std::chrono::milliseconds(ctx->wait_timeout_ms));
	else
		pkt = ctx->pktPool->dqFilledBuf();

	if(!pkt)
	{
		if(!ctx->flushing.load(std::memory_order_acquire)) return -1;
		bool spin = true;
		while(spin)
		{
			pkt = ctx->pktPool->dqFilledBuf();
			if(pkt || ctx->capPlaneGotEOS.load(std::memory_order_acquire)) spin = false;
			else std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		if(!pkt) return -2; //if got eos
	}

	*packet = pkt;
	return 0;
}

//Public API: force the next encoded frame to be an IDR (keyframe on demand).
//Calls NvVideoEncoder::forceIDR() which sets the V4L2 control
//V4L2_CID_MPEG_MFC51_VIDEO_FORCE_FRAME_TYPE. Safe to call from the user
//thread while the DQ thread is running — the V4L2 ioctl is thread-safe.
//Returns 0 on success, -1 on ioctl failure.
int nvmpi_encoder_force_idr(nvmpictx* ctx)
{
	int ret = ctx->enc->forceIDR();
	if (ret < 0) {
		NVMPI_LOG(NVMPI_LOG_ERROR, "Could not force IDR frame");
		return -1;
	}
	return 0;
}

//Public API: change the encoder target bitrate mid-stream.
//Calls NvVideoEncoder::setBitrate() which updates V4L2_CID_MPEG_VIDEO_BITRATE.
//The new bitrate takes effect from the next encoded frame. Also updates
//ctx->bitrate so the internal state stays consistent. For VBR mode the caller
//should also adjust peak_bitrate externally if needed.
//Returns 0 on success, -1 on ioctl failure.
int nvmpi_encoder_set_bitrate(nvmpictx* ctx, unsigned int bitrate)
{
	int ret = ctx->enc->setBitrate(bitrate);
	if (ret < 0) {
		NVMPI_LOG(NVMPI_LOG_ERROR, "Could not set bitrate to %u", bitrate);
		return -1;
	}
	/* Keep internal state in sync; freed in nvmpi_encoder_close(). */
	ctx->bitrate = bitrate;
	return 0;
}

//Public API: stop the encoder and free the context.
//Mid-stream flush: reset the V4L2 encoder pipeline so encoding can continue
//after a seek or stream restart without closing/reopening the codec context.
//
//Sequence (mirrors the decoder flush in nvmpi_dec_api.cpp):
//  1. Stop the DQ thread — prevents callbacks during STREAMOFF.
//  2. STREAMOFF both planes — tears down V4L2 queues, returns all buffers.
//  3. Drain the packet pool — discard any pending encoded packets by moving
//     them from the filled queue back to the empty queue.
//  4. Reset flushing/EOS atomics — re-enable put_frame/get_packet.
//  5. STREAMON both planes — restart the V4L2 pipeline.
//  6. Re-queue capture plane buffers — the encoder needs empty buffers to
//     write encoded output into.
//  7. Restart the DQ thread — resume capture-plane processing.
//  8. Reset the output-plane buffer index — next put_frame starts from 0.
//
//The caller (FFmpeg wrapper) is responsible for resetting its own
//encoder_flushing flag and forcing the next frame to IDR if needed.
int nvmpi_encoder_flush(nvmpictx* ctx)
{
	int ret;

	/* 1. Stop DQ thread — must complete before STREAMOFF to avoid
	 *    callbacks on torn-down queues. */
	if (ctx->blocking_mode && ctx->dq_thread_started) {
		ctx->enc->capture_plane.stopDQThread();
		ctx->enc->capture_plane.waitForDQThread(1000);
		ctx->dq_thread_started = false;
	}

	/* 2. STREAMOFF both planes — returns all queued buffers. */
	ctx->enc->capture_plane.setStreamStatus(false);
	ctx->enc->output_plane.setStreamStatus(false);

	/* 3. Drain packet pool: move filled packets back to empty queue.
	 *    The pool must be reset() first to clear any shutdown state
	 *    from a prior EOS drain, then drained. */
	ctx->pktPool->reset();
	{
		nvPacket* pkt;
		while ((pkt = ctx->pktPool->dqFilledBuf()))
			ctx->pktPool->qEmptyBuf(pkt);
	}

	/* 4. Reset EOS/flushing atomics so put_frame/get_packet work again. */
	ctx->flushing.store(false, std::memory_order_release);
	ctx->capPlaneGotEOS.store(false, std::memory_order_release);

	/* 5. STREAMON both planes — restart the V4L2 pipeline. */
	ret = ctx->enc->output_plane.setStreamStatus(true);
	if (ret < 0) {
		NVMPI_LOG(NVMPI_LOG_ERROR, "Error in output plane streamon after flush");
		return ret;
	}

	ret = ctx->enc->capture_plane.setStreamStatus(true);
	if (ret < 0) {
		NVMPI_LOG(NVMPI_LOG_ERROR, "Error in capture plane streamon after flush");
		return ret;
	}

	/* 6. Re-queue all capture plane buffers so the encoder has
	 *    somewhere to write encoded output. */
	for (uint32_t i = 0; i < ctx->enc->capture_plane.getNumBuffers(); i++) {
		struct v4l2_buffer v4l2_buf;
		struct v4l2_plane planes[MAX_PLANES];
		memset(&v4l2_buf, 0, sizeof(v4l2_buf));
		memset(planes, 0, MAX_PLANES * sizeof(struct v4l2_plane));

		v4l2_buf.index = i;
		v4l2_buf.m.planes = planes;

		ret = ctx->enc->capture_plane.qBuffer(v4l2_buf, NULL);
		if (ret < 0) {
			NVMPI_LOG(NVMPI_LOG_ERROR,
				  "Error while queueing buffer at capture plane after flush (buf %u)", i);
			return ret;
		}
	}

	/* 7. Restart DQ thread — resume capture-plane processing. */
	if (ctx->blocking_mode) {
		ctx->enc->capture_plane.setDQThreadCallback(encoder_capture_plane_dq_callback);
		ret = ctx->enc->capture_plane.startDQThread(ctx);
		if (ret < 0) {
			NVMPI_LOG(NVMPI_LOG_ERROR, "Could not restart DQ thread after flush");
			return ret;
		}
		ctx->dq_thread_started = true;
	}

	/* 8. Reset output-plane buffer index — next put_frame starts
	 *    from buffer 0 (first N frames use getNthBuffer directly). */
	ctx->index = 0;

	return 0;
}

//Stops/joins the capture DQ thread, releases self-allocated OUTPUT-plane
//dmabufs (DMA mode only), then destroys the device and the (by now empty)
//packet pool. Packets still inside the pool are NOT freed here — the
//FFmpeg wrapper drains and frees them first (nvmpienc_deinitPktPool).
int nvmpi_encoder_close(nvmpictx* ctx)
{
	/* Shutdown the packet pool to unblock any thread waiting in
	 * dqFilledBuf(timeout) — prevents hangs on early termination
	 * (e.g. ffmpeg -t) when blocking wait is active. */
	if (ctx->pktPool)
		ctx->pktPool->shutdown();

	/* Guard: only stop/join the DQ thread if it was actually started.
	 * A half-initialized encoder (create failed after STREAMON but before
	 * startDQThread) or a non-blocking-mode encoder never starts the DQ
	 * thread. Calling stopDQThread/waitForDQThread on an un-started
	 * thread invokes pthread_join on an uninitialized handle → UB/crash. */
	if(ctx->blocking_mode && ctx->dq_thread_started)
	{
		ctx->enc->capture_plane.stopDQThread();
		ctx->enc->capture_plane.waitForDQThread(1000);
	}
	else
	{
		//sem_destroy(&ctx.pollthread_sema);
		//sem_destroy(&ctx.encoderthread_sema);
	}

	/* Cleanup internal DMABUF surfaces. Applies to both compile-time
	 * DMA mode (OUTPLANE_MEMTYPE_DMA) and runtime external DMABUF mode
	 * (dmabuf_external). Check fd validity (-1 = unallocated slot). */
	if (ctx->enc && ctx->output_plane_fd) {
		for (uint32_t i = 0; i < ctx->enc->output_plane.getNumBuffers(); i++) {
			if (ctx->output_plane_fd[i] >= 0) {
				ctx->enc->output_plane.unmapOutputBuffers(i,
					ctx->output_plane_fd[i]);
				NvBufferDestroy(ctx->output_plane_fd[i]);
				ctx->output_plane_fd[i] = -1;
			}
		}
	}
	delete[] ctx->output_plane_fd;
	ctx->output_plane_fd = nullptr;

	/* unique_ptr: release the NvVideoEncoder after the DQ thread is
	 * joined and all V4L2 buffers are unmapped. */
	ctx->enc.reset();
	delete ctx->pktPool;
	delete ctx;
	return 0;
}
