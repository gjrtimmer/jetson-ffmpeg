/*
 * nvmpi_dec.c — FFmpeg decoder wrapper for libnvmpi (layer 2 of
 * jetson-ffmpeg, the FFmpeg integration layer).
 *
 * This file is NOT built in this repository: it is patched into a vanilla
 * FFmpeg source tree (libavcodec/) by scripts/ffpatch.sh or the generated
 * patches in ffmpeg/patches/, and compiled as part of FFmpeg itself. It
 * implements FFmpeg decoder codecs (h264_nvmpi, hevc_nvmpi, mpeg2_nvmpi,
 * mpeg4_nvmpi, vp8_nvmpi, vp9_nvmpi) by delegating all real work to the
 * libnvmpi C API (loaded at runtime via dynlink_nvmpi.h / dlopen).
 *
 * MJPEG uses a separate hardware engine (NvJPEGDecoder, not V4L2 M2M) with
 * its own init/decode/close/flush callbacks and no shared state with the
 * decoders below — see nvmpi_dec_mjpeg.c.
 *
 * One source file supports FFmpeg 6.0 through 9.0+: API differences are
 * handled with LIBAVCODEC_VERSION_MAJOR preprocessor guards (see
 * https://github.com/gjrtimmer/jetson-ffmpeg/wiki/Development-Guide "Wrapper code paths by FFmpeg version").
 */
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

/* Runtime-loaded via dlopen -- no link-time dependency on libnvmpi.so.
 * dynlink_nvmpi.h is self-contained (duplicates nvmpi.h types) and
 * provides macro redirects so all nvmpi_* calls dispatch through
 * function pointers resolved at codec init time. */
#include "dynlink_nvmpi.h"
#include "avcodec.h"
#include "decode.h"
#include "internal.h"
#include "libavutil/buffer.h"
#include "libavutil/common.h"
#include "libavutil/frame.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_drm.h"
#include "libavutil/imgutils.h"
#include "libavutil/mem.h"
#include "libavutil/log.h"
#include "libavutil/opt.h"

#include <unistd.h>

#include "codec_internal.h"
#include "version.h"

/* DRM_PRIME wrapping (descriptor, release, hw_frames_ctx) is shared with
 * nvmpi_dec_mjpeg.c; DRM_FORMAT_NV12 and NVMPI_DRM_MOD_ORIG_FD live there. */
#include "nvmpi_dec_drm.h"
#include "nvmpi_dec_fd2.h"

/*
 * libavcodec 63 (FFmpeg 9.0+): pix_fmts moved from the public AVCodec (.p)
 * to a direct field on FFCodec in codec_internal.h. Pre-63 uses .p.pix_fmts;
 * 63+ uses .pix_fmts (or the CODEC_PIXFMTS() macro).
 */
#if LIBAVCODEC_VERSION_MAJOR >= 63
/* libavcodec 63 (FFmpeg 9.0+): pix_fmts on FFCodec directly */
#define NVMPI_DEC_PIXFMTS \
	.pix_fmts = (const enum AVPixelFormat[]){AV_PIX_FMT_YUV420P, AV_PIX_FMT_NV12, AV_PIX_FMT_P010LE, AV_PIX_FMT_DRM_PRIME, AV_PIX_FMT_NONE}
#else
/* libavcodec 60-62 (FFmpeg 6.0-8.x): pix_fmts on public AVCodec sub-struct */
#define NVMPI_DEC_PIXFMTS \
	.p.pix_fmts = (const enum AVPixelFormat[]){AV_PIX_FMT_YUV420P, AV_PIX_FMT_NV12, AV_PIX_FMT_P010LE, AV_PIX_FMT_DRM_PRIME, AV_PIX_FMT_NONE}
#endif

//valid range / default for the frame_pool_size AVOption (mirrors the size
//of libnvmpi's decoded-frame pool, i.e. how many frames may buffer up
//before the user must call avcodec_receive_frame())
#define OPT_frame_pool_size_MIN 1
#define OPT_frame_pool_size_MAX 32
#define OPT_frame_pool_size_DEFAULT 5

//valid range for the chunk_size AVOption (bytes per compressed-input
//buffer inside libnvmpi). 0 = auto (libnvmpi default, 10 MiB). One input
//packet (access unit) must fit in one chunk; 4K high-bitrate I-frames
//can exceed the old 4 MB limit.
#define OPT_chunk_size_MIN 65536
#define OPT_chunk_size_MAX (64*1024*1024)
#define OPT_chunk_size_AUTO 0

#define OPT_wait_timeout_MIN 50
#define OPT_wait_timeout_MAX 5000
#define OPT_wait_timeout_AUTO 0

