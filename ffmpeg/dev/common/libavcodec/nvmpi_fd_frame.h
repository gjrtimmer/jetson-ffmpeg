/*
 * nvmpi_fd_frame.h -- libnvmpi 4.1.0 JPEG-decoder fd API types, copied byte
 * for byte from libnvmpi's nvmpi.h (the ABI is frozen there: fields are only
 * ever appended after `flags`). Included by dynlink_nvmpi.h right after
 * nvPixFormat is defined; do not include it directly.
 */
#ifndef NVMPI_FD_FRAME_H
#define NVMPI_FD_FRAME_H

#include <stddef.h>
#include <stdint.h>

/* Invalid argument (NULL pointer, struct_size too small, wrong handle
 * family). Copied byte for byte from libnvmpi 4.1.0 nvmpi.h. */
#define NVMPI_ERR_INVAL (-22)

/* One decoded JPEG frame as a borrowed DMA-BUF fd. FROZEN ABI: copied byte
 * for byte from libnvmpi 4.1.0 nvmpi.h (fields are only ever appended after
 * `flags`). The caller sets struct_size = sizeof(nvmpi_fd_frame) before the
 * call. Use pitch[]/offset[]/size to address planes, never width/height
 * (the displayed size may be smaller than the padded planes). */
typedef struct {
    uint32_t struct_size;      /* IN: sizeof(nvmpi_fd_frame) of the caller */
    int fd;                    /* borrowed pitch-linear dmabuf fd */
    uint32_t width, height;    /* displayed JPEG size (planes may be larger) */
    nvPixFormat format;        /* NV_PIX_NV12 (2 planes) or NV_PIX_YUV420 (3) */
    uint32_t num_planes;       /* 2 for NV12, 3 for YUV420 */
    uint32_t pitch[3];         /* bytes per row of each plane */
    uint32_t offset[3];        /* byte offset of each plane inside the fd */
    uint64_t size;             /* total size of the buffer behind fd */
    int64_t timestamp;         /* pts copied from nvPacket.pts of the input */
    uint32_t flags;            /* bit0 (NVMPI_FD_FRAME_FULL_RANGE): full range */
} nvmpi_fd_frame;

#define NVMPI_FD_FRAME_FULL_RANGE 0x1u
#define NVMPI_FD_FRAME_V1_SIZE \
    (offsetof(nvmpi_fd_frame, flags) + sizeof(uint32_t))

/* ABI drift guard: pins the LP64 (aarch64) layout against the 4.1.0 header.
 * A mismatch is a build error here instead of silent memory corruption. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && defined(__LP64__)
_Static_assert(NVMPI_ERR_INVAL == -22, "NVMPI_ERR_INVAL ABI");
_Static_assert(sizeof(nvPixFormat) == 4, "nvPixFormat ABI");
_Static_assert(sizeof(nvmpi_fd_frame) == 72, "nvmpi_fd_frame size ABI");
_Static_assert(offsetof(nvmpi_fd_frame, struct_size) == 0, "struct_size ABI");
_Static_assert(offsetof(nvmpi_fd_frame, fd) == 4, "fd ABI");
_Static_assert(offsetof(nvmpi_fd_frame, width) == 8, "width ABI");
_Static_assert(offsetof(nvmpi_fd_frame, height) == 12, "height ABI");
_Static_assert(offsetof(nvmpi_fd_frame, format) == 16, "format ABI");
_Static_assert(offsetof(nvmpi_fd_frame, num_planes) == 20, "num_planes ABI");
_Static_assert(offsetof(nvmpi_fd_frame, pitch) == 24, "pitch ABI");
_Static_assert(offsetof(nvmpi_fd_frame, offset) == 36, "offset ABI");
_Static_assert(offsetof(nvmpi_fd_frame, size) == 48, "size ABI");
_Static_assert(offsetof(nvmpi_fd_frame, timestamp) == 56, "timestamp ABI");
_Static_assert(offsetof(nvmpi_fd_frame, flags) == 64, "flags ABI");
_Static_assert(NVMPI_FD_FRAME_V1_SIZE == 68, "V1 size ABI");
#endif


#endif /* NVMPI_FD_FRAME_H */
