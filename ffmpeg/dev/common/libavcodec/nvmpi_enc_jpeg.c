/*
 * nvmpi_enc_jpeg.c — FFmpeg MJPEG encoder wrapper for libnvmpi JPEG encoder.
 *
 * This is a SEPARATE file from nvmpi_enc.c because the JPEG encoder is
 * fundamentally different from the V4L2-based H.264/HEVC encoders:
 *   - Synchronous per-frame (no packet pool, no DQ thread, no DELAY cap)
 *   - No bitrate/GOP/profile/level options — only quality
 *   - Uses NvJPEGEncoder, not NvVideoEncoder
 *
 * One source supports FFmpeg 6.0 .. 8.0+ via preprocessor guards:
 *   - LIBAVCODEC_VERSION_MAJOR >= 60: FFCodec + FF_CODEC_ENCODE_CB
 *
 * Quality mapping: FFmpeg's global_quality (from -q:v) is in
 * FF_QP2LAMBDA units. We convert: libjpeg_quality = 2 + (31 - qscale) * 98 / 30
 * where qscale = global_quality / FF_QP2LAMBDA, clamped to [1, 31].
 * -q:v 1 (best) → quality ~100; -q:v 31 (worst) → quality ~2.
 * If no -q:v is set, default quality is 85.
 */
#include <nvmpi.h>
#include "avcodec.h"
#include "internal.h"
#include <stdio.h>
#include "libavutil/avutil.h"
#include "libavutil/common.h"
#include "libavutil/imgutils.h"
#include "libavutil/log.h"
#include "libavutil/opt.h"
#include "libavutil/mem.h"

#include "version.h"

#include "encode.h"
#include "codec_internal.h"

/* Private encoder context — wraps the opaque libnvmpi JPEG encoder handle. */
typedef struct {
    AVClass *avclass;
    nvmpictx *ctx;
    /* Reusable output buffer for encoded JPEG data. Sized to worst-case
     * (uncompressed YUV420 + JPEG overhead). Allocated lazily on first encode. */
    uint8_t *pkt_buf;
    int pkt_buf_size;
} nvmpiJpegEncContext;

/* ------------------------------------------------------------------ */
/* Quality mapping                                                     */
/* ------------------------------------------------------------------ */

/*
 * Convert FFmpeg quality scale to libjpeg quality 1-100.
 *
 * FFmpeg's -q:v sets global_quality in FF_QP2LAMBDA units.
 * Standard MJPEG encoders use qscale 1 (best) to 31 (worst).
 * We linearly map: qscale 1 → quality 100, qscale 31 → quality 2.
 */
static int qscale_to_jpeg_quality(int global_quality)
{
    int qscale;

    if (global_quality <= 0)
        return 85; /* No -q:v specified — use default. */

    qscale = global_quality / FF_QP2LAMBDA;
    if (qscale < 1) qscale = 1;
    if (qscale > 31) qscale = 31;

    /* Linear map: qscale 1 → 100, qscale 31 → 2 */
    return 2 + (31 - qscale) * 98 / 30;
}

/* ------------------------------------------------------------------ */
/* AVCodec callbacks                                                   */
/* ------------------------------------------------------------------ */

static av_cold int nvmpi_jpegenc_init(AVCodecContext *avctx)
{
    nvmpiJpegEncContext *s = avctx->priv_data;
    int quality;

    avctx->pix_fmt = AV_PIX_FMT_YUV420P;
    avctx->color_range = AVCOL_RANGE_JPEG;

    quality = qscale_to_jpeg_quality(avctx->global_quality);

    s->ctx = nvmpi_create_jpeg_encoder(quality);
    if (!s->ctx) {
        av_log(avctx, AV_LOG_ERROR,
               "Failed to create JPEG hardware encoder. This Jetson module "
               "may not have NVJPG encode capability (e.g. Orin Nano). "
               "Use -c:v mjpeg for software JPEG encoding.\n");
        return AVERROR_EXTERNAL;
    }

    s->pkt_buf = NULL;
    s->pkt_buf_size = 0;

    return 0;
}

static av_cold int nvmpi_jpegenc_close(AVCodecContext *avctx)
{
    nvmpiJpegEncContext *s = avctx->priv_data;

    if (s->ctx) {
        nvmpi_jpeg_encoder_close(s->ctx);
        s->ctx = NULL;
    }

    av_freep(&s->pkt_buf);
    s->pkt_buf_size = 0;

    return 0;
}

/*
 * Ensure pkt_buf is large enough for worst-case JPEG output.
 * Worst case: uncompressed YUV420 is w*h*1.5 bytes; JPEG with
 * quality 100 can be larger than input due to headers. We use
 * w*h*2 + 65536 as a safe upper bound.
 */
