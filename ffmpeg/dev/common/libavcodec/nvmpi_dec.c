/*
 * nvmpi_dec.c — FFmpeg decoder wrapper for libnvmpi (layer 2 of
 * jetson-ffmpeg, the FFmpeg integration layer).
 *
 * This file is NOT built in this repository: it is patched into a vanilla
 * FFmpeg source tree (libavcodec/) by scripts/ffpatch.sh or the generated
 * patches in ffmpeg/patches/, and compiled as part of FFmpeg itself. It
 * implements FFmpeg decoder codecs (h264_nvmpi, hevc_nvmpi, mpeg2_nvmpi,
 * mpeg4_nvmpi, vp8_nvmpi, vp9_nvmpi) by delegating all real work to the
 * libnvmpi C API (<nvmpi.h>, implemented in src/nvmpi_dec.cpp).
 *
 * One source file supports FFmpeg 4.2 through 8.0+: API differences are
 * handled with LIBAVCODEC_VERSION_MAJOR preprocessor guards (see
 * https://github.com/gjrtimmer/jetson-ffmpeg/wiki/Development-Guide "Wrapper code paths by FFmpeg version"). For the
 * decoder the only breakpoint is the AVCodec -> FFCodec registration
 * change in libavcodec 60 (FFmpeg 6.0).
 */
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

#include <nvmpi.h>
#include "avcodec.h"
#include "decode.h"
#include "internal.h"
#include "libavutil/buffer.h"
#include "libavutil/common.h"
#include "libavutil/frame.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_drm.h"
#include "libavutil/imgutils.h"
#include "libavutil/log.h"
#include "libavutil/opt.h"