//Per-instance private context (priv_data of the AVCodecContext).
//AVClass must stay discoverable for the AVOption system; resize_expr and
//frame_pool_size are set via AVOptions before init.
typedef struct {
	char eos_reached;      //unused at present
	nvmpictx* ctx;         //libnvmpi decoder handle
	AVClass *av_class;
	AVFrame *bufFrame;     //pre-allocated frame the next decode copies into
	AVPacket *pkt;         //input holder for ff_decode_get_packet(); empty between calls
	int draining;          //ff_decode_get_packet() hit EOF; cleared by flush
	char *resize_expr;     //"-resize WxH" option (hw downscale in libnvmpi)
	int frame_pool_size;   //"-frame_pool_size N" option
	int chunk_size;        //"-chunk_size N" option (bytes; 0 = auto)
	int max_perf;          //lift NVDEC clock governor (default on)
	int disable_dpb;       //skip DPB reordering (low-latency, default off)
	int wait_timeout;      //blocking wait timeout in ms (0 = default 500ms)
	int output_format;     //requested output pixel format (AVPixelFormat; NONE = auto)
} nvmpiDecodeContext;

/* ------------------------------------------------------------------ */
/* DRM_PRIME (zero-copy) frame support                                 */
/* ------------------------------------------------------------------ */

//Translate FFmpeg's codec id into libnvmpi's coding type enum.
//Returns NV_VIDEO_CodingUnused for anything libnvmpi cannot decode.
static nvCodingType nvmpi_get_codingtype(AVCodecContext *avctx)
{
	switch (avctx->codec_id) {
		case AV_CODEC_ID_H264:          return NV_VIDEO_CodingH264;
		case AV_CODEC_ID_HEVC:          return NV_VIDEO_CodingHEVC;
		case AV_CODEC_ID_VP8:           return NV_VIDEO_CodingVP8;
		case AV_CODEC_ID_VP9:           return NV_VIDEO_CodingVP9;
		case AV_CODEC_ID_MPEG4:		return NV_VIDEO_CodingMPEG4;
		case AV_CODEC_ID_MPEG2VIDEO:    return NV_VIDEO_CodingMPEG2;
		case AV_CODEC_ID_MJPEG:         return NV_VIDEO_CodingMJPEG;
		//AV1 exists in every supported libavcodec (6.0-9.0), no version guard needed.
		//On JetPack 5 libnvmpi cannot create the decoder (no V4L2 AV1 format), so init fails cleanly.
		case AV_CODEC_ID_AV1:           return NV_VIDEO_CodingAV1;
		default:                        return NV_VIDEO_CodingUnused;
	}
};


