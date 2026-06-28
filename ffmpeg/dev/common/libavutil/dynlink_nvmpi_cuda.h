/*
 * dynlink_nvmpi_cuda.h -- Runtime lazy-loading of CUDA + EGL for
 *                         DRM_PRIME ↔ CUDA interop on NVIDIA Jetson.
 *
 * Used by hwcontext_nvmpi.c (libavutil) to transfer frames between
 * AV_PIX_FMT_DRM_PRIME and AV_PIX_FMT_CUDA without CPU copies.
 *
 * Interop path:
 *   DMA-BUF fd → eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT)
 *              → cuGraphicsEGLRegisterImage
 *              → cuGraphicsResourceGetMappedEglFrame → CUdeviceptr
 *              → cuMemcpy2D (device-to-device, block-linear→pitch-linear)
 *
 * Two shared libraries are loaded at runtime:
 *   - libEGL.so    — EGL display + DMA-BUF image import
 *   - libcuda.so   — CUDA driver API + EGL interop
 *
 * No build-time dependency on CUDA SDK or EGL SDK — all types are
 * defined here with ABI-compatible layouts.  The header is fully
 * self-contained (no #include of CUDA or EGL headers).
 *
 * Load sequence: same as dynlink_nvmpi.h — idempotent, no dlclose,
 * per-TU static pointers.  EGL extension functions (eglCreateImageKHR,
 * eglDestroyImageKHR) are resolved via eglGetProcAddress because they
 * are KHR extensions not exported by all libEGL builds.
 */
#ifndef DYNLINK_NVMPI_CUDA_H
#define DYNLINK_NVMPI_CUDA_H

#include <dlfcn.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* EGL type definitions (ABI-compatible with EGL/egl.h + eglext.h)     */
/* ------------------------------------------------------------------ */

typedef void          *nvmpi_EGLDisplay;
typedef void          *nvmpi_EGLImageKHR;
typedef int            nvmpi_EGLint;
typedef unsigned int   nvmpi_EGLBoolean;
typedef unsigned int   nvmpi_EGLenum;
typedef void          *nvmpi_EGLClientBuffer;

/* EGL constants */
#define NVMPI_EGL_DEFAULT_DISPLAY       ((nvmpi_EGLDisplay)0)
#define NVMPI_EGL_NO_CONTEXT            ((void *)0)
#define NVMPI_EGL_NO_IMAGE_KHR          ((nvmpi_EGLImageKHR)0)
#define NVMPI_EGL_TRUE                  1
#define NVMPI_EGL_FALSE                 0

/* EGL_EXT_image_dma_buf_import constants */
#define NVMPI_EGL_LINUX_DMA_BUF_EXT     0x3270
#define NVMPI_EGL_LINUX_DRM_FOURCC_EXT  0x3271
#define NVMPI_EGL_DMA_BUF_PLANE0_FD_EXT         0x3272
#define NVMPI_EGL_DMA_BUF_PLANE0_OFFSET_EXT     0x3273
#define NVMPI_EGL_DMA_BUF_PLANE0_PITCH_EXT      0x3274
#define NVMPI_EGL_DMA_BUF_PLANE1_FD_EXT         0x3278
#define NVMPI_EGL_DMA_BUF_PLANE1_OFFSET_EXT     0x3279
#define NVMPI_EGL_DMA_BUF_PLANE1_PITCH_EXT      0x327A

/* DRM fourcc for NV12: 'N' | 'V' << 8 | '1' << 16 | '2' << 24 */
#define NVMPI_DRM_FORMAT_NV12  0x3231564E

/* EGL misc */
#define NVMPI_EGL_WIDTH                 0x3057
#define NVMPI_EGL_HEIGHT                0x3056
#define NVMPI_EGL_NONE                  0x3038

/* ------------------------------------------------------------------ */
/* CUDA driver API type definitions (ABI-compatible with cuda.h)       */
/* ------------------------------------------------------------------ */

