/*
 * nvmpi_dec_mjpeg.c — FFmpeg MJPEG decoder wrapper for libnvmpi's JPEG
 * decoder (layer 2 of jetson-ffmpeg, the FFmpeg integration layer).
 *
 * This is a SEPARATE file from nvmpi_dec.c because the MJPEG decode path is
 * fundamentally different from the V4L2-based H.264/HEVC/MPEG2/MPEG4/VP8/VP9
 * decoders:
 *   - Uses NvJPEGDecoder, not V4L2 M2M (no capture thread, no frame pool)
 *   - Synchronous per-frame decode, with self-describing resolution that
 *     may require a re-decode when the bitstream's dimensions differ from
 *     avctx's current dimensions
 *   - YUV420P output with JPEG (full) color range by default (software
 *     frames); opt-in DRM_PRIME zero-copy output when libnvmpi >= 4.1.0
 *     (see nvmpi_dec_mjpeg_fd.h) — no P010LE
 *
 * This file is NOT built in this repository: it is patched into a vanilla
 * FFmpeg source tree (libavcodec/) by scripts/ffpatch.sh or the generated
 * patches in ffmpeg/patches/, and compiled as part of FFmpeg itself.
 *
 * The private context struct, AVOption table, and codec-registration
 * macros below are intentionally identical to the ones in nvmpi_dec.c —
 * the mjpeg_nvmpi decoder shares the same priv_data layout and options as
 * the V4L2-backed decoders (most are simply unused by this decode path).
 * Each nvmpi_dec*.c file is patched in and compiled as its own translation
 * unit, so this duplication is required for the file to be self-contained;
 * it must be kept in sync with nvmpi_dec.c if either changes.
 *
 * One source file supports FFmpeg 6.0 through 9.0+: API differences are
 * handled with LIBAVCODEC_VERSION_MAJOR preprocessor guards (see
 * https://github.com/gjrtimmer/jetson-ffmpeg/wiki/Development-Guide "Wrapper code paths by FFmpeg version").
 */

/* Runtime-loaded via dlopen -- no link-time dependency on libnvmpi.so.
 * dynlink_nvmpi.h is self-contained (duplicates nvmpi.h types) and
 * provides macro redirects so all nvmpi_* calls dispatch through
 * function pointers resolved at codec init time. */
#include "dynlink_nvmpi.h"
#include "avcodec.h"
#include "decode.h"
#include "internal.h"
#include "libavutil/common.h"
#include "libavutil/frame.h"
#include "libavutil/imgutils.h"
#include "libavutil/mem.h"
#include "libavutil/log.h"
#include "libavutil/opt.h"

#include "codec_internal.h"
#include "version.h"

/*
 * pix_fmts is a static table, so DRM_PRIME is listed after YUV420P (the
 * default) unconditionally; whether it is really usable (libnvmpi >= 4.1.0)
 * is decided at runtime in .init, which falls back to YUV420P otherwise.
 * libavcodec 63 (FFmpeg 9.0+): pix_fmts moved from the public AVCodec (.p)
 * to a direct field on FFCodec in codec_internal.h. Pre-63 uses .p.pix_fmts;
 * 63+ uses .pix_fmts (or the CODEC_PIXFMTS() macro).
 */
#if LIBAVCODEC_VERSION_MAJOR >= 63
/* libavcodec 63 (FFmpeg 9.0+): pix_fmts on FFCodec directly */
#define NVMPI_MJPEG_PIXFMTS \
	.pix_fmts = (const enum AVPixelFormat[]){AV_PIX_FMT_YUV420P, AV_PIX_FMT_DRM_PRIME, AV_PIX_FMT_NONE}
#else
/* libavcodec 60-62 (FFmpeg 6.0-8.x): pix_fmts on public AVCodec sub-struct */
#define NVMPI_MJPEG_PIXFMTS \
	.p.pix_fmts = (const enum AVPixelFormat[]){AV_PIX_FMT_YUV420P, AV_PIX_FMT_DRM_PRIME, AV_PIX_FMT_NONE}
#endif

//valid range / default for the frame_pool_size AVOption (mirrors the size
//of libnvmpi's decoded-frame pool, i.e. how many frames may buffer up
//before the user must call avcodec_receive_frame()). Unused by the MJPEG
//decode path but present because this decoder shares nvmpi_dec.c's options
//table (see file header).
#define OPT_frame_pool_size_MIN 1
#define OPT_frame_pool_size_MAX 32
#define OPT_frame_pool_size_DEFAULT 5

//valid range for the chunk_size AVOption (bytes per compressed-input
//buffer inside libnvmpi). Unused by the MJPEG decode path (see above).
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
	int drm_out;           //mjpeg only (appended: keeps all AVOption offsets): 1 = DRM_PRIME fd output
} nvmpiDecodeContext;

#include "nvmpi_dec_mjpeg_fd.h"
#include "nvmpi_dec_sw.h"

/* ------------------------------------------------------------------ */
/* MJPEG-specific callbacks (NvJPEGDecoder, not V4L2)                  */
/* ------------------------------------------------------------------ */

