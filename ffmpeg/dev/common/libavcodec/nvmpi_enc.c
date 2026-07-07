/*
 * nvmpi_enc.c — FFmpeg encoder wrapper for libnvmpi (layer 2 of
 * jetson-ffmpeg, the FFmpeg integration layer): codec init/close,
 * AVOptions, and codec registration. See nvmpi_enc_ff_internal.h for the
 * shared nvmpiEncodeContext struct and companion nvmpi_enc_runtime.c.
 *
 * One source supports FFmpeg 6.0 .. 9.0+ via preprocessor guards:
 *  - LIBAVCODEC_VERSION_MAJOR >= 60: FFCodec registration +
 *    ff_get_encode_buffer replaces ff_alloc_packet2.
 *  - lavc >= 62.11 (FFmpeg 8.0): FF_PROFILE_* renamed to AV_PROFILE_*.
 *  - lavc >= 63 (FFmpeg 9.0): pix_fmts moved from AVCodec to FFCodec.
 * See https://github.com/gjrtimmer/jetson-ffmpeg/wiki/Development-Guide "Wrapper code paths by FFmpeg version".
 */
/* Runtime-loaded via dlopen -- no link-time dependency on libnvmpi.so.
 * dynlink_nvmpi.h is self-contained (duplicates nvmpi.h types) and
 * provides macro redirects so all nvmpi_* calls dispatch through
 * function pointers resolved at codec init time. */
#include "dynlink_nvmpi.h"
#include "avcodec.h"
#include "libavutil/avutil.h"
#include "libavutil/common.h"
#include "libavutil/imgutils.h"
#include "libavutil/log.h"
#include "libavutil/opt.h"
#include "libavutil/mem.h"

/* Compatibility with FFmpeg 8.0+ (libavcodec >= 62.11): FF_PROFILE_*
 * names were deprecated in favour of AV_PROFILE_*.  Detect this by
 * checking whether FF_PROFILE_UNKNOWN is already defined — this avoids
 * version-arithmetic bugs when MAJOR bumps and MINOR resets to 0. */
#ifndef FF_PROFILE_UNKNOWN
#define FF_PROFILE_H264_INTRA AV_PROFILE_H264_INTRA
#define FF_PROFILE_UNKNOWN AV_PROFILE_UNKNOWN
#define FF_PROFILE_H264_HIGH AV_PROFILE_H264_HIGH
#define FF_PROFILE_H264_BASELINE AV_PROFILE_H264_BASELINE
#define FF_PROFILE_H264_MAIN AV_PROFILE_H264_MAIN
#endif

#include "encode.h"
#include "codec_internal.h"
#include "nvmpi_enc_ff_internal.h"

/*
 * libavcodec 63 (FFmpeg 9.0+): pix_fmts moved from the public AVCodec (.p)
 * to a direct field on FFCodec in codec_internal.h.
 */
/* libavcodec 63 (FFmpeg 9.0+): pix_fmts moved from AVCodec (.p) to FFCodec.
 * DRM_PRIME enables zero-copy encode from DMA-BUF fd input frames (#62). */
#if LIBAVCODEC_VERSION_MAJOR >= 63
#define NVMPI_ENC_PIXFMTS \
	.pix_fmts = (const enum AVPixelFormat[]){AV_PIX_FMT_YUV420P, AV_PIX_FMT_NV12, AV_PIX_FMT_DRM_PRIME, AV_PIX_FMT_NONE}
#else
#define NVMPI_ENC_PIXFMTS \
	.p.pix_fmts = (const enum AVPixelFormat[]){AV_PIX_FMT_YUV420P, AV_PIX_FMT_NV12, AV_PIX_FMT_DRM_PRIME, AV_PIX_FMT_NONE}
#endif

//valid range / default for the wait_timeout AVOption: how long (ms)
//nvmpi_encoder_get_packet() blocks in low-delay mode. Matches decoder.
#define OPT_wait_timeout_AUTO 0
#define OPT_wait_timeout_MAX  5000