typedef int                     nvmpi_CUresult;
typedef int                     nvmpi_CUdevice;
typedef struct CUctx_st        *nvmpi_CUcontext;
#if defined(__LP64__) || defined(_WIN64)
typedef unsigned long long      nvmpi_CUdeviceptr;
#else
typedef unsigned int            nvmpi_CUdeviceptr;
#endif
typedef struct CUgraphicsResource_st *nvmpi_CUgraphicsResource;
typedef struct CUarray_st      *nvmpi_CUarray;

/* CUDA success code */
#define NVMPI_CUDA_SUCCESS  0

/* CUmemorytype values */
#define NVMPI_CU_MEMORYTYPE_HOST    0x01
#define NVMPI_CU_MEMORYTYPE_DEVICE  0x02

/* CU_GRAPHICS_REGISTER_FLAGS */
#define NVMPI_CU_GRAPHICS_REGISTER_FLAGS_NONE       0x00
#define NVMPI_CU_GRAPHICS_REGISTER_FLAGS_READ_ONLY  0x01

/* CUDA_MEMCPY2D — ABI-compatible with CUDA_MEMCPY2D_v2 in cuda.h.
 * Only DEVICE↔DEVICE and HOST↔DEVICE copies are used;
 * array fields are set to NULL. */
typedef struct nvmpi_CUDA_MEMCPY2D {
    size_t srcXInBytes;
    size_t srcY;
    unsigned int srcMemoryType; /* nvmpi_CU_MEMORYTYPE_* */
    const void *srcHost;
    nvmpi_CUdeviceptr srcDevice;
    nvmpi_CUarray srcArray;
    size_t srcPitch;

    size_t dstXInBytes;
    size_t dstY;
    unsigned int dstMemoryType;
    void *dstHost;
    nvmpi_CUdeviceptr dstDevice;
    nvmpi_CUarray dstArray;
    size_t dstPitch;

    size_t WidthInBytes;
    size_t Height;
} nvmpi_CUDA_MEMCPY2D;

/* ------------------------------------------------------------------ */
/* CUeglFrame — ABI-compatible with CUeglFrame_v1 in cudaEGL.h.       */
/*                                                                     */
/* For CU_EGL_FRAME_TYPE_PITCH, frame.pPitch[i] is a CUdeviceptr      */
/* cast to void* — the CUDA device address of plane i.                 */
/* ------------------------------------------------------------------ */

#define NVMPI_MAX_PLANES 3

typedef enum {
    NVMPI_CU_EGL_FRAME_TYPE_ARRAY = 0,
    NVMPI_CU_EGL_FRAME_TYPE_PITCH = 1
} nvmpi_CUeglFrameType;

/* CUeglColorFormat — only values used by NV12/YUV420P decode */
#define NVMPI_CU_EGL_COLOR_FORMAT_YUV420_PLANAR     0x00
#define NVMPI_CU_EGL_COLOR_FORMAT_YUV420_SEMIPLANAR 0x01

typedef struct nvmpi_CUeglFrame {
    union {
        nvmpi_CUarray pArray[NVMPI_MAX_PLANES];
        void         *pPitch[NVMPI_MAX_PLANES];
    } frame;
    unsigned int width;
    unsigned int height;
    unsigned int depth;
    unsigned int pitch;
    unsigned int planeCount;
    unsigned int numChannels;
    nvmpi_CUeglFrameType frameType;
    unsigned int eglColorFormat;  /* CUeglColorFormat enum */
    unsigned int cuFormat;        /* CUarray_format enum */
} nvmpi_CUeglFrame;

/* ------------------------------------------------------------------ */
/* EGL function pointer typedefs                                       */
/* ------------------------------------------------------------------ */

typedef nvmpi_EGLDisplay  (*fn_eglGetDisplay)(void *display_id);
typedef nvmpi_EGLBoolean  (*fn_eglInitialize)(nvmpi_EGLDisplay dpy,
                               nvmpi_EGLint *major, nvmpi_EGLint *minor);
typedef nvmpi_EGLBoolean  (*fn_eglTerminate)(nvmpi_EGLDisplay dpy);
typedef nvmpi_EGLint      (*fn_eglGetError)(void);
typedef void             *(*fn_eglGetProcAddress)(const char *procname);

