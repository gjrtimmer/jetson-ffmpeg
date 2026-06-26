/*
 * nvmpi_surface.cpp — DMA-BUF surface allocation/destruction utilities
 * for the zero-copy encoder path (issue #60).
 *
 * Wraps NvBufSurf::NvAllocate / NvBufferCreateEx behind a simple C API
 * so FFmpeg wrappers (or any caller) can allocate pitch-linear NV12
 * DMA-BUF surfaces compatible with the encoder's V4L2_MEMORY_DMABUF
 * OUTPUT plane. The critical detail is NvBufSurfaceTag_VIDEO_CONVERT —
 * without this memtag the VIC hardware and encoder reject the buffer.
 *
 * Supports both buffer API generations:
 *   - NvUtils/NvBufSurface (WITH_NVUTILS, JetPack 5+)
 *   - Legacy nvbuf_utils (JetPack 4)
 */
#include "nvmpi.h"
#include "nvUtils2NvBuf.h"
#include "nvmpi_log.h"
#include <string.h>

/* Allocated here; freed in nvmpi_surface_destroy() via NvBufferDestroy */
int nvmpi_surface_alloc(unsigned int width, unsigned int height,
	int *dmabuf_fd)
{
	if (!dmabuf_fd) return -1;
	*dmabuf_fd = -1;

	/* Validate dimensions — zero or enormous values would either
	 * silently allocate nothing or exhaust DMA memory. Upper bound
	 * matches Tegra NVDEC/NVENC maximum (8192 on Orin). */
	if (width == 0 || height == 0 || width > 8192 || height > 8192) {
		NVMPI_LOG(NVMPI_LOG_ERROR,
			  "nvmpi_surface_alloc: invalid dimensions %ux%u",
			  width, height);
		return -1;
	}

	NvBufferCreateParams params;
	memset(&params, 0, sizeof(params));
	params.width = width;
	params.height = height;
	params.layout = NvBufferLayout_Pitch;
	params.colorFormat = NvBufferColorFormat_NV12;
#ifdef WITH_NVUTILS
	params.memType = NVBUF_MEM_SURFACE_ARRAY;
	/* VIDEO_CONVERT memtag: marks the surface as VIC-compatible so the
	 * encoder's V4L2 DMABUF path accepts it. Without this tag the
	 * driver rejects the fd at qBuffer time with -EINVAL. */
	params.memtag = NvBufSurfaceTag_VIDEO_CONVERT;
#else
	params.payloadType = NvBufferPayload_SurfArray;
	params.nvbuf_tag = NvBufferTag_VIDEO_CONVERT;
#endif

	int fd = -1;
#ifdef WITH_NVUTILS
	int ret = NvBufSurf::NvAllocate(&params, 1, &fd);
#else
	int ret = NvBufferCreateEx(&fd, &params);
#endif
	if (ret < 0 || fd < 0) {
		NVMPI_LOG(NVMPI_LOG_ERROR,
			  "nvmpi_surface_alloc: NvAllocate failed (%dx%d)",
			  width, height);
		return -1;
	}

	*dmabuf_fd = fd;
	return 0;
}

/* Freed here; allocated in nvmpi_surface_alloc() above */
int nvmpi_surface_destroy(int dmabuf_fd)
{
	if (dmabuf_fd < 0) return 0;

	int ret = NvBufferDestroy(dmabuf_fd);
	if (ret < 0) {
		NVMPI_LOG(NVMPI_LOG_ERROR,
			  "nvmpi_surface_destroy: NvBufferDestroy failed (fd=%d)",
			  dmabuf_fd);
		return -1;
	}
	return 0;
}