//MJPEG .init: create the JPEG decoder (no V4L2 device, no capture thread).
//Default output is YUV420P with JPEG (full) color range. DRM_PRIME (NV12
//DMA-BUF) is used only when explicitly requested AND libnvmpi >= 4.1.0.
static int nvmpi_init_mjpeg_decoder(AVCodecContext *avctx)
{
	nvmpiDecodeContext *nvmpi_context = avctx->priv_data;
	int ret;

	/* Load libnvmpi.so via dlopen (same as the V4L2 decoder path). */
	if (nvmpi_dynlink_load() < 0) {
		av_log(avctx, AV_LOG_ERROR,
		       "Failed to load libnvmpi.so: %s\n"
		       "Install libnvmpi for hardware-accelerated MJPEG decoding on Jetson.\n",
		       dlerror());
		return AVERROR_EXTERNAL;
	}

	avctx->color_range = AVCOL_RANGE_JPEG;

	//Opt-in zero-copy path. Falls back (with a warning, never an error) to
	//the software path below when the fd API is missing or unusable.
	if (nvmpi_mjpeg_drm_requested(avctx, nvmpi_context))
	{
		nvmpi_context->ctx = nvmpi_mjpeg_create_fd_decoder(avctx, nvmpi_context);
		if (nvmpi_context->ctx)
		{
			avctx->pix_fmt = AV_PIX_FMT_DRM_PRIME;
			nvmpi_context->drm_out = 1;
			ret = nvmpi_drm_init_hw_frames(avctx, AV_PIX_FMT_NV12);
			if (ret < 0)
				return ret; //.close (INIT_CLEANUP) destroys the decoder
			return 0;       //frames are built per decode, no bufFrame
		}
	}

	avctx->pix_fmt = AV_PIX_FMT_YUV420P;

	/* The software path needs libnvmpi's capacity-checked JPEG copy, which
	 * ships with get_frame2 (>= 4.6.0); refused otherwise (#137). */
	ret = nvmpi_dec_sw_check_support(avctx);
	if (ret < 0)
		return ret;

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
static int nvmpi_decode_mjpeg(AVCodecContext *avctx, AVFrame *data, int *got_frame, AVPacket *avpkt)
{
	nvmpiDecodeContext *nvmpi_context = avctx->priv_data;
	AVFrame *frame = data;
	nvFrame _nvframe = {0};
	nvPacket packet;
	int res, attempt;

	if (nvmpi_context->drm_out)
		return nvmpi_decode_mjpeg_fd(avctx, frame, got_frame, avpkt);

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

	/* Allocate or refresh bufFrame at the decoded resolution.
	 * JPEG dimensions come from the bitstream; we must update avctx once known.
	 * Also serves as re-acquisition guard: if a prior ff_get_buffer failure
	 * (during re-prime after av_frame_move_ref, or during resolution change)
	 * left bufFrame without backing store, this re-acquires it. */
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

	//libnvmpi copies the image only if it fits bufFrame's planes (their
	//capacity is passed in payload_size, #137). A larger image is dropped
	//with NVMPI_ERR_FRAME_SIZE and its size reported (no frame is held, unlike
	//the V4L2 decoders): re-allocate at that size and decode the packet again.
	for (attempt = 0; ; attempt++)
	{
		memset(&_nvframe, 0, sizeof(_nvframe));
		res = nvmpi_dec_sw_bind(avctx, bufFrame, &_nvframe);
		if (res < 0)
			return res;
		res = nvmpi_jpeg_decoder_get_frame(nvmpi_context->ctx, &_nvframe, 1);
		if (res != NVMPI_ERR_FRAME_SIZE)
			break;
		//Still refused after re-allocating at the reported size: the size
		//report is inconsistent, fail instead of looping.
		if (attempt)
		{
			av_log(avctx, AV_LOG_ERROR,
			       "nvmpi_jpeg_decoder_get_frame: image still does not fit at %dx%d.\n",
			       avctx->width, avctx->height);
			return AVERROR_EXTERNAL;
		}
		//Untrusted size from the bitstream: validated before avctx changes.
		res = nvmpi_dec_sw_set_size(avctx, _nvframe.width, _nvframe.height);
		if (res < 0)
			return res;
		res = nvmpi_dec_sw_alloc(avctx, bufFrame);
		if (res < 0)
			return res;
		res = nvmpi_jpeg_decoder_put_packet(nvmpi_context->ctx, &packet);
		if (res < 0)
		{
			av_log(avctx, AV_LOG_ERROR,
			       "nvmpi_jpeg_decoder_put_packet failed on resolution-change re-decode (code=%d).\n", res);
			return AVERROR_EXTERNAL;
		}
	}
	if (res == NVMPI_ERR_INVAL)
	{
		av_log(avctx, AV_LOG_ERROR, "nvmpi_jpeg_decoder_get_frame rejected its arguments.\n");
		return AVERROR_EXTERNAL;
	}
	if (res < 0)
	{
		*got_frame = 0;
		return avpkt->size;
	}

	//The image fit; it may be smaller than bufFrame: the output frame and
	//avctx carry the decoded size (JPEG is self-describing).
	res = nvmpi_dec_sw_set_size(avctx, _nvframe.width, _nvframe.height);
	if (res < 0)
		return res;
	bufFrame->width = (int)_nvframe.width;
	bufFrame->height = (int)_nvframe.height;

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
//options to video decoding. Identical to nvmpi_dec.c's table (see file
//header) — most entries are unused by the MJPEG decode path.
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

//MJPEG decoder — uses NvJPEGDecoder, not V4L2 M2M. Separate init/decode/
//close/flush callbacks, and only YUV420P output (no NV12/P010).
NVMPI_DEC_CLASS(mjpeg)
const FFCodec ff_mjpeg_nvmpi_decoder = {
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
	/* libavcodec 63 (FFmpeg 9.0+): pix_fmts moved from AVCodec to FFCodec */
	NVMPI_MJPEG_PIXFMTS,
	.p.wrapper_name   = "nvmpi",
};