/* KHR extensions — resolved via eglGetProcAddress at load time */
typedef nvmpi_EGLImageKHR (*fn_eglCreateImageKHR)(nvmpi_EGLDisplay dpy,
                               void *ctx, nvmpi_EGLenum target,
                               nvmpi_EGLClientBuffer buffer,
                               const nvmpi_EGLint *attrib_list);
typedef nvmpi_EGLBoolean  (*fn_eglDestroyImageKHR)(nvmpi_EGLDisplay dpy,
                               nvmpi_EGLImageKHR image);

/* ------------------------------------------------------------------ */
/* CUDA driver API function pointer typedefs                           */
/* ------------------------------------------------------------------ */

typedef nvmpi_CUresult (*fn_cuInit)(unsigned int flags);
typedef nvmpi_CUresult (*fn_cuDeviceGet)(nvmpi_CUdevice *dev, int ordinal);
typedef nvmpi_CUresult (*fn_cuCtxCreate)(nvmpi_CUcontext *pctx,
                           unsigned int flags, nvmpi_CUdevice dev);
typedef nvmpi_CUresult (*fn_cuCtxDestroy)(nvmpi_CUcontext ctx);
typedef nvmpi_CUresult (*fn_cuCtxPushCurrent)(nvmpi_CUcontext ctx);
typedef nvmpi_CUresult (*fn_cuCtxPopCurrent)(nvmpi_CUcontext *pctx);

/* CUDA-EGL interop (from cudaEGL.h, exported by libcuda.so) */
typedef nvmpi_CUresult (*fn_cuGraphicsEGLRegisterImage)(
                           nvmpi_CUgraphicsResource *pCudaResource,
                           nvmpi_EGLImageKHR image,
                           unsigned int flags);
typedef nvmpi_CUresult (*fn_cuGraphicsUnregisterResource)(
                           nvmpi_CUgraphicsResource resource);
typedef nvmpi_CUresult (*fn_cuGraphicsResourceGetMappedEglFrame)(
                           nvmpi_CUeglFrame *eglFrame,
                           nvmpi_CUgraphicsResource resource,
                           unsigned int index, unsigned int mipLevel);

/* Memory operations */
typedef nvmpi_CUresult (*fn_cuMemcpy2D)(const nvmpi_CUDA_MEMCPY2D *pCopy);
typedef nvmpi_CUresult (*fn_cuMemAlloc)(nvmpi_CUdeviceptr *dptr,
                           size_t bytesize);
typedef nvmpi_CUresult (*fn_cuMemFree)(nvmpi_CUdeviceptr dptr);
typedef nvmpi_CUresult (*fn_cuMemAllocPitch)(nvmpi_CUdeviceptr *dptr,
                           size_t *pPitch, size_t WidthInBytes,
                           size_t Height, unsigned int ElementSizeBytes);

/* ------------------------------------------------------------------ */
/* Static function pointers (one set per translation unit)             */
/* ------------------------------------------------------------------ */

/* EGL */
static fn_eglGetDisplay         eglGetDisplay_dl;
static fn_eglInitialize         eglInitialize_dl;
static fn_eglTerminate          eglTerminate_dl;
static fn_eglGetError           eglGetError_dl;
static fn_eglGetProcAddress     eglGetProcAddress_dl;
static fn_eglCreateImageKHR     eglCreateImageKHR_dl;
static fn_eglDestroyImageKHR    eglDestroyImageKHR_dl;

/* CUDA core */
static fn_cuInit                cuInit_dl;
static fn_cuDeviceGet           cuDeviceGet_dl;
static fn_cuCtxCreate           cuCtxCreate_dl;
static fn_cuCtxDestroy          cuCtxDestroy_dl;
static fn_cuCtxPushCurrent      cuCtxPushCurrent_dl;
static fn_cuCtxPopCurrent       cuCtxPopCurrent_dl;

/* CUDA-EGL interop */
static fn_cuGraphicsEGLRegisterImage        cuGraphicsEGLRegisterImage_dl;
static fn_cuGraphicsUnregisterResource      cuGraphicsUnregisterResource_dl;
static fn_cuGraphicsResourceGetMappedEglFrame cuGraphicsResourceGetMappedEglFrame_dl;

