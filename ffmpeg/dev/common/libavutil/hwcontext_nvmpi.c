/*
 * hwcontext_nvmpi.c — FFmpeg hardware device context for NVIDIA Jetson
 * NVMPI (V4L2 M2M) decode/encode via libnvmpi.
 *
 * This file is patched into a vanilla FFmpeg source tree (libavutil/)
 * by scripts/ffpatch.sh. It registers AV_HWDEVICE_TYPE_NVMPI, enabling
 * -hwaccel nvmpi from the command line.
 *
 * Design: libnvmpi manages all V4L2 device and DMA-BUF buffer lifecycle.
 * This hwcontext provides:
 *   - device type registration (so -hwaccel nvmpi is recognized)
 *   - format constraints (DRM_PRIME hw format, NV12/YUV420P sw formats)
 *   - transfer format negotiation (for sw readback of hw frames)
 *   - DRM_PRIME -> CUDA transfer via EGL interop (zero CPU-copy)
 *
 * CUDA interop path:
 *   DMA-BUF fd (from AVDRMFrameDescriptor)
 *     -> eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT)
 *     -> cuGraphicsEGLRegisterImage -> cuGraphicsResourceGetMappedEglFrame
 *     -> cuMemcpy2D device-to-device (block-linear -> pitch-linear)
 *     -> AV_PIX_FMT_CUDA AVFrame
 *
 * EGL and CUDA libraries are loaded at runtime via dlopen (see
 * dynlink_nvmpi_cuda.h) — no build-time dependency on CUDA SDK.
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
#include "log.h"
#include "imgutils.h"
#include "pixdesc.h"

/* DRM_PRIME frame descriptor access */
#include "hwcontext_drm.h"

/* Runtime CUDA + EGL symbol loading — no build-time CUDA dependency */
#include "dynlink_nvmpi_cuda.h"

/* ------------------------------------------------------------------ */
/* CUDA interop state — stored in AVNVMPIDeviceContext                  */
/* ------------------------------------------------------------------ */

/*
 * Lazy-initialize CUDA + EGL on first transfer request.
 *
 * Creates an EGL display and CUDA context that persist for the lifetime
 * of the AVHWDeviceContext.  Thread safety: the FFmpeg hwcontext layer
 * serializes device_create and device_uninit, and transfer_data calls
 * are serialized by the caller (single decode thread).
 *
 * Returns 0 on success, AVERROR on failure.
 */
static int nvmpi_cuda_init(AVHWDeviceContext *ctx)
{
    AVNVMPIDeviceContext *hwctx = ctx->hwctx;
    nvmpi_CUdevice cu_dev;
    nvmpi_CUcontext dummy;
    nvmpi_EGLint egl_major, egl_minor;

    if (hwctx->cuda_initialized)
        return 0;

    /* Load CUDA + EGL shared libraries via dlopen */
    if (nvmpi_cuda_dynlink_load_all() < 0) {
        av_log(ctx, AV_LOG_ERROR, "nvmpi: failed to load CUDA/EGL "
               "libraries — CUDA interop unavailable\n");
        return AVERROR_EXTERNAL;
    }

    /* Initialize EGL display (headless, no surface needed).
     * Allocated here; terminated in nvmpi_cuda_uninit(). */
    hwctx->egl_display = nvmpi_eglGetDisplay(NVMPI_EGL_DEFAULT_DISPLAY);
    if (!hwctx->egl_display) {
        av_log(ctx, AV_LOG_ERROR, "nvmpi: eglGetDisplay failed\n");
        return AVERROR_EXTERNAL;
    }

    if (!nvmpi_eglInitialize(hwctx->egl_display, &egl_major, &egl_minor)) {
        av_log(ctx, AV_LOG_ERROR, "nvmpi: eglInitialize failed "
               "(error 0x%x)\n", nvmpi_eglGetError());
        hwctx->egl_display = NULL;
        return AVERROR_EXTERNAL;
    }
    av_log(ctx, AV_LOG_DEBUG, "nvmpi: EGL %d.%d initialized\n",
           egl_major, egl_minor);

    /* Initialize CUDA driver and create context on device 0
     * (Jetson has one integrated GPU). */
    if (nvmpi_cuInit(0) != NVMPI_CUDA_SUCCESS) {
        av_log(ctx, AV_LOG_ERROR, "nvmpi: cuInit failed\n");
        goto fail_egl;
    }

    if (nvmpi_cuDeviceGet(&cu_dev, 0) != NVMPI_CUDA_SUCCESS) {
        av_log(ctx, AV_LOG_ERROR, "nvmpi: cuDeviceGet(0) failed\n");
        goto fail_egl;
    }

    /* Flags=0: default scheduling, no special flags.
     * Created here; destroyed in nvmpi_cuda_uninit(). */
    if (nvmpi_cuCtxCreate(&hwctx->cuda_ctx, 0, cu_dev) != NVMPI_CUDA_SUCCESS) {
        av_log(ctx, AV_LOG_ERROR, "nvmpi: cuCtxCreate failed\n");
        goto fail_egl;
    }

    /* Pop context from thread-local stack — we push/pop around each
     * transfer to stay compatible with any other CUDA contexts. */
    nvmpi_cuCtxPopCurrent(&dummy);

    hwctx->cuda_device = cu_dev;
    hwctx->cuda_initialized = 1;
    av_log(ctx, AV_LOG_DEBUG, "nvmpi: CUDA context created on device %d\n",
           cu_dev);
    return 0;

fail_egl:
    /* Clean up EGL on CUDA init failure */
    nvmpi_eglTerminate(hwctx->egl_display);
    hwctx->egl_display = NULL;
    return AVERROR_EXTERNAL;
}