//AVCodec/FFCodec .init callback: validate options/pix_fmt, build the
//nvDecParam from the AVOptions, pre-allocate the first output AVFrame
//(bufFrame) and create the libnvmpi decoder (which starts its internal
//capture thread). Returns 0 or an AVERROR code.
static int nvmpi_init_decoder(AVCodecContext *avctx)
{
	nvmpiDecodeContext *nvmpi_context = avctx->priv_data;
	nvDecParam param={0};
	enum AVPixelFormat choices[5];
	int nchoices;

	/* Load libnvmpi.so via dlopen on first use.  If the library is not
	 * installed, report a clear error and let FFmpeg fall back to other
	 * decoders or software decode. */
	if (nvmpi_dynlink_load() < 0) {
		av_log(avctx, AV_LOG_ERROR,
		       "Failed to load libnvmpi.so: %s\n"
		       "Install libnvmpi for hardware-accelerated decoding on Jetson.\n",
		       dlerror());
		return AVERROR_EXTERNAL;
	}

	/* Packet holder for the receive_frame callback. Allocated here; freed
	 * in nvmpi_close(), which FF_CODEC_CAP_INIT_CLEANUP also runs when a
	 * later init step fails. */
	nvmpi_context->pkt = av_packet_alloc();
	if (!nvmpi_context->pkt)
		return AVERROR(ENOMEM);
	nvmpi_context->draining = 0;

	param.codingType =nvmpi_get_codingtype(avctx);
	if (param.codingType == NV_VIDEO_CodingUnused)
	{
		av_log(avctx, AV_LOG_ERROR, "Unknown codec type (%d).\n", avctx->codec_id);
		return AVERROR_UNKNOWN;
	}

	param.frame_pool_size = nvmpi_context->frame_pool_size;
	if(param.frame_pool_size < OPT_frame_pool_size_MIN || param.frame_pool_size > OPT_frame_pool_size_MAX)
	{
		av_log(avctx, AV_LOG_WARNING, "Incorrect frame_pool_size specified: %d. Default (%d) will be used.\n", param.frame_pool_size, OPT_frame_pool_size_DEFAULT);
		param.frame_pool_size = OPT_frame_pool_size_DEFAULT;
	}

	param.chunk_size = nvmpi_context->chunk_size;
	if(param.chunk_size != OPT_chunk_size_AUTO && (param.chunk_size < OPT_chunk_size_MIN || param.chunk_size > OPT_chunk_size_MAX))
	{
		av_log(avctx, AV_LOG_WARNING, "Incorrect chunk_size specified: %d. Auto (libnvmpi default) will be used.\n", nvmpi_context->chunk_size);
		param.chunk_size = OPT_chunk_size_AUTO;
	}

	param.max_perf = nvmpi_context->max_perf;
	param.disable_dpb = nvmpi_context->disable_dpb;
	param.wait_timeout = nvmpi_context->wait_timeout;

	/* Pass container-reported dimensions to libnvmpi so it can pre-allocate
	 * the frame pool before the first resolution-change event, reducing
	 * first-frame latency. Zero values (unknown) = no hint, existing behavior. */
	param.width = avctx->width;
	param.height = avctx->height;

	//Output pixel-format negotiation. The decoder can emit YUV420P, NV12, or
	//(HEVC Main10 only) 10-bit P010LE, all produced natively by the VIC
	//transform — no software conversion. Selection precedence:
	//  1. explicit -output_format option (the reliable CLI knob; non-hwaccel
	//     decoders otherwise always get pix_fmts[0] from default get_format);
	//  2. a usable avctx->pix_fmt already set by the caller (programmatic API
	//     consumers) — YUVJ420P is normalized to YUV420P + JPEG color range;
	//  3. downstream negotiation via ff_get_format, defaulting to YUV420P.
	//Whether the installed libnvmpi actually supports P010 (it needs the
	//NvUtils/JetPack-5+ buffer API) is enforced in nvmpi_create_decoder,
	//which returns NULL otherwise — handled below.
	nchoices = 0;
	choices[nchoices++] = AV_PIX_FMT_YUV420P;
	choices[nchoices++] = AV_PIX_FMT_NV12;
	if(avctx->codec_id == AV_CODEC_ID_HEVC)
		choices[nchoices++] = AV_PIX_FMT_P010LE;
	choices[nchoices++] = AV_PIX_FMT_DRM_PRIME;
	choices[nchoices] = AV_PIX_FMT_NONE;

	/* -hwaccel nvmpi: when an NVMPI hardware device context is attached,
	 * auto-select DRM_PRIME output for zero-copy decode. The hw_device_ctx
	 * is set by FFmpeg's command-line hwaccel machinery before codec init.
	 * This has highest priority in the format negotiation chain. */
	if (avctx->hw_device_ctx) {
		AVHWDeviceContext *device_ctx =
			(AVHWDeviceContext *)avctx->hw_device_ctx->data;
		if (device_ctx->type == AV_HWDEVICE_TYPE_NVMPI)
			avctx->pix_fmt = AV_PIX_FMT_DRM_PRIME;
	}

	if(nvmpi_context->output_format != AV_PIX_FMT_NONE)
	{
		avctx->pix_fmt = nvmpi_context->output_format;
	}
	else if(avctx->pix_fmt == AV_PIX_FMT_YUVJ420P)
	{
		//Deprecated full-range alias: route as YUV420P. The decoder does not
		//probe the stream's range, so no color_range is asserted here.
		avctx->pix_fmt = AV_PIX_FMT_YUV420P;
	}
	else if(avctx->pix_fmt != AV_PIX_FMT_YUV420P &&
	        avctx->pix_fmt != AV_PIX_FMT_NV12 &&
	        avctx->pix_fmt != AV_PIX_FMT_P010LE &&
	        avctx->pix_fmt != AV_PIX_FMT_DRM_PRIME)
	{
		enum AVPixelFormat got = ff_get_format(avctx, choices);
		avctx->pix_fmt = (got != AV_PIX_FMT_NONE) ? got : AV_PIX_FMT_YUV420P;
	}

	if(avctx->pix_fmt == AV_PIX_FMT_P010LE && avctx->codec_id != AV_CODEC_ID_HEVC)
	{
		av_log(avctx, AV_LOG_ERROR, "P010LE (10-bit) output is only supported for HEVC streams.\n");
		return AVERROR_INVALIDDATA;
	}

	switch(avctx->pix_fmt)
	{
		case AV_PIX_FMT_YUV420P:
			param.pixFormat = NV_PIX_YUV420;
			break;
		case AV_PIX_FMT_NV12:
			param.pixFormat = NV_PIX_NV12;
			break;
		case AV_PIX_FMT_P010LE:
			param.pixFormat = NV_PIX_P010;
			break;
		case AV_PIX_FMT_DRM_PRIME:
			/* DRM_PRIME wraps the hardware's native NV12 output as a
			 * DMA-BUF fd — no pixel format conversion needed. */
			param.pixFormat = NV_PIX_NV12;
			break;
		default:
			av_log(avctx, AV_LOG_ERROR, "Invalid Pix_FMT for NVMPI: only YUV420P, YUVJ420P, NV12, P010LE and DRM_PRIME are supported\n");
			return AVERROR_INVALIDDATA;
	}

	/* Create hw_frames_ctx when an NVMPI hwaccel device is attached and
	 * the final output format is DRM_PRIME. This tells downstream filters
	 * and encoders that frames are DRM_PRIME-wrapped DMA-BUF surfaces
	 * with NV12 software layout. Created after format negotiation so
	 * -output_format can override -hwaccel if both are specified. */
	{
		int hret = nvmpi_drm_init_hw_frames(avctx, AV_PIX_FMT_NV12);
		if (hret < 0)
			return hret;
	}

    if (nvmpi_context->resize_expr && sscanf(nvmpi_context->resize_expr, "%dx%d",
                                             &param.resized.width, &param.resized.height) != 2)
	{
        av_log(avctx, AV_LOG_ERROR, "Invalid resize expressions\n");
        return AVERROR(EINVAL);
    }

	//overwrite avctx w and h if resize option is used
	if(param.resized.width && param.resized.height)
	{
		avctx->width = param.resized.width;
		avctx->height = param.resized.height;
	}

	/* DRM_PRIME frames carry a DMA-BUF fd, not pixel data — no
	 * pre-allocated output buffer needed. Each frame is built
	 * on-the-fly in nvmpi_dec_output_frame from the fd returned by libnvmpi. */
	if (avctx->pix_fmt != AV_PIX_FMT_DRM_PRIME) {
		/* Pre-allocate the destination frame via FFmpeg's buffer pool so
		 * nvmpi_dec_output_frame() can copy decoded planes straight into it. A fresh
		 * bufFrame is re-acquired after each frame is handed to the user. */
		nvmpi_context->bufFrame = av_frame_alloc();
		/* Guard against av_frame_alloc OOM — dereferencing NULL here
		 * would crash immediately. Freed in nvmpi_decode_close(). */
		if (!nvmpi_context->bufFrame)
			return AVERROR(ENOMEM);
		nvmpi_context->bufFrame->width = avctx->width;
		nvmpi_context->bufFrame->height = avctx->height;
		if (ff_get_buffer(avctx, nvmpi_context->bufFrame, 0) < 0)
		{
			av_frame_free(&(nvmpi_context->bufFrame));
			nvmpi_context->bufFrame = NULL;
			return AVERROR(ENOMEM);
		}
	} else {
		nvmpi_context->bufFrame = NULL;
	}

	nvmpi_context->ctx=nvmpi_create_decoder(&param);

	if(!nvmpi_context->ctx)
	{
		av_frame_free(&(nvmpi_context->bufFrame));
		nvmpi_context->bufFrame = NULL;
		av_log(avctx, AV_LOG_ERROR, "Failed to nvmpi_create_decoder (code = %d).\n", AVERROR_EXTERNAL);
		return AVERROR_EXTERNAL;
	}

	//Prime the decoder with out-of-band parameter sets (SPS/PPS), if any.
	//RTSP/RTP streams commonly carry them only in the SDP
	//(sprop-parameter-sets): FFmpeg stores those in avctx->extradata and
	//never injects them into the packet stream (the *_mp4toannexb BSFs are
	//pass-through for Annex-B input). The hardware decoder configures its
	//capture plane only after parsing an in-band SPS, so without priming
	//such streams decode zero frames (upstream Keylost/jetson-ffmpeg#14).
	//Only Annex-B extradata (00 00 01 / 00 00 00 01 start code, plus at
	//least one NAL byte) may be fed: avcC/hvcC extradata (MP4/MKV) is not
	//parseable by the hardware and not needed — for those inputs the
	//mp4toannexb BSF inserts the parameter sets in-band at every IDR.
	//mpeg2/mpeg4 sequence/VOS headers are start-code-prefixed too and
	//intentionally primed (same out-of-band failure class). Other
	//CodecPrivate shapes (e.g. Matroska VP9 feature triples, VFW
	//BITMAPINFOHEADER) never start with a start code and must not reach
	//the parser. An empty payload must never be sent here: libnvmpi
	//treats it as the EOS marker. The size cap protects libnvmpi's
	//fixed-size bitstream buffers (4 MB) from hostile extradata — real
	//parameter sets are tens of bytes.
	if(avctx->extradata && avctx->extradata_size >= 4 &&
	   avctx->extradata_size < (1 << 20) &&
	   avctx->extradata[0] == 0 && avctx->extradata[1] == 0 &&
	   (avctx->extradata[2] == 1 ||
	    (avctx->extradata_size >= 5 && avctx->extradata[2] == 0 && avctx->extradata[3] == 1)))
	{
		nvPacket packet = {0};
		packet.payload_size=avctx->extradata_size;
		packet.payload=avctx->extradata;
		packet.pts=0;

		if(nvmpi_decoder_put_packet(nvmpi_context->ctx, &packet) < 0)
		{
			//not fatal: the stream may still carry in-band parameter sets
			av_log(avctx, AV_LOG_WARNING, "Failed to prime decoder with extradata.\n");
		}
	}

   return 0;
}

