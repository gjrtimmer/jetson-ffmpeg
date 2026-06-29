/*
 * nvmpi_vic.cpp — standalone VIC hardware transform API.
 *
 * Exposes the Tegra VIC (Video Image Compositor) engine through a simple
 * C API for hardware-accelerated scale and color-space conversion on
 * DMA-BUF surfaces.  This is the same VIC engine the decoder capture loop
 * uses internally (nvmpi_dec_capture.cpp), but wrapped as a standalone
 * service for use by external consumers like the FFmpeg scale_vic filter.
 *
 * Lifecycle:
 *   nvmpi_vic_create   → allocate context, bind VIC compute session
 *   nvmpi_vic_transform → NvBufSurfTransform on two DMA-BUF fds
 *   nvmpi_vic_close     → destroy context
 *
 * Buffer allocation/destruction uses the existing nvmpi_surface_alloc /
 * nvmpi_surface_destroy API (nvmpi_surface.cpp) — not duplicated here.
 *
 * Supports both buffer API generations:
 *   - NvUtils/NvBufSurface (WITH_NVUTILS, JetPack 5+)
 *   - Legacy nvbuf_utils (JetPack 4)
 */
#include "nvmpi.h"
#include "nvUtils2NvBuf.h"
#include "nvmpi_log.h"
#include <string.h>
#include <stdlib.h>

/* Internal VIC context — opaque to callers via the nvmpi_vic_ctx typedef.
 * Allocated in nvmpi_vic_create(); freed in nvmpi_vic_close(). */
struct nvmpi_vic_ctx {
#ifdef WITH_NVUTILS
    /* NvUtils API: per-thread session binding for VIC engine */
    NvBufSurfTransformConfigParams session;
#else
    /* Legacy API: session handle from NvBufferSessionCreate */
    NvBufferSession session;
#endif
    /* Cached transform parameters — rebuilt on each transform call
     * since source/destination dimensions may vary per call. */
    NvBufferTransformParams transform_params;
    NvBufferRect src_rect;
    NvBufferRect dest_rect;
    int initialized;
};

/* Allocated here; freed in nvmpi_vic_close() */
nvmpi_vic_ctx *nvmpi_vic_create(void)
{
    nvmpi_vic_ctx *ctx = (nvmpi_vic_ctx *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        NVMPI_LOG(NVMPI_LOG_ERROR,
                  "nvmpi_vic_create: failed to allocate context");
        return NULL;
    }

    /* Use GPU (CUDA) compute instead of VIC for standalone transforms.
     * The decoder's capture thread also uses NvBufSurfTransform with VIC
     * compute mode (nvmpi_dec_capture.cpp); concurrent VIC submissions
     * from two threads cause the Tegra driver to deadlock.  Using GPU
     * compute for the filter avoids VIC hardware contention while still
     * leveraging hardware-accelerated scaling via the iGPU. */
#ifdef WITH_NVUTILS
    ctx->session.compute_mode = NvBufSurfTransformCompute_GPU;
    ctx->session.gpu_id = 0;
    ctx->session.cuda_stream = 0;
    int ret = NvBufSurfTransformSetSessionParams(&ctx->session);
    if (ret != 0) {
        NVMPI_LOG(NVMPI_LOG_ERROR,
                  "nvmpi_vic_create: NvBufSurfTransformSetSessionParams failed");
        free(ctx);
        return NULL;
    }
#else
    ctx->session = NvBufferSessionCreate();
    if (!ctx->session) {
        NVMPI_LOG(NVMPI_LOG_ERROR,
                  "nvmpi_vic_create: NvBufferSessionCreate failed");
        free(ctx);
        return NULL;
    }
#endif

    ctx->initialized = 1;
    return ctx;
}

/*
 * Perform a VIC hardware transform (scale + optional CSC) between two
 * DMA-BUF surfaces.  Both src_fd and dst_fd must be valid DMA-BUF file
 * descriptors allocated via nvmpi_surface_alloc or the decoder's internal
 * buffer pool.  The VIC engine handles block-linear ↔ pitch-linear
 * conversion, scaling, and NV12 ↔ NV12 pass-through in hardware.
 *
 * Returns 0 on success, -1 on error.
 */