/*
 * Tear down CUDA context and EGL display.
 * Called from nvmpi_device_uninit when the hwdevice is freed.
 * Safe to call even if CUDA was never initialized.
 */
static void nvmpi_cuda_uninit(AVHWDeviceContext *ctx)
{
    AVNVMPIDeviceContext *hwctx = ctx->hwctx;

    if (!hwctx->cuda_initialized)
        return;

    /* Freed here; created in nvmpi_cuda_init() */
    if (hwctx->cuda_ctx) {
        nvmpi_cuCtxDestroy(hwctx->cuda_ctx);
        hwctx->cuda_ctx = NULL;
    }

    /* Freed here; created in nvmpi_cuda_init() */
    if (hwctx->egl_display) {
        nvmpi_eglTerminate(hwctx->egl_display);
        hwctx->egl_display = NULL;
    }

    hwctx->cuda_initialized = 0;
}

/* ------------------------------------------------------------------ */
/* DRM_PRIME -> CUDA transfer                                          */
/* ------------------------------------------------------------------ */

/*
 * Transfer one DRM_PRIME frame to a CUDA device allocation.
 *
 * Steps:
 *   1. Extract DMA-BUF fd + geometry from AVDRMFrameDescriptor
 *   2. Create EGLImageKHR from DMA-BUF fd (EGL_LINUX_DMA_BUF_EXT)
 *   3. Register EGL image with CUDA -> CUgraphicsResource
 *   4. Map -> CUeglFrame (plane device pointers)
 *   5. cuMemcpy2D device->device per plane into dst AVFrame
 *   6. Unregister + destroy EGL image (per-frame, not cached —
 *      the DMA-BUF fd changes each frame)
 *
 * dst must be a CUDA AVFrame with data[0] = Y plane CUdeviceptr,
 * data[1] = UV plane CUdeviceptr (NV12), allocated by caller.
 */