//AVCodec/FFCodec .close callback: drop the buffered output frame and shut
//down the libnvmpi decoder (joins its capture thread, frees DMA buffers).
static int nvmpi_close(AVCodecContext *avctx)
{
	nvmpiDecodeContext *nvmpi_context = avctx->priv_data;
	/* Single release point of the packet holder (NULL-safe). */
	av_packet_free(&nvmpi_context->pkt);
	if(nvmpi_context->bufFrame)
	{
		av_frame_free(&(nvmpi_context->bufFrame));
		nvmpi_context->bufFrame = NULL;
	}
	/* Guard against NULL ctx — nvmpi_create_decoder may have returned NULL
	 * (V4L2 device unavailable) and .close is called during init cleanup
	 * when FF_CODEC_CAP_INIT_CLEANUP is set. */
	if(!nvmpi_context->ctx)
		return 0;
	return nvmpi_decoder_close(nvmpi_context->ctx);
}

/* Fetch ONE decoded frame from libnvmpi into *frame, if one is ready.
 * wait=0: non-blocking. wait=1: bounded blocking wait (libnvmpi's
 * wait_timeout, default 500 ms), so this never hangs.
 * Returns 0 when *frame holds a frame, 1 when no frame is ready (nothing
 * consumed, *frame untouched), or a negative AVERROR. */