//valid range / default for the packet_pool_size AVOption: how many encoded
//packets may pile up before the libnvmpi DQ thread starts dropping output.
#define OPT_packet_pool_size_MIN 1
#define OPT_packet_pool_size_MAX 32
#define OPT_packet_pool_size_DEFAULT 10

//alloc nvPacket and AVPacket buffer;
//Creates one pool packet: an nvPacket whose payload points into a real
//AVPacket data buffer (kept in privData). This is what makes the later
//handoff to FFmpeg zero-copy. Returns NULL on allocation failure.
//ff_get_encode_buffer() is how encoders obtain packet buffers (libavcodec 60+).
nvPacket* nvmpienc_nvPacket_alloc(AVCodecContext *avctx, int bufSize)
{
	AVPacket* pkt;
	nvPacket* nPkt;
	int res;

	/* Allocate AVPacket first — ff_get_encode_buffer requires a valid
	 * AVPacket, so this must succeed before anything else. */
	pkt = av_packet_alloc();
	if (!pkt)
		return NULL;

	nPkt = (nvPacket*)malloc(sizeof(nvPacket));
	if (!nPkt) {
		av_packet_free(&pkt);
		return NULL;
	}
	memset(nPkt, 0, sizeof(nvPacket));

	if ((res = ff_get_encode_buffer(avctx, pkt, bufSize, 0)))
	{
		av_packet_free(&pkt);
		free(nPkt);
		return NULL;
	}
	nPkt->privData = pkt;
	nPkt->payload = pkt->data;
	return nPkt;
}
//(nvmpienc_nvPacket_free lives in nvmpi_enc_runtime.c, next to its main caller.)

//Re-arm a pool packet after its previous buffer was moved into the user's
//AVPacket: acquire a fresh encode buffer for the (now empty) AVPacket and
//clear the nvPacket bookkeeping. Returns 0 or -1 on allocation failure.
int nvmpienc_nvPacket_reset(nvPacket* nPkt, AVCodecContext *avctx, int bufSize)
{
	AVPacket* pkt = nPkt->privData;
	int res;
	if((res = ff_get_encode_buffer(avctx, pkt, bufSize, 0)))
	{
		return -1;
	}
	nPkt->payload = pkt->data;
	nPkt->payload_size = 0;
	nPkt->flags = 0;
	nPkt->pts = 0;
	return 0;
}

//must call after nvmpi_create_encoder() to preallocate buffers
//Seeds libnvmpi's empty-packet pool with pktNum packets of
//NVMPI_ENC_CHUNK_SIZE each; the encoder's DQ thread fills them as encoded
//frames appear.
int nvmpienc_initPktPool(AVCodecContext *avctx, int pktNum)
{
	nvmpiEncodeContext * nvmpi_context = avctx->priv_data;
	for(int i=0;i<pktNum;i++)
	{
		nvPacket* nPkt = nvmpienc_nvPacket_alloc(avctx, NVMPI_ENC_CHUNK_SIZE);
		if (!nPkt)
		{
			/* Allocation failed mid-pool: drain and free all previously
			 * queued packets to avoid a partially populated pool. */
			av_log(avctx, AV_LOG_ERROR,
			       "nvmpi: packet pool allocation failed at %d/%d\n", i, pktNum);
			nvmpienc_deinitPktPool(avctx);
			return AVERROR(ENOMEM);
		}
		nvmpi_encoder_qEmptyPacket(nvmpi_context->ctx, nPkt);
	}
	return 0;
}
//(nvmpienc_deinitPktPool lives in nvmpi_enc_runtime.c with its drain loops.)