//libavcodec >= 60 (FFmpeg 6.0+) moved the internal codec description to
//FFCodec in codec_internal.h; older versions register a plain AVCodec and
//must not include this private header.
#if LIBAVCODEC_VERSION_MAJOR >= 60
#include "codec_internal.h"
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
	char *resize_expr;     //"-resize WxH" option (hw downscale in libnvmpi)
	int frame_pool_size;   //"-frame_pool_size N" option
	int chunk_size;        //"-chunk_size N" option (bytes; 0 = auto)
	int max_perf;          //lift NVDEC clock governor (default on)
	int disable_dpb;       //skip DPB reordering (low-latency, default off)
	int wait_timeout;      //blocking wait timeout in ms (0 = default 500ms)
	int output_format;     //requested output pixel format (AVPixelFormat; NONE = auto)
} nvmpiDecodeContext;

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
	enum AVPixelFormat choices[4];
	int nchoices;

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
	choices[nchoices] = AV_PIX_FMT_NONE;

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
	        avctx->pix_fmt != AV_PIX_FMT_P010LE)
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
		default:
			av_log(avctx, AV_LOG_ERROR, "Invalid Pix_FMT for NVMPI: only YUV420P, YUVJ420P, NV12 and P010LE are supported\n");
			return AVERROR_INVALIDDATA;
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

	//Pre-allocate the destination frame via FFmpeg's buffer pool so
	//nvmpi_decode() can copy decoded planes straight into it. A fresh
	//bufFrame is re-acquired after each frame is handed to the user.
	nvmpi_context->bufFrame = av_frame_alloc();
	nvmpi_context->bufFrame->width = avctx->width;
	nvmpi_context->bufFrame->height = avctx->height;
	if (ff_get_buffer(avctx, nvmpi_context->bufFrame, 0) < 0)
	{
		av_frame_free(&(nvmpi_context->bufFrame));
		nvmpi_context->bufFrame = NULL;
		return AVERROR(ENOMEM);
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

//Classic .decode callback (wired up via FF_CODEC_DECODE_CB on >= 60):
//one call feeds one compressed packet to libnvmpi and, if a decoded frame
//is already available, returns it via *data/got_frame. Asymmetric latency
//(decoder pipeline depth) is absorbed by libnvmpi's frame pool.
//Signature guard: libavcodec 60 changed the decode callback's output
//argument from void* to a typed AVFrame*; both spellings receive the same
//pointer, so the body is shared.
#if LIBAVCODEC_VERSION_MAJOR >= 60
static int nvmpi_decode(AVCodecContext *avctx, AVFrame *data, int *got_frame, AVPacket *avpkt)
#else
static int nvmpi_decode(AVCodecContext *avctx, void *data, int *got_frame, AVPacket *avpkt)
#endif
{
	nvmpiDecodeContext *nvmpi_context = avctx->priv_data;
	AVFrame *frame = data;
	AVFrame *bufFrame = nvmpi_context->bufFrame;
	nvFrame _nvframe={0};
	nvPacket packet;
	int res;
	int decode_ret = avpkt->size;

	//Feed the compressed packet (if any — FFmpeg sends empty packets while
	//draining). libnvmpi memcpy's the payload, so avpkt stays caller-owned.
	if(avpkt->size)
	{
		packet.payload_size=avpkt->size;
		packet.payload=avpkt->data;
		packet.pts=avpkt->pts;

		res=nvmpi_decoder_put_packet(nvmpi_context->ctx,&packet);
		if(res < 0)
		{
			//A decode callback must never return AVERROR(EAGAIN) for a
			//consumed packet — libavcodec >= 6.1 asserts on it
			//(decode.c: "Assertion consumed != AVERROR(EAGAIN)").
			//-3 = packet exceeds chunk_size: invalid input data, the
			//decoder stays usable. Anything else is a V4L2 queue/dequeue
			//failure.
			av_log(avctx, AV_LOG_ERROR,
			       "nvmpi_decoder_put_packet failed (code=%d, packet=%d bytes).\n",
			       res, avpkt->size);
			return (res == -3) ? AVERROR_INVALIDDATA : AVERROR_EXTERNAL;
		}
	}

	//Point the nvFrame at bufFrame's pre-allocated planes; on success
	//libnvmpi copies the decoded frame directly into FFmpeg-owned memory.
	_nvframe.payload[0] = bufFrame->data[0];
	_nvframe.payload[1] = bufFrame->data[1];
	_nvframe.payload[2] = bufFrame->data[2];
	_nvframe.linesize[0] = bufFrame->linesize[0];
	_nvframe.linesize[1] = bufFrame->linesize[1];
	_nvframe.linesize[2] = bufFrame->linesize[2];

	res=nvmpi_decoder_get_frame(nvmpi_context->ctx,&_nvframe,avctx->flags & AV_CODEC_FLAG_LOW_DELAY);

	if(res<0)
	{
		//no decoded frame ready yet — not an error, just got_frame == 0
		return decode_ret;
	}

	//Hand the filled bufFrame to the caller (zero-copy move of the ref).
	//Output format mirrors the negotiated avctx->pix_fmt (YUV420P / NV12 /
	//P010LE) — the bufFrame was allocated against it via ff_get_buffer.
	bufFrame->format=avctx->pix_fmt;
	bufFrame->pts=_nvframe.timestamp;
	bufFrame->pkt_dts = AV_NOPTS_VALUE;
	av_frame_move_ref(frame, bufFrame);

	*got_frame = 1;

	//...and immediately acquire a fresh buffer for the next decode call.
	bufFrame->width = avctx->width;
	bufFrame->height = avctx->height;
	if (ff_get_buffer(avctx, bufFrame, 0) < 0)
	{
		av_log(avctx, AV_LOG_ERROR, "ff_get_buffer failed\n");
		return AVERROR(ENOMEM);
	}
	
	//keep the metadata that belonged to the returned frame, not the new one
	frame->metadata = bufFrame->metadata;
	bufFrame->metadata = NULL;

	return decode_ret;
}



//AVCodec/FFCodec .flush callback: reset the libnvmpi decoder pipeline
//(stops capture thread, drains stale frames, restarts) and re-prime
//the hardware with extradata (SPS/PPS) so it can reconfigure its capture
//plane. Called by avcodec_flush_buffers() on seek / stream restart.
static void nvmpi_flush_decoder(AVCodecContext *avctx)
{
	nvmpiDecodeContext *nvmpi_context = avctx->priv_data;

	nvmpi_decoder_flush(nvmpi_context->ctx);

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
		nvmpi_decoder_put_packet(nvmpi_context->ctx, &packet);
	}
}

/* ------------------------------------------------------------------ */
/* MJPEG-specific callbacks (NvJPEGDecoder, not V4L2)                  */
/* ------------------------------------------------------------------ */