static int nvmpi_dec_output_frame(AVCodecContext *avctx, AVFrame *frame, int wait)
{
	nvmpiDecodeContext *nvmpi_context = avctx->priv_data;
	AVFrame *bufFrame = nvmpi_context->bufFrame;
	nvFrame _nvframe={0};
	int res;

	/* ---- DRM_PRIME path: wrap DMA-BUF fd in AVDRMFrameDescriptor ---- */
	if (avctx->pix_fmt == AV_PIX_FMT_DRM_PRIME) {
		NvmpiDRMFrameInfo info;

		/* Real per-plane layout from libnvmpi >= 4.3.0 (fd2); older libraries
		 * fall back to pitch*height inside the helper (jetson-ffmpeg#131).
		 * Returns 1 when no frame is ready, <0 on error (borrow already
		 * released). */
		res = nvmpi_dec_get_drm_info(avctx, nvmpi_context->ctx, wait, &info);
		if (res != 0)
			return res;

		/* Resolution change: re-derive avctx size + hw_frames_ctx first. */
		res = nvmpi_dec_sync_geometry(avctx, &info);
		if (res < 0)
			return res;

		/* The shared helper dup()s the fd and releases the borrow exactly once
		 * on every path (free order: close dup, release, free). */
		return nvmpi_drm_wrap_frame(frame, &info, avctx->hw_frames_ctx);
	}

	/* ---- Software-frame path: copy decoded pixels into bufFrame ---- */

	/* Re-acquire bufFrame if a prior ff_get_buffer failure left it without
	 * backing store.  Without this guard, the payload[] assignments below
	 * copy NULL pointers into nvframe and libnvmpi writes to address 0. */
	if (bufFrame && !bufFrame->data[0]) {
		bufFrame->width  = avctx->width;
		bufFrame->height = avctx->height;
		bufFrame->format = avctx->pix_fmt;
		if (ff_get_buffer(avctx, bufFrame, 0) < 0)
			return AVERROR(ENOMEM);
	}

	//Point the nvFrame at bufFrame's pre-allocated planes; on success
	//libnvmpi copies the decoded frame directly into FFmpeg-owned memory.
	//Known limit (libnvmpi API): the copying get_frame does not report the
	//frame geometry, so after a mid-stream resolution change software
	//frames keep the allocation size; only DRM_PRIME frames carry the new
	//size (fd2). See jetson-ffmpeg#132.
	_nvframe.payload[0] = bufFrame->data[0];
	_nvframe.payload[1] = bufFrame->data[1];
	_nvframe.payload[2] = bufFrame->data[2];
	_nvframe.linesize[0] = bufFrame->linesize[0];
	_nvframe.linesize[1] = bufFrame->linesize[1];
	_nvframe.linesize[2] = bufFrame->linesize[2];

	res=nvmpi_decoder_get_frame(nvmpi_context->ctx,&_nvframe,wait);

	if(res<0)
		return 1;   //no decoded frame ready (or bounded wait timed out)
	if(res>0)
	{
		//A frame was dequeued but the CPU map for the copy failed: the
		//frame is lost (libnvmpi recycled its buffer); bufFrame holds
		//undefined pixels, so it must not be handed out.
		av_log(avctx, AV_LOG_ERROR, "nvmpi_decoder_get_frame: buffer map failed (%d).\n", res);
		return AVERROR_EXTERNAL;
	}

	//Hand the filled bufFrame to the caller (zero-copy move of the ref).
	//Output format mirrors the negotiated avctx->pix_fmt (YUV420P / NV12 /
	//P010LE) — the bufFrame was allocated against it via ff_get_buffer.
	bufFrame->format=avctx->pix_fmt;
	bufFrame->pts=_nvframe.timestamp;
	bufFrame->pkt_dts = AV_NOPTS_VALUE;
	av_frame_move_ref(frame, bufFrame);

	//...and immediately acquire a fresh buffer for the next frame. On
	//failure bufFrame stays empty and is re-acquired at the top of the
	//next call; the frame already handed out is still valid.
	bufFrame->width = avctx->width;
	bufFrame->height = avctx->height;
	if (ff_get_buffer(avctx, bufFrame, 0) < 0)
		av_log(avctx, AV_LOG_WARNING, "ff_get_buffer failed, retrying on the next frame\n");

	//keep the metadata that belonged to the returned frame, not the new one
	frame->metadata = bufFrame->metadata;
	bufFrame->metadata = NULL;

	return 0;
}