/* CUDA memory */
static fn_cuMemcpy2D            cuMemcpy2D_dl;
static fn_cuMemAlloc            cuMemAlloc_dl;
static fn_cuMemFree             cuMemFree_dl;
static fn_cuMemAllocPitch       cuMemAllocPitch_dl;

static void *nvmpi_egl_lib_handle;
static void *nvmpi_cuda_lib_handle;

/* ------------------------------------------------------------------ */
/* Loader                                                              */
/* ------------------------------------------------------------------ */

#define NVMPI_CUDA_LOAD_SYM(handle, ptr, name) do {                 \
    *(void **)&(ptr) = dlsym(handle, #name);                        \
    if (!(ptr)) goto fail;                                          \
} while (0)

/*
 * Load libEGL.so and resolve base + KHR extension symbols.
 * Returns 0 on success, -1 on failure.  Idempotent.
 */
static int nvmpi_egl_dynlink_load(void)
{
    if (nvmpi_egl_lib_handle)
        return 0;

    nvmpi_egl_lib_handle = dlopen("libEGL.so.1", RTLD_LAZY);
    if (!nvmpi_egl_lib_handle)
        nvmpi_egl_lib_handle = dlopen("libEGL.so", RTLD_LAZY);
    if (!nvmpi_egl_lib_handle)
        return -1;

    /* Base EGL functions — direct dlsym */
    NVMPI_CUDA_LOAD_SYM(nvmpi_egl_lib_handle, eglGetDisplay_dl,     eglGetDisplay);
    NVMPI_CUDA_LOAD_SYM(nvmpi_egl_lib_handle, eglInitialize_dl,     eglInitialize);
    NVMPI_CUDA_LOAD_SYM(nvmpi_egl_lib_handle, eglTerminate_dl,      eglTerminate);
    NVMPI_CUDA_LOAD_SYM(nvmpi_egl_lib_handle, eglGetError_dl,       eglGetError);
    NVMPI_CUDA_LOAD_SYM(nvmpi_egl_lib_handle, eglGetProcAddress_dl, eglGetProcAddress);

    /* KHR extensions — must use eglGetProcAddress because some libEGL
     * builds (e.g. Mesa) do not export these directly. */
    eglCreateImageKHR_dl  = (fn_eglCreateImageKHR)eglGetProcAddress_dl("eglCreateImageKHR");
    eglDestroyImageKHR_dl = (fn_eglDestroyImageKHR)eglGetProcAddress_dl("eglDestroyImageKHR");
    if (!eglCreateImageKHR_dl || !eglDestroyImageKHR_dl)
        goto fail;

    return 0;

fail:
    dlclose(nvmpi_egl_lib_handle);
    nvmpi_egl_lib_handle = NULL;
    return -1;
}

/*
 * Load libcuda.so and resolve CUDA driver API + EGL interop symbols.
 * Returns 0 on success, -1 on failure.  Idempotent.
 *
 * Uses versioned symbol names (_v2) where the CUDA driver API has
 * evolved the ABI (cuCtxCreate, cuCtxDestroy, cuCtxPushCurrent,
 * cuCtxPopCurrent, cuMemcpy2D, cuMemAlloc, cuMemFree).
 */