static int ensure_pkt_buf(nvmpiJpegEncContext *s, int width, int height)
{
    /* Integer overflow protection: check multiplication before allocating.
     * Width and height are validated against NVJPEG_ENC_MAX_DIM (16384) by
     * libnvmpi, so the product fits in int64_t safely. */
    int64_t needed = (int64_t)width * height * 2 + 65536;
    if (needed > INT_MAX)
        return AVERROR(EINVAL);

    if (s->pkt_buf_size >= (int)needed)
        return 0;

    av_freep(&s->pkt_buf);
    s->pkt_buf = av_malloc(needed);
    if (!s->pkt_buf) {
        s->pkt_buf_size = 0;
        return AVERROR(ENOMEM);
    }
    s->pkt_buf_size = (int)needed;
    return 0;
}

/*
 * Encode one frame. JPEG encoding is synchronous — one frame in,
 * one packet out, no pipeline delay.
 */
static int nvmpi_jpegenc_encode(AVCodecContext *avctx, AVPacket *avpkt,
                                const AVFrame *frame, int *got_packet)
{
    nvmpiJpegEncContext *s = avctx->priv_data;
    nvFrame nv_frame = {0};
    nvPacket nv_packet = {0};
    int ret;

    *got_packet = 0;

    if (!frame) {
        /* Flush / EOS — JPEG has no pipeline delay, nothing to drain. */
        return 0;
    }

    /* Build nvFrame from AVFrame planes. */
    nv_frame.payload[0] = frame->data[0];
    nv_frame.payload[1] = frame->data[1];
    nv_frame.payload[2] = frame->data[2];
    nv_frame.linesize[0] = frame->linesize[0];
    nv_frame.linesize[1] = frame->linesize[1];
    nv_frame.linesize[2] = frame->linesize[2];
    nv_frame.width = frame->width;
    nv_frame.height = frame->height;
    nv_frame.timestamp = frame->pts;
    nv_frame.type = NV_PIX_YUV420;

    ret = nvmpi_jpeg_encoder_put_frame(s->ctx, &nv_frame);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "nvmpi_jpeg_encoder_put_frame failed.\n");
        return AVERROR_EXTERNAL;
    }

    /* Ensure output buffer is large enough. */
    ret = ensure_pkt_buf(s, frame->width, frame->height);
    if (ret < 0)
        return ret;

    nv_packet.payload = s->pkt_buf;
    nv_packet.payload_size = 0;

    ret = nvmpi_jpeg_encoder_get_packet(s->ctx, &nv_packet);
    if (ret < 0) {
        if (ret == -2) return 0; /* EOS, no more packets. */
        return 0; /* No packet ready — shouldn't happen for sync encode. */
    }

    /* Allocate the AVPacket data buffer. */
    ret = ff_get_encode_buffer(avctx, avpkt, nv_packet.payload_size, 0);
    if (ret < 0)
        return ret;

    memcpy(avpkt->data, nv_packet.payload, nv_packet.payload_size);
    avpkt->size = nv_packet.payload_size;

    /* JPEG frames are always keyframes (intra-only codec). */
    avpkt->flags |= AV_PKT_FLAG_KEY;
    avpkt->pts = frame->pts;
    avpkt->dts = frame->pts;

    *got_packet = 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* AVOption table                                                      */
/* ------------------------------------------------------------------ */

/* JPEG encoder has minimal options — quality is driven by -q:v.
 * We expose no private options for now. */
static const AVOption options[] = {
    { NULL }
};

static const AVClass nvmpi_mjpeg_enc_class = {
    .class_name = "mjpeg_nvmpi_encoder",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

/* ------------------------------------------------------------------ */
/* Codec registration                                                  */
/* ------------------------------------------------------------------ */

/* JPEG is intra-only (no pipeline delay), so no AV_CODEC_CAP_DELAY.
 * Uses FF_CODEC_ENCODE_CB (synchronous encode callback, libavcodec 60+). */
const FFCodec ff_mjpeg_nvmpi_encoder = {
    .p.name           = "mjpeg_nvmpi",
    CODEC_LONG_NAME("mjpeg (nvmpi)"),
    .p.type           = AVMEDIA_TYPE_VIDEO,
    .p.id             = AV_CODEC_ID_MJPEG,
    .priv_data_size = sizeof(nvmpiJpegEncContext),
    .p.priv_class     = &nvmpi_mjpeg_enc_class,
    .init           = nvmpi_jpegenc_init,
    FF_CODEC_ENCODE_CB(nvmpi_jpegenc_encode),
    .close          = nvmpi_jpegenc_close,
    .p.pix_fmts       = (const enum AVPixelFormat[]) {
        AV_PIX_FMT_YUV420P,
        AV_PIX_FMT_NONE
    },
    .p.capabilities   = AV_CODEC_CAP_HARDWARE,
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,
    .p.wrapper_name   = "nvmpi",
};