/* Feed the packet just pulled into nvmpi_context->pkt to libnvmpi and drop
 * our reference (libnvmpi memcpy's the payload into its V4L2 OUTPUT buffer,
 * so the packet is unref'd here on every path). Blocks while every
 * OUTPUT buffer is in flight; the caller only gets here after draining
 * the filled-frame queue, so the capture thread is never back-pressured
 * by frames this thread still has to collect (jetson-ffmpeg#132). */
static int nvmpi_dec_put_pkt(AVCodecContext *avctx)
{
	nvmpiDecodeContext *nvmpi_context = avctx->priv_data;
	AVPacket *pkt = nvmpi_context->pkt;
	nvPacket packet = {0};
	int size = pkt->size;
	int res;

	/* An empty payload is libnvmpi's EOS marker, which stops its capture
	 * loop at once (in-flight frames lost). Never forward one: draining
	 * is done by the bounded blocking get in nvmpi_receive_frame. */
	if (!size) {
		av_packet_unref(pkt);
		return 0;
	}

	packet.payload_size=size;
	packet.payload=pkt->data;
	/* Raw elementary streams (e.g. Annex B .h264) reach the decoder
	 * without a pts; the FFmpeg 6.x CLI only fills in a dts. The old
	 * decode callback got best_effort_timestamp from that dts (libavcodec
	 * stamped the output frame with the input packet's dts); receive_frame
	 * frames carry only what libnvmpi hands back, so pass the dts through
	 * the V4L2 timestamp when there is no pts. Otherwise every frame is
	 * NOPTS and the 6.x CLI drops most of them. */
	packet.pts = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts : pkt->dts;

	res=nvmpi_decoder_put_packet(nvmpi_context->ctx,&packet);
	av_packet_unref(pkt);   //single release point of the pending packet
	if(res < 0)
	{
		//-3 = packet exceeds chunk_size: invalid input data, the packet
		//is dropped and the decoder stays usable. Anything else is a V4L2
		//queue/dequeue failure.
		av_log(avctx, AV_LOG_ERROR,
		       "nvmpi_decoder_put_packet failed (code=%d, packet=%d bytes).\n",
		       res, size);
		return (res == -3) ? AVERROR_INVALIDDATA : AVERROR_EXTERNAL;
	}
	return 0;
}

