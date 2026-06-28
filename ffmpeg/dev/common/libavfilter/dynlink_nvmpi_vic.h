/*
 * dynlink_nvmpi_vic.h -- Runtime lazy-loading of libnvmpi VIC API.
 *
 * Separate from dynlink_nvmpi.h (libavcodec) because:
 *   1. This header is for libavfilter (vf_scale_vic.c), a different
 *      FFmpeg library with its own translation unit.
 *   2. Only VIC + surface functions are needed — no decoder/encoder.
 *   3. If VIC symbols are absent (older libnvmpi), the filter gracefully
 *      reports unavailable without breaking decoder/encoder codecs.
 *
 * Load sequence: same as dynlink_nvmpi.h — idempotent, no dlclose,
 * per-TU static pointers, thread-safe via dlopen guarantees.
 *
 * Type definitions are duplicated from nvmpi.h so this header is
 * self-contained (no build-time dependency on libnvmpi headers).
 */
#ifndef DYNLINK_NVMPI_VIC_H
#define DYNLINK_NVMPI_VIC_H

#include <dlfcn.h>

/* ------------------------------------------------------------------ */
/* Type definitions (duplicated from nvmpi.h -- keep in sync)          */
/* ------------------------------------------------------------------ */

/* Opaque VIC context handle */
typedef struct nvmpi_vic_ctx nvmpi_vic_ctx;

/* ------------------------------------------------------------------ */
/* Function pointer typedefs                                           */
/* ------------------------------------------------------------------ */

/* VIC transform */
typedef nvmpi_vic_ctx* (*fn_nvmpi_vic_create)(void);
typedef int            (*fn_nvmpi_vic_transform)(nvmpi_vic_ctx *ctx,
                           int src_fd, int dst_fd,
                           unsigned int src_w, unsigned int src_h,
                           unsigned int dst_w, unsigned int dst_h);
typedef void           (*fn_nvmpi_vic_close)(nvmpi_vic_ctx *ctx);

/* Surface alloc/free (shared with encoder path, same libnvmpi.so) */
typedef int            (*fn_nvmpi_surface_alloc)(unsigned int width,
                           unsigned int height, int *dmabuf_fd);
typedef int            (*fn_nvmpi_surface_destroy)(int dmabuf_fd);

/* ------------------------------------------------------------------ */
/* Static function pointers (one set per translation unit)             */
/* ------------------------------------------------------------------ */

static fn_nvmpi_vic_create       nvmpi_vic_create_dl;
static fn_nvmpi_vic_transform    nvmpi_vic_transform_dl;
static fn_nvmpi_vic_close        nvmpi_vic_close_dl;
static fn_nvmpi_surface_alloc    nvmpi_surface_alloc_dl;
static fn_nvmpi_surface_destroy  nvmpi_surface_destroy_dl;

static void *nvmpi_vic_lib_handle;

/* ------------------------------------------------------------------ */
/* Loader                                                              */
/* ------------------------------------------------------------------ */

#define NVMPI_VIC_LOAD_SYM(ptr, name) do {                          \
    *(void **)&(ptr) = dlsym(nvmpi_vic_lib_handle, #name);          \
    if (!(ptr)) goto fail;                                           \
} while (0)

/*
 * Load libnvmpi.so and resolve VIC + surface symbols.
 * Returns 0 on success, -1 on failure.  Idempotent.
 */
static int nvmpi_vic_dynlink_load(void)
{
    if (nvmpi_vic_lib_handle)
        return 0;

    nvmpi_vic_lib_handle = dlopen("libnvmpi.so", RTLD_LAZY);
    if (!nvmpi_vic_lib_handle)
        return -1;

    /* VIC transform symbols */
    NVMPI_VIC_LOAD_SYM(nvmpi_vic_create_dl,      nvmpi_vic_create);
    NVMPI_VIC_LOAD_SYM(nvmpi_vic_transform_dl,   nvmpi_vic_transform);
    NVMPI_VIC_LOAD_SYM(nvmpi_vic_close_dl,       nvmpi_vic_close);

    /* Surface allocation symbols (same .so, shared with encoder path) */
    NVMPI_VIC_LOAD_SYM(nvmpi_surface_alloc_dl,   nvmpi_surface_alloc);
    NVMPI_VIC_LOAD_SYM(nvmpi_surface_destroy_dl, nvmpi_surface_destroy);

    return 0;

fail:
    dlclose(nvmpi_vic_lib_handle);
    nvmpi_vic_lib_handle = NULL;
    return -1;
}

/* ------------------------------------------------------------------ */
/* Macro redirects                                                     */
/* ------------------------------------------------------------------ */

#define nvmpi_vic_create       nvmpi_vic_create_dl
#define nvmpi_vic_transform    nvmpi_vic_transform_dl
#define nvmpi_vic_close        nvmpi_vic_close_dl
#define nvmpi_surface_alloc    nvmpi_surface_alloc_dl
#define nvmpi_surface_destroy  nvmpi_surface_destroy_dl

#endif /* DYNLINK_NVMPI_VIC_H */
