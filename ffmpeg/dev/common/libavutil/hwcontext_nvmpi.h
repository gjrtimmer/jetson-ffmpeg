/*
 * hwcontext_nvmpi.h — FFmpeg hardware device context for NVIDIA Jetson
 * NVMPI (V4L2 M2M) decode/encode via libnvmpi.
 *
 * This file is patched into a vanilla FFmpeg source tree (libavutil/)
 * by scripts/ffpatch.sh. It registers AV_HWDEVICE_TYPE_NVMPI so that
 * -hwaccel nvmpi works from the ffmpeg command line.
 *
 * No NVIDIA-specific headers are required — libnvmpi handles all V4L2
 * and DMA-BUF management internally. CUDA/EGL state is managed by
 * hwcontext_nvmpi.c using dlopen (no build-time CUDA dependency).
 */

#ifndef AVUTIL_HWCONTEXT_NVMPI_H
#define AVUTIL_HWCONTEXT_NVMPI_H

/**
 * NVMPI device context.
 *
 * Allocated as AVHWDeviceContext.hwctx. The struct holds optional
 * CUDA/EGL interop state for DRM_PRIME ↔ CUDA transfers — these
 * fields are populated lazily on first transfer request and cleaned
 * up in device_uninit.
 *
 * All pointer fields use void* to avoid requiring CUDA/EGL headers.
 */
typedef struct AVNVMPIDeviceContext {
    /**
     * CUDA interop state — lazy-initialized by nvmpi_cuda_init().
     * Set to 1 after successful CUDA + EGL initialization.
     * If CUDA libraries are unavailable, remains 0 and transfers
     * fall back to the DRM mmap path (CPU readback).
     */
    int cuda_initialized;

    /**
     * CUDA device ordinal (CUdevice). Always 0 on Jetson
     * (single integrated GPU). Set by nvmpi_cuda_init().
     */
    int cuda_device;

    /**
     * CUDA context (CUcontext, cast to void*).
     * Created in nvmpi_cuda_init(); destroyed in nvmpi_cuda_uninit().
     */
    void *cuda_ctx;

    /**
     * EGL display handle (EGLDisplay, cast to void*).
     * Initialized in nvmpi_cuda_init(); terminated in nvmpi_cuda_uninit().
     * Used by eglCreateImageKHR to import DMA-BUF fds.
     */
    void *egl_display;
} AVNVMPIDeviceContext;

#endif /* AVUTIL_HWCONTEXT_NVMPI_H */
