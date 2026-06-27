/*
 * hwcontext_nvmpi.c — FFmpeg hardware device context for NVIDIA Jetson
 * NVMPI (V4L2 M2M) decode/encode via libnvmpi.
 *
 * This file is patched into a vanilla FFmpeg source tree (libavutil/)
 * by scripts/ffpatch.sh. It registers AV_HWDEVICE_TYPE_NVMPI, enabling
 * -hwaccel nvmpi from the command line.
 *
 * Design: intentionally thin. libnvmpi manages all V4L2 device and
 * DMA-BUF buffer lifecycle — this hwcontext only provides:
 *   - device type registration (so -hwaccel nvmpi is recognized)
 *   - format constraints (DRM_PRIME hw format, NV12/YUV420P sw formats)
 *   - transfer format negotiation (for sw readback of hw frames)
 *
 * The decoder/encoder codec wrappers (nvmpi_dec.c / nvmpi_enc.c) check
 * for hw_device_ctx type NVMPI and auto-select DRM_PRIME output/input.
 */

#include "config.h"
#include "hwcontext.h"
#include "hwcontext_internal.h"
#include "hwcontext_nvmpi.h"
#include "pixfmt.h"
#include "mem.h"

/* ------------------------------------------------------------------ */
/* Device context                                                      */
/* ------------------------------------------------------------------ */

/**
 * Create an NVMPI hardware device context.
 *
 * Succeeds unconditionally — V4L2 device lifecycle is managed inside
 * libnvmpi (per-codec, not per-device-context). The @p device parameter
 * is currently ignored; a future extension could use it to select a
 * specific V4L2 M2M device node (e.g. "/dev/nvhost-nvdec").
 */
static int nvmpi_device_create(AVHWDeviceContext *ctx, const char *device,
                               AVDictionary *opts, int flags)
{
    /* No-op: libnvmpi opens /dev/nvhost-nvdec and /dev/nvhost-msenc
     * internally when a decoder/encoder is created. The hwdevice ctx
     * exists purely as a type marker for format negotiation. */
    return 0;
}

/* ------------------------------------------------------------------ */
/* Frame constraints                                                   */
/* ------------------------------------------------------------------ */

/**
 * Report supported formats and resolution limits.
 *
 * Hardware format: AV_PIX_FMT_DRM_PRIME (DMA-BUF fd wrapped in
 * AVDRMFrameDescriptor). Software formats: NV12 (Jetson's native hw
 * layout) and YUV420P (VIC-converted). Min resolution 16×16 matches
 * the NVDEC hardware minimum block size.
 */
static int nvmpi_frames_get_constraints(AVHWDeviceContext *hwdev,
                                        const void *hwconfig,
                                        AVHWFramesConstraints *constraints)
{
    constraints->min_width  = 16;
    constraints->min_height = 16;

    /* Allocated here; freed by av_hwframe_constraints_free in libavutil */
    constraints->valid_hw_formats =
        av_malloc_array(2, sizeof(enum AVPixelFormat));
    if (!constraints->valid_hw_formats)
        return AVERROR(ENOMEM);
    constraints->valid_hw_formats[0] = AV_PIX_FMT_DRM_PRIME;
    constraints->valid_hw_formats[1] = AV_PIX_FMT_NONE;

    /* Allocated here; freed by av_hwframe_constraints_free in libavutil */
    constraints->valid_sw_formats =
        av_malloc_array(3, sizeof(enum AVPixelFormat));
    if (!constraints->valid_sw_formats)
        return AVERROR(ENOMEM);
    constraints->valid_sw_formats[0] = AV_PIX_FMT_NV12;
    constraints->valid_sw_formats[1] = AV_PIX_FMT_YUV420P;
    constraints->valid_sw_formats[2] = AV_PIX_FMT_NONE;

    return 0;
}

/* ------------------------------------------------------------------ */
/* Transfer format negotiation                                         */
/* ------------------------------------------------------------------ */

/**
 * Report formats available for hw↔sw transfer.
 *
 * Returns ctx->sw_format — the caller-configured software layout.
 * Actual DMA-BUF mmap transfer is handled by the DRM hwcontext's
 * map_from path (DRM_PRIME frames are interoperable with the DRM
 * hwcontext transfer machinery).
 */
static int nvmpi_transfer_get_formats(AVHWFramesContext *ctx,
                                      enum AVHWFrameTransferDirection dir,
                                      enum AVPixelFormat **formats)
{
    /* Allocated here; freed by caller (av_hwframe_transfer_get_formats) */
    enum AVPixelFormat *fmts = av_malloc_array(2, sizeof(*fmts));
    if (!fmts)
        return AVERROR(ENOMEM);
    fmts[0] = ctx->sw_format;
    fmts[1] = AV_PIX_FMT_NONE;
    *formats = fmts;
    return 0;
}

/* ------------------------------------------------------------------ */
/* HWContextType registration                                          */
/* ------------------------------------------------------------------ */

const HWContextType ff_hwcontext_type_nvmpi = {
    .type                   = AV_HWDEVICE_TYPE_NVMPI,
    .name                   = "NVMPI",

    .device_hwctx_size      = sizeof(AVNVMPIDeviceContext),

    .device_create          = nvmpi_device_create,
    .frames_get_constraints = nvmpi_frames_get_constraints,
    .transfer_get_formats   = nvmpi_transfer_get_formats,

    .pix_fmts = (const enum AVPixelFormat[]) {
        AV_PIX_FMT_DRM_PRIME,
        AV_PIX_FMT_NONE,
    },
};