//AVCodec/FFCodec .init callback: translate AVCodecContext settings and
//AVOptions into an nvEncParam, optionally pre-generate global extradata
//(SPS/PPS), then create the real libnvmpi encoder and seed its packet
//pool. The numeric profile/level/rc/preset conventions used here are
//decoded on the libnvmpi side (src/nvmpi_enc.cpp).
static av_cold int nvmpi_encode_init(AVCodecContext *avctx)
{
	nvmpiEncodeContext * nvmpi_context = avctx->priv_data;

	nvEncParam param={0};

	/* Load libnvmpi.so via dlopen on first use.  If the library is not
	 * installed, report a clear error and let FFmpeg fall back to other
	 * encoders or software encode. */
	if (nvmpi_dynlink_load() < 0) {
		av_log(avctx, AV_LOG_ERROR,
		       "Failed to load libnvmpi.so: %s\n"
		       "Install libnvmpi for hardware-accelerated encoding on Jetson.\n",
		       dlerror());
		return AVERROR_EXTERNAL;
	}

	param.width=avctx->width;
	param.height=avctx->height;
	param.bitrate=avctx->bit_rate;
	param.vbv_buffer_size = avctx->rc_buffer_size;
	//TODO use rc_initial_buffer_occupancy or ignore?
	param.mode_vbr=0;
	param.idr_interval=60;
	param.iframe_interval=30;
	param.peak_bitrate=avctx->rc_max_rate;
	param.fps_n=avctx->framerate.num;
	param.fps_d=avctx->framerate.den;
	//strip the INTRA flag bit so only the base profile id is passed down
	param.profile=nvmpi_context->profile& ~FF_PROFILE_H264_INTRA;
	param.level=nvmpi_context->level;
	param.capture_num=nvmpi_context->num_capture_buffers;
	//param.packet_pool_size=nvmpi_context->packet_pool_size;
	param.hw_preset_type=nvmpi_context->preset;
	param.max_perf=nvmpi_context->max_perf;
	param.poc_type=nvmpi_context->poc_type;
	//with GLOBAL_HEADER the SPS/PPS live in extradata (generated below)
	//instead of being repeated in-band at every IDR
	param.insert_spspps_idr=(avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER)?0:1;
	param.insert_vui=nvmpi_context->insert_vui;
	param.insert_aud=nvmpi_context->insert_aud;
	param.enable_cabac=nvmpi_context->cabac;
	param.enableLossless=nvmpi_context->lossless;
	param.wait_timeout=nvmpi_context->wait_timeout;
	param.nonblocking=nvmpi_context->nonblocking;

	//Raw input layout: NV12 routes to libnvmpi's V4L2 NV12M path, otherwise
	//planar YUV420. Full-range (YUVJ420P / MJPEG) input is intentionally NOT
	//advertised in pix_fmts: the Jetson V4L2 encoder exposes no API to set
	//the VUI video_full_range_flag, so it always emits limited-range. Letting
	//FFmpeg's swscale convert full->limited range before the encoder yields
	//correct levels; passing full-range pixels through under a limited-range
	//flag would not (the latent bug in the bradcagle/xsacha forks).
	param.pixFormat = (avctx->pix_fmt == AV_PIX_FMT_NV12) ? NV_PIX_NV12 : NV_PIX_YUV420;

	/* DRM_PRIME input: the DMA-BUF fd arrives via AVDRMFrameDescriptor.
	 * The underlying pixel format is NV12 (Jetson's native hw layout).
	 *
	 * The encoder stays in MMAP mode (default) — NOT DMABUF mode.
	 * DRM_PRIME frames are mmap'd for CPU read and copied into the
	 * encoder's MMAP buffers via the regular put_frame path.  This
	 * avoids two Tegra V4L2 driver bugs:
	 *   1. bBlitMode throughput deadlock at 720p (NvBufSurface format
	 *      vs V4L2 format mismatch triggers per-frame VIC conversion
	 *      that can't keep up at passthrough speed)
	 *   2. YUV420M + V4L2_MEMORY_DMABUF segfault at >1024x576
	 *
	 * pixFormat = NV_PIX_NV12 so MMAP buffers are NV12M (2 planes),
	 * matching the NV12 data from the decoder/VIC filter. */
	if (avctx->pix_fmt == AV_PIX_FMT_DRM_PRIME) {
		param.pixFormat = NV_PIX_NV12;
		nvmpi_context->dmabuf_input = 1;
	}

	nvmpi_context->frame = av_frame_alloc();
	if (!nvmpi_context->frame) return AVERROR(ENOMEM);

	if(nvmpi_context->rc==1){
		param.mode_vbr=1;
	}

	if(avctx->qmin >= 0 && avctx->qmax >= 0){
		param.qmin=avctx->qmin;
		param.qmax=avctx->qmax;
	}

	if (avctx->refs >= 0){
		param.refs=avctx->refs;

	}

	if(avctx->max_b_frames > 0 && avctx->max_b_frames < 3){
		param.max_b_frames=avctx->max_b_frames;
	}

	if(avctx->gop_size>0){
		param.idr_interval=param.iframe_interval=avctx->gop_size;

	}

	//TODO should replace it. must gen extradata directly without calling for encoder
	//Extradata generation for GLOBAL_HEADER: spin up a THROWAWAY encoder
	//instance, encode one blank frame, and scan the resulting bitstream
	//for the first IDR NAL — everything before it (SPS/PPS, VPS for HEVC)
	//becomes avctx->extradata. The temporary encoder is then drained and
	//destroyed; the real encoder is created afterwards (helper call below
	//is defined in nvmpi_enc_runtime.c).
	if ((avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER) && (avctx->codec->id == AV_CODEC_ID_H264 || avctx->codec->id == AV_CODEC_ID_H265))
	{
		int hdr_ret = nvmpi_encode_gen_global_header_extradata(avctx, &param);
		if (hdr_ret < 0)
			return hdr_ret;
	}

	//create the real encoder used for the rest of the session
	if(avctx->codec->id == AV_CODEC_ID_H264)
	{
		param.codingType = NV_VIDEO_CodingH264;
		nvmpi_context->ctx=nvmpi_create_encoder(&param);
	}
	else if(avctx->codec->id == AV_CODEC_ID_HEVC)
	{
		param.codingType = NV_VIDEO_CodingHEVC;
		nvmpi_context->ctx=nvmpi_create_encoder(&param);
	}
	//else TODO

	if(!nvmpi_context->ctx)
	{
		av_log(avctx, AV_LOG_ERROR, "nvmpi: encoder creation failed\n");
		/* Free extradata allocated by the GLOBAL_HEADER block above —
		 * FFmpeg does not call .close() after a failed .init() (unless
		 * FF_CODEC_CAP_INIT_CLEANUP is set), so this is the only path. */
		av_freep(&avctx->extradata);
		avctx->extradata_size = 0;
		av_frame_free(&nvmpi_context->frame);
		/* AVERROR_EXTERNAL: device-level failure (EBUSY, ENODEV, etc.),
		 * not a memory allocation error. See #37. */
		return AVERROR_EXTERNAL;
	}
	{
		int pool_ret = nvmpienc_initPktPool(avctx, nvmpi_context->packet_pool_size);
		if (pool_ret < 0) {
			av_log(avctx, AV_LOG_ERROR, "nvmpi: packet pool init failed\n");
			nvmpi_encoder_close(nvmpi_context->ctx);
			nvmpi_context->ctx = NULL;
			av_freep(&avctx->extradata);
			avctx->extradata_size = 0;
			av_frame_free(&nvmpi_context->frame);
			return pool_ret;
		}
	}

	/* Track initial bitrate for runtime change detection in send_frame. */
	nvmpi_context->last_bitrate = avctx->bit_rate;

	return 0;
}