//MJPEG .init: create the JPEG decoder (no V4L2 device, no capture thread).
//Output is always YUV420P with JPEG (full) color range.
static int nvmpi_init_mjpeg_decoder(AVCodecContext *avctx)
{
	nvmpiDecodeContext *nvmpi_context = avctx->priv_data;

	avctx->pix_fmt = AV_PIX_FMT_YUV420P;
	avctx->color_range = AVCOL_RANGE_JPEG;

	nvmpi_context->bufFrame = av_frame_alloc();
	if (!nvmpi_context->bufFrame)
		return AVERROR(ENOMEM);

	nvmpi_context->ctx = nvmpi_create_jpeg_decoder();
	if (!nvmpi_context->ctx)
	{
		av_frame_free(&(nvmpi_context->bufFrame));
		nvmpi_context->bufFrame = NULL;
		av_log(avctx, AV_LOG_ERROR, "Failed to create JPEG hardware decoder.\n");
		return AVERROR_EXTERNAL;
	}

	return 0;
}

//MJPEG .close: destroy the JPEG decoder.
static int nvmpi_close_mjpeg(AVCodecContext *avctx)
{
	nvmpiDecodeContext *nvmpi_context = avctx->priv_data;
	if (nvmpi_context->bufFrame)
	{
		av_frame_free(&(nvmpi_context->bufFrame));
		nvmpi_context->bufFrame = NULL;
	}
	if (!nvmpi_context->ctx)
		return 0;
	return nvmpi_jpeg_decoder_close(nvmpi_context->ctx);
}

//MJPEG .decode: synchronous per-frame decode.
#if LIBAVCODEC_VERSION_MAJOR >= 60
static int nvmpi_decode_mjpeg(AVCodecContext *avctx, AVFrame *data, int *got_frame, AVPacket *avpkt)
#else
static int nvmpi_decode_mjpeg(AVCodecContext *avctx, void *data, int *got_frame, AVPacket *avpkt)
#endif
{
	nvmpiDecodeContext *nvmpi_context = avctx->priv_data;
	AVFrame *frame = data;
	nvFrame _nvframe = {0};
	nvPacket packet;
	int res;

	if (!avpkt->size)
	{
		*got_frame = 0;
		return 0;
	}

	packet.payload_size = avpkt->size;
	packet.payload = avpkt->data;
	packet.pts = avpkt->pts;

	res = nvmpi_jpeg_decoder_put_packet(nvmpi_context->ctx, &packet);
	if (res < 0)
	{
		av_log(avctx, AV_LOG_ERROR,
		       "nvmpi_jpeg_decoder_put_packet failed (code=%d).\n", res);
		return AVERROR_EXTERNAL;
	}

	//Allocate or refresh bufFrame at the decoded resolution.
	//JPEG dimensions come from the bitstream; we must update avctx once known.
	AVFrame *bufFrame = nvmpi_context->bufFrame;
	if (!bufFrame->data[0] || bufFrame->width != avctx->width || bufFrame->height != avctx->height)
	{
		av_frame_unref(bufFrame);
		bufFrame->format = avctx->pix_fmt;
		bufFrame->width = avctx->width ? avctx->width : 1280;
		bufFrame->height = avctx->height ? avctx->height : 720;
		if (ff_get_buffer(avctx, bufFrame, 0) < 0)
			return AVERROR(ENOMEM);
	}

	_nvframe.payload[0] = bufFrame->data[0];
	_nvframe.payload[1] = bufFrame->data[1];
	_nvframe.payload[2] = bufFrame->data[2];
	_nvframe.linesize[0] = bufFrame->linesize[0];
	_nvframe.linesize[1] = bufFrame->linesize[1];
	_nvframe.linesize[2] = bufFrame->linesize[2];

	res = nvmpi_jpeg_decoder_get_frame(nvmpi_context->ctx, &_nvframe, 1);
	if (res < 0)
	{
		*got_frame = 0;
		return avpkt->size;
	}

	//Update avctx dimensions from the decoded frame (JPEG is self-describing).
	if (_nvframe.width && _nvframe.height &&
	    ((int)_nvframe.width != avctx->width || (int)_nvframe.height != avctx->height))
	{
		avctx->width = _nvframe.width;
		avctx->height = _nvframe.height;

		//Reallocate bufFrame at new resolution.
		av_frame_unref(bufFrame);
		bufFrame->format = avctx->pix_fmt;
		bufFrame->width = avctx->width;
		bufFrame->height = avctx->height;
		if (ff_get_buffer(avctx, bufFrame, 0) < 0)
			return AVERROR(ENOMEM);

		//Re-decode into new buffer.
		_nvframe.payload[0] = bufFrame->data[0];
		_nvframe.payload[1] = bufFrame->data[1];
		_nvframe.payload[2] = bufFrame->data[2];
		_nvframe.linesize[0] = bufFrame->linesize[0];
		_nvframe.linesize[1] = bufFrame->linesize[1];
		_nvframe.linesize[2] = bufFrame->linesize[2];
		//Frame is already in the pool from the first get_frame; just copy it.
	}

	bufFrame->format = avctx->pix_fmt;
	bufFrame->pts = _nvframe.timestamp;
	bufFrame->pkt_dts = AV_NOPTS_VALUE;
	av_frame_move_ref(frame, bufFrame);

	*got_frame = 1;

	//Re-acquire a fresh bufFrame for the next decode.
	bufFrame->width = avctx->width;
	bufFrame->height = avctx->height;
	bufFrame->format = avctx->pix_fmt;
	if (ff_get_buffer(avctx, bufFrame, 0) < 0)
	{
		av_log(avctx, AV_LOG_ERROR, "ff_get_buffer failed\n");
		return AVERROR(ENOMEM);
	}

	frame->metadata = bufFrame->metadata;
	bufFrame->metadata = NULL;

	return avpkt->size;
}