//receive_frame callback (FF_CODEC_RECEIVE_FRAME_CB, libavcodec 60-63):
//the decoder pulls its own input with ff_decode_get_packet(), which also
//runs the codec's bsfs (h264/hevc mp4toannexb). Order per iteration:
//  1. return a ready frame if there is one (non-blocking get);
//  2. at end of input, wait (bounded) for frames still in the hw pipeline,
//     then report AVERROR_EOF;
//  3. otherwise take the next packet (AVERROR(EAGAIN) = feed me) and put it.
//Draining before every put keeps libnvmpi's filled-frame queue from
//building up lag. The old decode callback (one packet in, at most one
//frame out) let it fill the whole frame pool; a resolution change at that
//point deadlocked put_packet against the capture thread (jetson-ffmpeg#132).
static int nvmpi_receive_frame(AVCodecContext *avctx, AVFrame *frame)
{
	nvmpiDecodeContext *nvmpi_context = avctx->priv_data;
	int low_delay = !!(avctx->flags & AV_CODEC_FLAG_LOW_DELAY);
	int ret;

	for (;;) {
		ret = nvmpi_dec_output_frame(avctx, frame, 0);
		if (ret <= 0)
			return ret;   //got a frame (0) or an error (<0)

		if (nvmpi_context->draining) {
			//Input exhausted: frames may still be in the hw pipeline. Wait
			//for each with libnvmpi's bounded timeout; a timeout ends the
			//stream. libavcodec stops calling after AVERROR_EOF until
			//flush, which clears draining.
			ret = nvmpi_dec_output_frame(avctx, frame, 1);
			return (ret == 1) ? AVERROR_EOF : ret;
		}

		//No frame ready: only now pull the next packet. nvmpi_context->pkt
		//is a reusable holder; nvmpi_dec_put_pkt() releases it on
		//every path, so no packet stays pending across calls.
		ret = ff_decode_get_packet(avctx, nvmpi_context->pkt);
		if (ret == AVERROR_EOF) {
			nvmpi_context->draining = 1;
			continue;
		}
		if (ret < 0)
			return ret;   //AVERROR(EAGAIN): caller must send a packet

		ret = nvmpi_dec_put_pkt(avctx);
		if (ret < 0)
			return ret;

		//Low-delay: wait (bounded) for this packet's frame right away,
		//as the old decode callback did, instead of returning EAGAIN.
		if (low_delay) {
			ret = nvmpi_dec_output_frame(avctx, frame, 1);
			if (ret <= 0)
				return ret;
		}
	}
}



//AVCodec/FFCodec .flush callback: reset the libnvmpi decoder pipeline
//(stops capture thread, drains stale frames, restarts) and re-prime
//the hardware with extradata (SPS/PPS) so it can reconfigure its capture
//plane. Called by avcodec_flush_buffers() on seek / stream restart.
static void nvmpi_flush_decoder(AVCodecContext *avctx)
{
	nvmpiDecodeContext *nvmpi_context = avctx->priv_data;

	nvmpi_decoder_flush(nvmpi_context->ctx);

	/* Seek/restart: input is live again. The packet holder is always
	 * empty between receive_frame calls; unref is a defensive no-op. */
	nvmpi_context->draining = 0;
	av_packet_unref(nvmpi_context->pkt);

	if(avctx->extradata && avctx->extradata_size >= 4 &&
	   avctx->extradata_size < (1 << 20) &&
	   avctx->extradata[0] == 0 && avctx->extradata[1] == 0 &&
	   (avctx->extradata[2] == 1 ||
	    (avctx->extradata_size >= 5 && avctx->extradata[2] == 0 && avctx->extradata[3] == 1)))
	{
		nvPacket packet = {0};
		packet.payload_size=avctx->extradata_size;
		packet.payload=avctx->extradata;
		packet.pts=0;
		{
			int put_ret = nvmpi_decoder_put_packet(nvmpi_context->ctx, &packet);
			if (put_ret < 0)
				av_log(avctx, AV_LOG_WARNING,
				       "nvmpi: failed to re-prime decoder with extradata "
				       "after flush (ret=%d); decode may produce artifacts\n",
				       put_ret);
		}
	}
}