//AVCodec/FFCodec .close callback: make sure EOS was sent, drain remaining
//packets (recycling pool buffers while doing so), then free the pool and
//close the libnvmpi encoder. Order matters — the pool must be drained
//before nvmpi_encoder_close(), which does not free packet memory.
static av_cold int nvmpi_encode_close(AVCodecContext *avctx)
{
	nvmpiEncodeContext *nvmpi_context = avctx->priv_data;

	/* FF_CODEC_CAP_INIT_CLEANUP: FFmpeg calls .close() even when .init()
	 * failed partway through. Guard against NULL ctx — the encoder may
	 * never have been created. */
	if (!nvmpi_context->ctx) {
		av_frame_free(&nvmpi_context->frame);
		return 0;
	}

	//drain encoder
	{
		int ret;
		nvPacket *nPkt;
		if(!nvmpi_context->encoder_flushing)
		{
			nvmpi_context->encoder_flushing = 1;
			/* MMAP mode for both DRM_PRIME and software paths. */
			nvmpi_encoder_put_frame(nvmpi_context->ctx, NULL);
		}

		while(1)
		{
			ret=nvmpi_encoder_get_packet(nvmpi_context->ctx, &nPkt, false);
			if(ret < 0)
			{
				if(ret == -2) break; //got eos
				continue;
			}
			nvmpienc_nvPacket_free(nPkt);
			nPkt = nvmpienc_nvPacket_alloc(avctx, NVMPI_ENC_CHUNK_SIZE);
			if (!nPkt) break; /* OOM during close — stop draining */
			nvmpi_encoder_qEmptyPacket(nvmpi_context->ctx, nPkt);
		}
	}

	nvmpienc_deinitPktPool(avctx);
	nvmpi_encoder_close(nvmpi_context->ctx);
	av_frame_free(&nvmpi_context->frame);

	return 0;
}