static int nvmpi_transfer_drm_to_cuda(AVHWFramesContext *hwfc,
                                       AVFrame *dst, const AVFrame *src)
{
    AVHWDeviceContext *dev_ctx = hwfc->device_ctx;
    AVNVMPIDeviceContext *hwctx = dev_ctx->hwctx;
    const AVDRMFrameDescriptor *drm_desc;
    nvmpi_EGLImageKHR egl_image = NVMPI_EGL_NO_IMAGE_KHR;
    nvmpi_CUgraphicsResource cu_res = NULL;
    nvmpi_CUeglFrame egl_frame;
    nvmpi_CUcontext dummy;
    nvmpi_CUresult cu_err;
    int ret = 0;
    int fd, width, height, stride;
    unsigned int p;

    /* Validate DRM frame descriptor from decoder */
    drm_desc = (const AVDRMFrameDescriptor *)src->data[0];
    if (!drm_desc || drm_desc->nb_objects < 1) {
        av_log(dev_ctx, AV_LOG_ERROR, "nvmpi: invalid DRM frame descriptor\n");
        return AVERROR(EINVAL);
    }

    /* Step 1: Extract DMA-BUF geometry.
     * For NV12: single DMA-BUF object, two layers (Y + UV). */
    fd     = drm_desc->objects[0].fd;
    width  = src->width;
    height = src->height;

    /* Compute stride from first layer, first plane */
    stride = 0;
    if (drm_desc->nb_layers > 0 && drm_desc->layers[0].nb_planes > 0)
        stride = drm_desc->layers[0].planes[0].pitch;
    if (stride <= 0)
        stride = width;  /* fallback */

    /* EGL attrib list for DMA-BUF import.
     * Plane 0 = Y, Plane 1 = UV (at offset stride * height for NV12). */
    {
        nvmpi_EGLint attribs[] = {
            NVMPI_EGL_WIDTH,                    width,
            NVMPI_EGL_HEIGHT,                   height,
            NVMPI_EGL_LINUX_DRM_FOURCC_EXT,     NVMPI_DRM_FORMAT_NV12,
            NVMPI_EGL_DMA_BUF_PLANE0_FD_EXT,   fd,
            NVMPI_EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
            NVMPI_EGL_DMA_BUF_PLANE0_PITCH_EXT,  stride,
            /* UV plane (NV12: interleaved UV at offset = stride * height) */
            NVMPI_EGL_DMA_BUF_PLANE1_FD_EXT,   fd,
            NVMPI_EGL_DMA_BUF_PLANE1_OFFSET_EXT, stride * height,
            NVMPI_EGL_DMA_BUF_PLANE1_PITCH_EXT,  stride,
            NVMPI_EGL_NONE
        };

        /* Step 2: Create EGL image from DMA-BUF.
         * Created here; destroyed in cleanup below. */
        egl_image = nvmpi_eglCreateImageKHR(hwctx->egl_display,
                                             NVMPI_EGL_NO_CONTEXT,
                                             NVMPI_EGL_LINUX_DMA_BUF_EXT,
                                             NULL, attribs);
    }

    if (egl_image == NVMPI_EGL_NO_IMAGE_KHR) {
        av_log(dev_ctx, AV_LOG_ERROR, "nvmpi: eglCreateImageKHR failed "
               "(error 0x%x) for %dx%d fd=%d\n",
               nvmpi_eglGetError(), width, height, fd);
        return AVERROR_EXTERNAL;
    }

    /* Push CUDA context for this thread */
    nvmpi_cuCtxPushCurrent(hwctx->cuda_ctx);

    /* Step 3: Register EGL image with CUDA.
     * Created here; unregistered in cleanup below. */
    cu_err = nvmpi_cuGraphicsEGLRegisterImage(
        &cu_res, egl_image, NVMPI_CU_GRAPHICS_REGISTER_FLAGS_READ_ONLY);
    if (cu_err != NVMPI_CUDA_SUCCESS) {
        av_log(dev_ctx, AV_LOG_ERROR, "nvmpi: cuGraphicsEGLRegisterImage "
               "failed (CUresult %d)\n", cu_err);
        ret = AVERROR_EXTERNAL;
        goto cleanup;
    }

    /* Step 4: Get mapped EGL frame with CUDA device pointers */
    cu_err = nvmpi_cuGraphicsResourceGetMappedEglFrame(
        &egl_frame, cu_res, 0, 0);
    if (cu_err != NVMPI_CUDA_SUCCESS) {
        av_log(dev_ctx, AV_LOG_ERROR, "nvmpi: "
               "cuGraphicsResourceGetMappedEglFrame failed "
               "(CUresult %d)\n", cu_err);
        ret = AVERROR_EXTERNAL;
        goto cleanup;
    }

    /* Verify we got pitch-linear planes (not CUDA arrays) */
    if (egl_frame.frameType != NVMPI_CU_EGL_FRAME_TYPE_PITCH) {
        av_log(dev_ctx, AV_LOG_ERROR, "nvmpi: EGL frame is array type, "
               "expected pitch (type=%d)\n", egl_frame.frameType);
        ret = AVERROR_EXTERNAL;
        goto cleanup;
    }

    /* Step 5: cuMemcpy2D per plane — device-to-device copy from
     * EGL-mapped block-linear surface to pitch-linear CUDA allocation.
     *
     * For NV12: plane 0 = Y (width x height), plane 1 = UV (width x height/2)
     * dst->data[0] = Y CUdeviceptr, dst->data[1] = UV CUdeviceptr
     * dst->linesize[0] = Y pitch, dst->linesize[1] = UV pitch */
    for (p = 0; p < egl_frame.planeCount && p < 2; p++) {
        nvmpi_CUDA_MEMCPY2D cpy;
        /* Zero-init prevents stale array/host fields from confusing
         * the CUDA driver — only device fields are used. */
        memset(&cpy, 0, sizeof(cpy));

        /* Source: EGL-mapped device pointer (block-linear DMA-BUF) */
        cpy.srcMemoryType = NVMPI_CU_MEMORYTYPE_DEVICE;
        cpy.srcDevice     = (nvmpi_CUdeviceptr)egl_frame.frame.pPitch[p];
        cpy.srcPitch      = egl_frame.pitch;

        /* Destination: caller-allocated pitch-linear CUDA buffer */
        cpy.dstMemoryType = NVMPI_CU_MEMORYTYPE_DEVICE;
        cpy.dstDevice     = (nvmpi_CUdeviceptr)dst->data[p];
        cpy.dstPitch      = dst->linesize[p];

        /* NV12: Y plane is full height, UV plane is half height */
        cpy.WidthInBytes  = width;
        cpy.Height        = (p == 0) ? (unsigned int)height
                                     : (unsigned int)height / 2;

        cu_err = nvmpi_cuMemcpy2D(&cpy);
        if (cu_err != NVMPI_CUDA_SUCCESS) {
            av_log(dev_ctx, AV_LOG_ERROR, "nvmpi: cuMemcpy2D failed "
                   "for plane %u (CUresult %d)\n", p, cu_err);
            ret = AVERROR_EXTERNAL;
            goto cleanup;
        }
    }

cleanup:
    /* Step 6: Unregister and destroy — per-frame, not cached.
     * Different DMA-BUF fds each frame require fresh EGL images. */
    if (cu_res) {
        /* Freed here; allocated by cuGraphicsEGLRegisterImage above */
        nvmpi_cuGraphicsUnregisterResource(cu_res);
    }
    nvmpi_cuCtxPopCurrent(&dummy);

    if (egl_image != NVMPI_EGL_NO_IMAGE_KHR) {
        /* Freed here; allocated by eglCreateImageKHR above */
        nvmpi_eglDestroyImageKHR(hwctx->egl_display, egl_image);
    }

    return ret;
}

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
 *
 * CUDA/EGL init is deferred to first transfer_data call to avoid
 * loading CUDA libraries when only software readback is used.
 */