//AVOption table: user-settable private options, resolved into
//nvmpiDecodeContext fields by FFmpeg's option system before .init runs.
//OFFSET locates the field inside the priv_data struct; VD restricts the
//options to video decoding.
#define OFFSET(x) offsetof(nvmpiDecodeContext, x)
#define VD AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_DECODING_PARAM
static const AVOption options[] = {
    { "resize",   "Resize (width)x(height)", OFFSET(resize_expr), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, VD, "resize" },
    { "frame_pool_size", "Number of frames that could be buffered in the decoder before user must read it with avcodec_receive_frame()", OFFSET(frame_pool_size), AV_OPT_TYPE_INT, {.i64 = OPT_frame_pool_size_DEFAULT }, OPT_frame_pool_size_MIN, OPT_frame_pool_size_MAX, VD, "frame_pool_size" },
    { "chunk_size", "Bytes per compressed-input buffer; one input packet must fit in one chunk (0 = auto, 10 MiB)", OFFSET(chunk_size), AV_OPT_TYPE_INT, {.i64 = OPT_chunk_size_AUTO }, 0, OPT_chunk_size_MAX, VD, "chunk_size" },
    { "max_perf", "Enable max performance mode (lifts NVDEC clock governor)", OFFSET(max_perf), AV_OPT_TYPE_BOOL, {.i64 = 1 }, 0, 1, VD, "max_perf" },
    { "disable_dpb", "Disable decoded-picture-buffer reordering (low-latency, B-frame-free streams only)", OFFSET(disable_dpb), AV_OPT_TYPE_BOOL, {.i64 = 0 }, 0, 1, VD, "disable_dpb" },
    { "wait_timeout", "Blocking wait timeout in milliseconds for low-delay mode (0 = default 500ms)", OFFSET(wait_timeout), AV_OPT_TYPE_INT, {.i64 = OPT_wait_timeout_AUTO }, 0, OPT_wait_timeout_MAX, VD, "wait_timeout" },
    { "output_format", "Decoder output pixel format (default auto=yuv420p; nv12; p010le for 10-bit HEVC)", OFFSET(output_format), AV_OPT_TYPE_PIXEL_FMT, {.i64 = AV_PIX_FMT_NONE }, -1, INT_MAX, VD },
    { NULL }
};

//Per-codec AVClass: binds the shared option table above to each decoder
//instance (referenced by priv_class in the registration struct below).
#define NVMPI_DEC_CLASS(NAME) \
	static const AVClass nvmpi_##NAME##_dec_class = { \
		.class_name = "nvmpi_" #NAME "_dec", \
		.option     = options, \
		.version    = LIBAVUTIL_VERSION_INT, \
	};

//Codec registration struct, stamped out once per codec by NVMPI_DEC().
//FFCodec variant for libavcodec >= 60 (FFmpeg 6.0+). The matching extern in
//allcodecs.c is "extern const FFCodec". Capabilities: DELAY, AVOID_PROBING,
//HARDWARE. BSFS inserts h264/hevc mp4toannexb for Annex-B NAL units.
#define NVMPI_DEC(NAME, ID, BSFS) \
	NVMPI_DEC_CLASS(NAME) \
	const FFCodec ff_##NAME##_nvmpi_decoder = { \
		.p.name           = #NAME "_nvmpi", \
		CODEC_LONG_NAME(#NAME " (nvmpi)"), \
		.p.type           = AVMEDIA_TYPE_VIDEO, \
		.p.id             = ID, \
		.priv_data_size = sizeof(nvmpiDecodeContext), \
		.init           = nvmpi_init_decoder, \
		.close          = nvmpi_close, \
		/* receive_frame: drain-before-put, see nvmpi_receive_frame (#132) */ \
		FF_CODEC_RECEIVE_FRAME_CB(nvmpi_receive_frame), \
		.flush          = nvmpi_flush_decoder, \
		.p.priv_class     = &nvmpi_##NAME##_dec_class, \
		.p.capabilities   = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AVOID_PROBING | AV_CODEC_CAP_HARDWARE, \
		.caps_internal  = FF_CODEC_CAP_INIT_CLEANUP, \
		/* libavcodec 63 (FFmpeg 9.0+): pix_fmts moved from AVCodec to FFCodec */ \
		NVMPI_DEC_PIXFMTS, \
		.bsfs           = BSFS, \
		.p.wrapper_name   = "nvmpi", \
	};


//Instantiate the nvmpi decoders. Each expansion must have a matching
//extern in allcodecs.c and a CONFIG_*_NVMPI_DECODER Makefile/configure
//entry (added by ffpatch.sh / the version overlays).
NVMPI_DEC(h264,  AV_CODEC_ID_H264,"h264_mp4toannexb");
NVMPI_DEC(hevc,  AV_CODEC_ID_HEVC,"hevc_mp4toannexb");
NVMPI_DEC(mpeg2, AV_CODEC_ID_MPEG2VIDEO,NULL);
NVMPI_DEC(mpeg4, AV_CODEC_ID_MPEG4,NULL);
NVMPI_DEC(vp9,  AV_CODEC_ID_VP9,NULL);
NVMPI_DEC(vp8, AV_CODEC_ID_VP8,NULL);
//No bsf: libnvmpi takes one low-overhead-format temporal unit per packet as is.
NVMPI_DEC(av1, AV_CODEC_ID_AV1,NULL);