//Default values for GENERIC AVCodecContext options (not the private
//AVOptions below): 2M bitrate, GOP 50, no B-frames, qmin/qmax unset, etc.
//Only the struct's name changed with the FFCodec split in libavcodec 60;
//the entries are identical.
static const FFCodecDefault defaults[] = {
	{ "b", "2M" },
	{ "qmin", "-1" },
	{ "qmax", "-1" },
	{ "qdiff", "-1" },
	{ "qblur", "-1" },
	{ "qcomp", "-1" },
	{ "g", "50" },
	{ "bf", "0" },
	{ "refs", "0" },
	{ NULL },
};


//AVOption table: private "-x264-style" options resolved into
//nvmpiEncodeContext fields before .init. AV_OPT_TYPE_CONST entries are
//named aliases for values of the preceding INT option (grouped by the
//trailing unit string, e.g. "profile"). The numeric values intentionally
//match what src/nvmpi_enc.cpp's switch statements expect: FFmpeg profile
//ids, level_idc*10, rc 0/1 = CBR/VBR, preset 1..4.
#define OFFSET(x) offsetof(nvmpiEncodeContext, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM

static const AVOption options[] = {
	{ "num_capture_buffers", "Number of buffers in the capture context", OFFSET(num_capture_buffers), AV_OPT_TYPE_INT, {.i64 = 10 }, 1, 32, VE, "num_capture_buffers" },
	{ "packet_pool_size", "Number of packets that could be buffered in the encoder before user must read it with avcodec_receive_packet()", OFFSET(packet_pool_size), AV_OPT_TYPE_INT, {.i64 = OPT_packet_pool_size_DEFAULT }, OPT_packet_pool_size_MIN, OPT_packet_pool_size_MAX, VE, "packet_pool_size" },
	/// Profile,

	{ "profile",  "Set the encoding profile", OFFSET(profile), AV_OPT_TYPE_INT,   { .i64 = FF_PROFILE_UNKNOWN },       FF_PROFILE_UNKNOWN, FF_PROFILE_H264_HIGH, VE, "profile" },
	{ "baseline", "",                         0,               AV_OPT_TYPE_CONST, { .i64 = FF_PROFILE_H264_BASELINE }, 0, 0, VE, "profile" },
	{ "main",     "",                         0,               AV_OPT_TYPE_CONST, { .i64 = FF_PROFILE_H264_MAIN },     0, 0, VE, "profile" },
	{ "high",     "",                         0,               AV_OPT_TYPE_CONST, { .i64 = FF_PROFILE_H264_HIGH },     0, 0, VE, "profile" },

	/// Profile Level (shared by H.264 and HEVC; level_idc convention: 51 = 5.1)
	{ "level",          "Encoding level (e.g. 4.1, 5.1)",  OFFSET(level),  AV_OPT_TYPE_INT,   { .i64 = 0  }, 0, 62, VE, "level" },
	{ "auto",           "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 0  }, 0, 0,  VE, "level" },
	{ "1.0",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 10 }, 0, 0,  VE, "level" },
	{ "1.1",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 11 }, 0, 0,  VE, "level" },
	{ "1.2",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 12 }, 0, 0,  VE, "level" },
	{ "1.3",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 13 }, 0, 0,  VE, "level" },
	{ "2.0",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 20 }, 0, 0,  VE, "level" },
	{ "2.1",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 21 }, 0, 0,  VE, "level" },
	{ "2.2",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 22 }, 0, 0,  VE, "level" },
	{ "3.0",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 30 }, 0, 0,  VE, "level" },
	{ "3.1",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 31 }, 0, 0,  VE, "level" },
	{ "3.2",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 32 }, 0, 0,  VE, "level" },
	{ "4.0",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 40 }, 0, 0,  VE, "level" },
	{ "4.1",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 41 }, 0, 0,  VE, "level" },
	{ "4.2",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 42 }, 0, 0,  VE, "level" },
	{ "5.0",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 50 }, 0, 0,  VE, "level" },
	{ "5.1",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 51 }, 0, 0,  VE, "level" },
	{ "5.2",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 52 }, 0, 0,  VE, "level" },
	{ "6.0",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 60 }, 0, 0,  VE, "level" },
	{ "6.1",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 61 }, 0, 0,  VE, "level" },
	{ "6.2",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 62 }, 0, 0,  VE, "level" },

	{ "rc",           "Override the preset rate-control",   OFFSET(rc),           AV_OPT_TYPE_INT,   { .i64 = -1 },                                  -1, INT_MAX, VE, "rc" },
	{ "cbr",          "Constant bitrate mode",              0,                    AV_OPT_TYPE_CONST, { .i64 = 0 },                       0, 0, VE, "rc" },
	{ "vbr",          "Variable bitrate mode",              0,                    AV_OPT_TYPE_CONST, { .i64 = 1 },                       0, 0, VE, "rc" },

	{ "preset",          "Set the encoding preset",            OFFSET(preset),       AV_OPT_TYPE_INT,   { .i64 = 3 }, 1, 4, VE, "preset" },
	{ "default",         "",                                   0,                    AV_OPT_TYPE_CONST, { .i64 = 3 }, 0, 0, VE, "preset" },
	{ "slow",            "",                        0,                    AV_OPT_TYPE_CONST, { .i64 = 4 },            0, 0, VE, "preset" },
	{ "medium",          "",                        0,                    AV_OPT_TYPE_CONST, { .i64 = 3 },            0, 0, VE, "preset" },
	{ "fast",            "",                        0,                    AV_OPT_TYPE_CONST, { .i64 = 2 },            0, 0, VE, "preset" },
	{ "ultrafast",       "",                        0,                    AV_OPT_TYPE_CONST, { .i64 = 1 },            0, 0, VE, "preset" },

	{ "max_perf", "Enable max performance mode (lifts NVENC clock governor)", OFFSET(max_perf), AV_OPT_TYPE_BOOL, {.i64 = 1 }, 0, 1, VE, "max_perf" },
	{ "poc_type", "H.264 picture order count type (0=default, 2=decode-order for low-latency)", OFFSET(poc_type), AV_OPT_TYPE_INT, {.i64 = 0 }, 0, 2, VE, "poc_type" },
	{ "insert_vui", "Embed VUI timing_info (fps) in the bitstream so players/muxers report the frame rate", OFFSET(insert_vui), AV_OPT_TYPE_BOOL, {.i64 = 1 }, 0, 1, VE, "insert_vui" },
	{ "aud", "Insert Access Unit Delimiter NALs (required by some TS/HLS workflows)", OFFSET(insert_aud), AV_OPT_TYPE_BOOL, {.i64 = 0 }, 0, 1, VE, "aud" },
	{ "cabac", "Enable CABAC entropy coding (H.264 only; ~10-15%% better compression than CAVLC)", OFFSET(cabac), AV_OPT_TYPE_BOOL, {.i64 = 0 }, 0, 1, VE, "cabac" },
	{ "lossless", "Enable lossless encoding (H.264 only, constant QP 0 + High 4:4:4 Predictive profile)", OFFSET(lossless), AV_OPT_TYPE_BOOL, {.i64 = 0 }, 0, 1, VE, "lossless" },
	{ "wait_timeout", "Blocking wait timeout in milliseconds for low-delay mode (0 = default 500ms)", OFFSET(wait_timeout), AV_OPT_TYPE_INT, {.i64 = OPT_wait_timeout_AUTO }, 0, OPT_wait_timeout_MAX, VE, "wait_timeout" },
	{ "nonblocking", "Non-blocking encode mode: send_frame returns EAGAIN instead of blocking when no output-plane buffer is available", OFFSET(nonblocking), AV_OPT_TYPE_BOOL, {.i64 = 0 }, 0, 1, VE, "nonblocking" },
	{ NULL }
};