//MJPEG .flush: JPEG is stateless per-picture, nothing to reset.
static void nvmpi_flush_mjpeg(AVCodecContext *avctx)
{
	(void)avctx;
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
//Two variants exist because libavcodec 60 (FFmpeg 6.0) split the codec
//description into a public AVCodec (now the .p sub-struct) plus private
//FFCodec fields, and replaced the bare .decode pointer with the
//FF_CODEC_DECODE_CB() wrapper. The matching extern declaration patched
//into allcodecs.c differs likewise ("extern AVCodec" vs "extern const
//FFCodec") — that is why version overlay files exist (https://github.com/gjrtimmer/jetson-ffmpeg/wiki/Development-Guide).
//Shared semantics of both variants:
//  - name "<codec>_nvmpi", wrapper_name "nvmpi" (how FFmpeg knows this is
//    a hw wrapper around an external implementation);
//  - capabilities: DELAY (frames come out later than packets go in),
//    AVOID_PROBING, HARDWARE;
//  - BSFS optionally inserts a bitstream filter — h264/hevc use
//    *_mp4toannexb so libnvmpi always receives Annex-B NAL units.
#if LIBAVCODEC_VERSION_MAJOR >= 60
	//FFCodec variant for libavcodec >= 60 (FFmpeg 6.0/6.1/7.x/8.x)
	#define NVMPI_DEC(NAME, ID, BSFS) \
		NVMPI_DEC_CLASS(NAME) \
		FFCodec ff_##NAME##_nvmpi_decoder = { \
			.p.name           = #NAME "_nvmpi", \
			CODEC_LONG_NAME(#NAME " (nvmpi)"), \
			.p.type           = AVMEDIA_TYPE_VIDEO, \
			.p.id             = ID, \
			.priv_data_size = sizeof(nvmpiDecodeContext), \
			.init           = nvmpi_init_decoder, \
			.close          = nvmpi_close, \
			FF_CODEC_DECODE_CB(nvmpi_decode), \
			.flush          = nvmpi_flush_decoder, \
			.p.priv_class     = &nvmpi_##NAME##_dec_class, \
			.p.capabilities   = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AVOID_PROBING | AV_CODEC_CAP_HARDWARE, \
			.caps_internal  = FF_CODEC_CAP_INIT_CLEANUP, \
			.p.pix_fmts	=(const enum AVPixelFormat[]){AV_PIX_FMT_YUV420P,AV_PIX_FMT_NV12,AV_PIX_FMT_P010LE,AV_PIX_FMT_NONE},\
			.bsfs           = BSFS, \
			.p.wrapper_name   = "nvmpi", \
		};
#else
	//legacy AVCodec variant for libavcodec < 60 (FFmpeg 4.2/4.4/5.x)
	#define NVMPI_DEC(NAME, ID, BSFS) \
		NVMPI_DEC_CLASS(NAME) \
		AVCodec ff_##NAME##_nvmpi_decoder = { \
			.name           = #NAME "_nvmpi", \
			.long_name      = NULL_IF_CONFIG_SMALL(#NAME " (nvmpi)"), \
			.type           = AVMEDIA_TYPE_VIDEO, \
			.id             = ID, \
			.priv_data_size = sizeof(nvmpiDecodeContext), \
			.init           = nvmpi_init_decoder, \
			.close          = nvmpi_close, \
			.decode         = nvmpi_decode, \
			.flush          = nvmpi_flush_decoder, \
			.priv_class     = &nvmpi_##NAME##_dec_class, \
			.capabilities   = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AVOID_PROBING | AV_CODEC_CAP_HARDWARE, \
			.caps_internal  = FF_CODEC_CAP_INIT_CLEANUP, \
			.pix_fmts	=(const enum AVPixelFormat[]){AV_PIX_FMT_YUV420P,AV_PIX_FMT_NV12,AV_PIX_FMT_P010LE,AV_PIX_FMT_NONE},\
			.bsfs           = BSFS, \
			.wrapper_name   = "nvmpi", \
		};
#endif


//Instantiate the six nvmpi decoders. Each expansion must have a matching
//extern in allcodecs.c and a CONFIG_*_NVMPI_DECODER Makefile/configure
//entry (added by ffpatch.sh / the version overlays).
NVMPI_DEC(h264,  AV_CODEC_ID_H264,"h264_mp4toannexb");
NVMPI_DEC(hevc,  AV_CODEC_ID_HEVC,"hevc_mp4toannexb");
NVMPI_DEC(mpeg2, AV_CODEC_ID_MPEG2VIDEO,NULL);
NVMPI_DEC(mpeg4, AV_CODEC_ID_MPEG4,NULL);
NVMPI_DEC(vp9,  AV_CODEC_ID_VP9,NULL);
NVMPI_DEC(vp8, AV_CODEC_ID_VP8,NULL);

//MJPEG decoder — uses NvJPEGDecoder, not V4L2 M2M. Separate init/decode/
//close/flush callbacks, and only YUV420P output (no NV12/P010).
#if LIBAVCODEC_VERSION_MAJOR >= 60
	NVMPI_DEC_CLASS(mjpeg)
	FFCodec ff_mjpeg_nvmpi_decoder = {
		.p.name           = "mjpeg_nvmpi",
		CODEC_LONG_NAME("mjpeg (nvmpi)"),
		.p.type           = AVMEDIA_TYPE_VIDEO,
		.p.id             = AV_CODEC_ID_MJPEG,
		.priv_data_size = sizeof(nvmpiDecodeContext),
		.init           = nvmpi_init_mjpeg_decoder,
		.close          = nvmpi_close_mjpeg,
		FF_CODEC_DECODE_CB(nvmpi_decode_mjpeg),
		.flush          = nvmpi_flush_mjpeg,
		.p.priv_class     = &nvmpi_mjpeg_dec_class,
		.p.capabilities   = AV_CODEC_CAP_AVOID_PROBING | AV_CODEC_CAP_HARDWARE,
		.caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,
		.p.pix_fmts     = (const enum AVPixelFormat[]){AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE},
		.p.wrapper_name   = "nvmpi",
	};
#else
	NVMPI_DEC_CLASS(mjpeg)
	AVCodec ff_mjpeg_nvmpi_decoder = {
		.name           = "mjpeg_nvmpi",
		.long_name      = NULL_IF_CONFIG_SMALL("mjpeg (nvmpi)"),
		.type           = AVMEDIA_TYPE_VIDEO,
		.id             = AV_CODEC_ID_MJPEG,
		.priv_data_size = sizeof(nvmpiDecodeContext),
		.init           = nvmpi_init_mjpeg_decoder,
		.close          = nvmpi_close_mjpeg,
		.decode         = nvmpi_decode_mjpeg,
		.flush          = nvmpi_flush_mjpeg,
		.priv_class     = &nvmpi_mjpeg_dec_class,
		.capabilities   = AV_CODEC_CAP_AVOID_PROBING | AV_CODEC_CAP_HARDWARE,
		.caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,
		.pix_fmts       = (const enum AVPixelFormat[]){AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE},
		.wrapper_name   = "nvmpi",
	};
#endif