static int nvmpi_cuda_dynlink_load(void)
{
    if (nvmpi_cuda_lib_handle)
        return 0;

    nvmpi_cuda_lib_handle = dlopen("libcuda.so.1", RTLD_LAZY);
    if (!nvmpi_cuda_lib_handle)
        nvmpi_cuda_lib_handle = dlopen("libcuda.so", RTLD_LAZY);
    if (!nvmpi_cuda_lib_handle)
        return -1;

    /* Core CUDA driver API */
    NVMPI_CUDA_LOAD_SYM(nvmpi_cuda_lib_handle, cuInit_dl,           cuInit);
    NVMPI_CUDA_LOAD_SYM(nvmpi_cuda_lib_handle, cuDeviceGet_dl,      cuDeviceGet);

    /* Versioned symbols (_v2) — ABI change between CUDA driver versions */
    NVMPI_CUDA_LOAD_SYM(nvmpi_cuda_lib_handle, cuCtxCreate_dl,      cuCtxCreate_v2);
    NVMPI_CUDA_LOAD_SYM(nvmpi_cuda_lib_handle, cuCtxDestroy_dl,     cuCtxDestroy_v2);
    NVMPI_CUDA_LOAD_SYM(nvmpi_cuda_lib_handle, cuCtxPushCurrent_dl, cuCtxPushCurrent_v2);
    NVMPI_CUDA_LOAD_SYM(nvmpi_cuda_lib_handle, cuCtxPopCurrent_dl,  cuCtxPopCurrent_v2);

    /* CUDA-EGL interop — no _v2 suffix for these */
    NVMPI_CUDA_LOAD_SYM(nvmpi_cuda_lib_handle, cuGraphicsEGLRegisterImage_dl,
                         cuGraphicsEGLRegisterImage);
    NVMPI_CUDA_LOAD_SYM(nvmpi_cuda_lib_handle, cuGraphicsUnregisterResource_dl,
                         cuGraphicsUnregisterResource);
    NVMPI_CUDA_LOAD_SYM(nvmpi_cuda_lib_handle, cuGraphicsResourceGetMappedEglFrame_dl,
                         cuGraphicsResourceGetMappedEglFrame);

    /* Memory operations — versioned */
    NVMPI_CUDA_LOAD_SYM(nvmpi_cuda_lib_handle, cuMemcpy2D_dl,       cuMemcpy2D_v2);
    NVMPI_CUDA_LOAD_SYM(nvmpi_cuda_lib_handle, cuMemAlloc_dl,       cuMemAlloc_v2);
    NVMPI_CUDA_LOAD_SYM(nvmpi_cuda_lib_handle, cuMemFree_dl,        cuMemFree_v2);
    NVMPI_CUDA_LOAD_SYM(nvmpi_cuda_lib_handle, cuMemAllocPitch_dl,  cuMemAllocPitch_v2);

    return 0;

fail:
    dlclose(nvmpi_cuda_lib_handle);
    nvmpi_cuda_lib_handle = NULL;
    return -1;
}

/*
 * Load both EGL and CUDA libraries.
 * Returns 0 on success, -1 if either fails.  Idempotent.
 */
static int nvmpi_cuda_dynlink_load_all(void)
{
    if (nvmpi_egl_dynlink_load() < 0)
        return -1;
    if (nvmpi_cuda_dynlink_load() < 0)
        return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Macro redirects — call sites use these names, hiding the _dl suffix */
/* ------------------------------------------------------------------ */

/* EGL */
#define nvmpi_eglGetDisplay         eglGetDisplay_dl
#define nvmpi_eglInitialize         eglInitialize_dl
#define nvmpi_eglTerminate          eglTerminate_dl
#define nvmpi_eglGetError           eglGetError_dl
#define nvmpi_eglCreateImageKHR     eglCreateImageKHR_dl
#define nvmpi_eglDestroyImageKHR    eglDestroyImageKHR_dl

/* CUDA core */
#define nvmpi_cuInit                cuInit_dl
#define nvmpi_cuDeviceGet           cuDeviceGet_dl
#define nvmpi_cuCtxCreate           cuCtxCreate_dl
#define nvmpi_cuCtxDestroy          cuCtxDestroy_dl
#define nvmpi_cuCtxPushCurrent      cuCtxPushCurrent_dl
#define nvmpi_cuCtxPopCurrent       cuCtxPopCurrent_dl

/* CUDA-EGL interop */
#define nvmpi_cuGraphicsEGLRegisterImage        cuGraphicsEGLRegisterImage_dl
#define nvmpi_cuGraphicsUnregisterResource      cuGraphicsUnregisterResource_dl
#define nvmpi_cuGraphicsResourceGetMappedEglFrame cuGraphicsResourceGetMappedEglFrame_dl

/* CUDA memory */
#define nvmpi_cuMemcpy2D            cuMemcpy2D_dl
#define nvmpi_cuMemAlloc            cuMemAlloc_dl
#define nvmpi_cuMemFree             cuMemFree_dl
#define nvmpi_cuMemAllocPitch       cuMemAllocPitch_dl

#endif /* DYNLINK_NVMPI_CUDA_H */