//Per-codec AVClass binding the option table above to each encoder
//instance (referenced via priv_class in the registration struct).
#define NVMPI_ENC_CLASS(NAME) \
	static const AVClass nvmpi_ ## NAME ## _enc_class = { \
		.class_name = #NAME "_nvmpi_encoder", \
		.item_name  = av_default_item_name, \
		.option     = options, \
		.version    = LIBAVUTIL_VERSION_INT, \
	};


//Codec registration, stamped out per codec by NVMPI_ENC(). Covers paths C/D:
//  - lavc >= 60 (FFmpeg 6.0–7.x, path C): FFCodec struct, pull-based receive_packet.
//  - lavc >= 62.11 (FFmpeg 8.0, path D): same, plus AV_PROFILE_* aliases above.
//Matching allcodecs.c extern is "extern const FFCodec" (receive_packet/flush
//callbacks are implemented in nvmpi_enc_runtime.c).
#define NVMPI_ENC(NAME, LONGNAME, CODEC) \
	NVMPI_ENC_CLASS(NAME) \
	const FFCodec ff_ ## NAME ## _nvmpi_encoder = { \
		.p.name           = #NAME "_nvmpi" , \
		CODEC_LONG_NAME("nvmpi " LONGNAME " encoder wrapper"), \
		.p.type           = AVMEDIA_TYPE_VIDEO, \
		.p.id             = CODEC , \
		.priv_data_size = sizeof(nvmpiEncodeContext), \
		.p.priv_class     = &nvmpi_ ## NAME ##_enc_class, \
		.init           = nvmpi_encode_init, \
		FF_CODEC_RECEIVE_PACKET_CB(ff_nvmpi_receive_packet_async), \
		.close          = nvmpi_encode_close, \
		.flush          = nvmpi_flush_encoder, \
		/* libavcodec 63 (FFmpeg 9.0+): pix_fmts moved from AVCodec to FFCodec */ \
		NVMPI_ENC_PIXFMTS, \
		.p.capabilities   = AV_CODEC_CAP_HARDWARE | AV_CODEC_CAP_DELAY, \
		.caps_internal  = FF_CODEC_CAP_INIT_CLEANUP, \
		.defaults       = defaults,\
		.p.wrapper_name   = "nvmpi", \
	};

//Instantiate the two nvmpi encoders. Each expansion must have a matching
//extern in allcodecs.c and CONFIG_*_NVMPI_ENCODER Makefile/configure
//entries (added by ffpatch.sh / the version overlays).
NVMPI_ENC(h264, "H.264", AV_CODEC_ID_H264);
NVMPI_ENC(hevc, "HEVC", AV_CODEC_ID_HEVC);