int nvmpi_vic_transform(nvmpi_vic_ctx *ctx,
    int src_fd, int dst_fd,
    unsigned int src_w, unsigned int src_h,
    unsigned int dst_w, unsigned int dst_h)
{
    if (!ctx || !ctx->initialized) {
        NVMPI_LOG(NVMPI_LOG_ERROR,
                  "nvmpi_vic_transform: NULL or uninitialized context");
        return -1;
    }

    /* Validate fd arguments — negative fds would cause driver errors */
    if (src_fd < 0 || dst_fd < 0) {
        NVMPI_LOG(NVMPI_LOG_ERROR,
                  "nvmpi_vic_transform: invalid fd (src=%d, dst=%d)",
                  src_fd, dst_fd);
        return -1;
    }

    /* Validate dimensions — zero or oversized values cause driver hangs
     * or silent failures.  Upper bound matches Tegra hw maximum. */
    if (src_w == 0 || src_h == 0 || src_w > 8192 || src_h > 8192 ||
        dst_w == 0 || dst_h == 0 || dst_w > 8192 || dst_h > 8192) {
        NVMPI_LOG(NVMPI_LOG_ERROR,
                  "nvmpi_vic_transform: invalid dimensions src=%ux%u dst=%ux%u",
                  src_w, src_h, dst_w, dst_h);
        return -1;
    }

    /* Build source rect — full source surface */
    ctx->src_rect.top    = 0;
    ctx->src_rect.left   = 0;
    ctx->src_rect.width  = src_w;
    ctx->src_rect.height = src_h;

    /* Build destination rect — full destination surface */
    ctx->dest_rect.top    = 0;
    ctx->dest_rect.left   = 0;
    ctx->dest_rect.width  = dst_w;
    ctx->dest_rect.height = dst_h;

    /* Configure transform: bilinear filtering (Algo3 = high quality),
     * no flip/rotation.  Same settings as the decoder capture loop
     * (nvmpi_dec_api.cpp:updateBufferTransformParams). */
    memset(&ctx->transform_params, 0, sizeof(ctx->transform_params));
    ctx->transform_params.transform_flag   = NVBUFFER_TRANSFORM_FILTER;
    ctx->transform_params.transform_flip   = NvBufferTransform_None;
    ctx->transform_params.transform_filter = NvBufferTransform_Filter_Smart;

#ifdef WITH_NVUTILS
    ctx->transform_params.src_rect = &ctx->src_rect;
    ctx->transform_params.dst_rect = &ctx->dest_rect;
#else
    ctx->transform_params.src_rect  = ctx->src_rect;
    ctx->transform_params.dst_rect  = ctx->dest_rect;
    ctx->transform_params.session   = ctx->session;
#endif

    /* Execute the hardware transform */
    int ret;
#ifdef WITH_NVUTILS
    /* Re-set session params before each transform — ensures this thread's
     * session binding is current even if NvBufSurfTransformSetSessionParams
     * was called on a different thread (e.g. decoder capture thread)
     * between nvmpi_vic_create and this call. */
    NvBufSurfTransformSetSessionParams(&ctx->session);

    /* Resolve NvBufSurface pointers from the fd pair — NvBufSurfTransform
     * requires NvBufSurface*, not raw fds.  The surface structs are
     * transient (stack-local); the underlying DMA buffers are not copied. */
    NvBufSurface *src_surface = NULL;
    NvBufSurface *dst_surface = NULL;

    ret = NvBufSurfaceFromFd(src_fd, (void **)&src_surface);
    if (ret != 0 || !src_surface) {
        NVMPI_LOG(NVMPI_LOG_ERROR,
                  "nvmpi_vic_transform: NvBufSurfaceFromFd failed for src fd=%d",
                  src_fd);
        return -1;
    }

    ret = NvBufSurfaceFromFd(dst_fd, (void **)&dst_surface);
    if (ret != 0 || !dst_surface) {
        NVMPI_LOG(NVMPI_LOG_ERROR,
                  "nvmpi_vic_transform: NvBufSurfaceFromFd failed for dst fd=%d",
                  dst_fd);
        return -1;
    }

    ret = NvBufSurfTransform(src_surface, dst_surface,
                             &ctx->transform_params);
#else
    ret = NvBufferTransform(src_fd, dst_fd, &ctx->transform_params);
#endif

    if (ret != 0) {
        NVMPI_LOG(NVMPI_LOG_ERROR,
                  "nvmpi_vic_transform: transform failed (%ux%u → %ux%u, ret=%d)",
                  src_w, src_h, dst_w, dst_h, ret);
        return -1;
    }

    return 0;
}

/* Freed here; allocated in nvmpi_vic_create() above */
void nvmpi_vic_close(nvmpi_vic_ctx *ctx)
{
    if (!ctx) return;

#ifndef WITH_NVUTILS
    /* Legacy API: destroy the session handle */
    if (ctx->session) {
        NvBufferSessionDestroy(ctx->session);
        ctx->session = NULL;
    }
#endif

    /* Zero-out context before freeing — prevent stale pointer use */
    memset(ctx, 0, sizeof(*ctx));
    free(ctx);
}