static int nvmpi_device_create(AVHWDeviceContext *ctx, const char *device,
                               AVDictionary *opts, int flags)
{
    AVNVMPIDeviceContext *hwctx = ctx->hwctx;
    /* Zero all fields — CUDA init is deferred to first transfer */
    memset(hwctx, 0, sizeof(*hwctx));
    return 0;
}

/**
 * Clean up CUDA context and EGL display on device destruction.
 * Safe to call even if CUDA was never initialized.
 */
static void nvmpi_device_uninit(AVHWDeviceContext *ctx)
{
    nvmpi_cuda_uninit(ctx);
}

/* ------------------------------------------------------------------ */
/* Frame constraints                                                   */
/* ------------------------------------------------------------------ */

/**
 * Report supported formats and resolution limits.
 *
 * Hardware format: AV_PIX_FMT_DRM_PRIME (DMA-BUF fd wrapped in
 * AVDRMFrameDescriptor). Software formats: NV12 (Jetson's native hw
 * layout) and YUV420P (VIC-converted). Min resolution 16x16 matches
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
 * Report formats available for hw<->sw transfer.
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
/* Frame data transfer                                                 */
/* ------------------------------------------------------------------ */

/**
 * Transfer data FROM an NVMPI hardware frame (DRM_PRIME) to dst.
 *
 * If dst is a CUDA frame (has hw_frames_ctx with AV_PIX_FMT_CUDA):
 *   -> EGL interop path (device-to-device, no CPU copy)
 *
 * Otherwise: returns ENOSYS to fall through to the DRM hwcontext's
 * mmap-based sw transfer (which handles DRM_PRIME -> CPU natively).
 */
static int nvmpi_transfer_data_from(AVHWFramesContext *ctx,
                                    AVFrame *dst, const AVFrame *src)
{
    /* Only handle CUDA destination — sw fallback handled by
     * the DRM hwcontext layer or av_hwframe_transfer_data's
     * generic mmap path. */
    if (dst->hw_frames_ctx) {
        AVHWFramesContext *dst_fc =
            (AVHWFramesContext *)dst->hw_frames_ctx->data;
        if (dst_fc->device_ctx->type == AV_HWDEVICE_TYPE_CUDA) {
            /* Lazy-init CUDA + EGL on first transfer */
            int err = nvmpi_cuda_init(ctx->device_ctx);
            if (err < 0)
                return err;
            return nvmpi_transfer_drm_to_cuda(ctx, dst, src);
        }
    }

    /* Not a CUDA destination — let FFmpeg try other paths */
    return AVERROR(ENOSYS);
}

/* ------------------------------------------------------------------ */
/* HWContextType registration                                          */
/* ------------------------------------------------------------------ */

const HWContextType ff_hwcontext_type_nvmpi = {
    .type                   = AV_HWDEVICE_TYPE_NVMPI,
    .name                   = "NVMPI",

    .device_hwctx_size      = sizeof(AVNVMPIDeviceContext),

    .device_create          = nvmpi_device_create,
    .device_uninit          = nvmpi_device_uninit,
    .frames_get_constraints = nvmpi_frames_get_constraints,
    .transfer_get_formats   = nvmpi_transfer_get_formats,
    .transfer_data_from     = nvmpi_transfer_data_from,

    .pix_fmts = (const enum AVPixelFormat[]) {
        AV_PIX_FMT_DRM_PRIME,
        AV_PIX_FMT_NONE,
    },
};
