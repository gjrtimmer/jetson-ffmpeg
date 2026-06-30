/*
 * nvmpi_surface.cpp — DMA-BUF surface allocation/destruction utilities
 * for the hardware video pipeline (issue #60, #64).
 *
 * Wraps NvBufSurf::NvAllocate / NvBufferCreateEx behind a simple C API
 * so FFmpeg wrappers (or any caller) can allocate pitch-linear NV12
 * DMA-BUF surfaces.  Used by the VIC scale filter (input + output
 * surface pools) and the encoder DMABUF path.
 *
 * Two allocation functions for different NvMap domains:
 *   nvmpi_surface_alloc         — VIDEO_CONVERT memtag for VIC-only buffers
 *   nvmpi_surface_alloc_for_enc — VIDEO_ENC memtag for encoder-bound buffers
 *
 * The memtag determines which NvMap domain the buffer is registered in.
 * The encoder's NvMMLite layer requires VIDEO_ENC to resolve the NvMap
 * handle during its internal VIC format conversion; VIDEO_CONVERT
 * buffers cause NVMAP_IOC_PARAMETERS failures at the encoder.
 *
 * Supports both buffer API generations:
 *   - NvUtils/NvBufSurface (WITH_NVUTILS, JetPack 5+)
 *   - Legacy nvbuf_utils (JetPack 4)
 */
#include "nvmpi.h"
#include "nvUtils2NvBuf.h"
#include "nvmpi_log.h"
#include <string.h>
#include <sys/mman.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>

/* Internal: allocate a pitch-linear NV12 DMA-BUF surface with a
 * specific NvBufSurfaceTag.  Both public alloc functions delegate here.
 * Allocated here; freed in nvmpi_surface_destroy() via NvBufferDestroy. */
static int nvmpi_surface_alloc_internal(unsigned int width, unsigned int height,
	int *dmabuf_fd, int use_enc_tag)
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
	/* VIDEO_CONVERT: VIC-only buffers (not passed to encoder).
	 * VIDEO_ENC: encoder-bound buffers — registers in the encoder's
	 * NvMap domain so NvMMLite's internal VIC format conversion can
	 * resolve the NvMap handle. Without VIDEO_ENC, the encoder fails
	 * with NVMAP_IOC_PARAMETERS at larger resolutions. */
	params.memtag = use_enc_tag
		? NvBufSurfaceTag_VIDEO_ENC
		: NvBufSurfaceTag_VIDEO_CONVERT;
#else
	params.payloadType = NvBufferPayload_SurfArray;
	params.nvbuf_tag = use_enc_tag
		? NvBufferTag_VIDEO_ENC
		: NvBufferTag_VIDEO_CONVERT;
#endif

	int fd = -1;
#ifdef WITH_NVUTILS
	int ret = NvBufSurf::NvAllocate(&params, 1, &fd);
#else
	int ret = NvBufferCreateEx(&fd, &params);
#endif
	if (ret < 0 || fd < 0) {
		NVMPI_LOG(NVMPI_LOG_ERROR,
			  "nvmpi_surface_alloc: NvAllocate failed (%dx%d, enc_tag=%d)",
			  width, height, use_enc_tag);
		return -1;
	}

	*dmabuf_fd = fd;
	return 0;
}

/* Public: allocate with VIDEO_CONVERT tag (VIC-only buffers) */
int nvmpi_surface_alloc(unsigned int width, unsigned int height,
	int *dmabuf_fd)
{
	return nvmpi_surface_alloc_internal(width, height, dmabuf_fd, 0);
}

/* Public: allocate with VIDEO_ENC tag (encoder-bound buffers).
 * VIC transform also works on VIDEO_ENC tagged surfaces. */
int nvmpi_surface_alloc_for_enc(unsigned int width, unsigned int height,
	int *dmabuf_fd)
{
	return nvmpi_surface_alloc_internal(width, height, dmabuf_fd, 1);
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

/*
 * Copy NV12 frame data from an unregistered DMA-BUF fd (e.g. dup'd fd
 * from the decoder's DRM_PRIME output) into a registered NvBufSurface fd
 * (allocated via nvmpi_surface_alloc).
 *
 * The decoder dup()s its DMA-BUF fds for AVFrame lifetime safety.  Dup'd
 * fds point to the same kernel DMA-BUF object but are NOT registered in
 * NvBufSurface's internal table — NvBufSurfaceFromFd fails on them.  This
 * function bridges the gap: it mmap's the dup'd fd for CPU read and copies
 * the frame data into a properly registered surface that NvBufSurfTransform
 * can operate on.
 *
 * @param dst_fd       Registered NvBufSurface fd (allocated via nvmpi_surface_alloc)
 * @param src_fd       Unregistered DMA-BUF fd (dup'd from decoder)
 * @param width        Frame width in pixels
 * @param height       Frame height in pixels
 * @param src_pitch    Source row stride in bytes (from DRM descriptor)
 * @return 0 on success, -1 on error
 */
int nvmpi_surface_copy_from_dmabuf(int dst_fd, int src_fd,
	unsigned int width, unsigned int height, unsigned int src_pitch)
{
	if (dst_fd < 0 || src_fd < 0) return -1;
	if (width == 0 || height == 0 || src_pitch == 0) return -1;

	int ret = -1;

	/* Source buffer size: NV12 = Y (pitch * height) + UV (pitch * height/2) */
	size_t src_size = (size_t)src_pitch * height * 3 / 2;

	/* DMA-BUF sync: begin CPU read access on the source fd.
	 * Required before mmap to ensure GPU/VIC writes are visible to CPU. */
	struct dma_buf_sync sync_start;
	memset(&sync_start, 0, sizeof(sync_start));
	sync_start.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ;
	if (ioctl(src_fd, DMA_BUF_IOCTL_SYNC, &sync_start) < 0) {
		NVMPI_LOG(NVMPI_LOG_WARN,
			  "nvmpi_surface_copy_from_dmabuf: DMA_BUF_SYNC_START "
			  "failed (fd=%d), proceeding without sync", src_fd);
	}

	/* mmap the source DMA-BUF for CPU read */
	void *src_map = mmap(NULL, src_size, PROT_READ, MAP_SHARED, src_fd, 0);
	if (src_map == MAP_FAILED) {
		NVMPI_LOG(NVMPI_LOG_ERROR,
			  "nvmpi_surface_copy_from_dmabuf: mmap failed (fd=%d, size=%zu)",
			  src_fd, src_size);
		goto end_sync;
	}

#ifdef WITH_NVUTILS
	{
		NvBufSurface *dst_surface = NULL;
		ret = NvBufSurfaceFromFd(dst_fd, (void **)&dst_surface);
		if (ret != 0 || !dst_surface) {
			NVMPI_LOG(NVMPI_LOG_ERROR,
				  "nvmpi_surface_copy_from_dmabuf: NvBufSurfaceFromFd "
				  "failed for dst fd=%d", dst_fd);
			ret = -1;
			goto unmap;
		}

		/* Map destination surface for CPU write */
		ret = NvBufSurfaceMap(dst_surface, 0, -1, NVBUF_MAP_WRITE);
		if (ret != 0) {
			NVMPI_LOG(NVMPI_LOG_ERROR,
				  "nvmpi_surface_copy_from_dmabuf: NvBufSurfaceMap "
				  "failed for dst fd=%d", dst_fd);
			ret = -1;
			goto unmap;
		}

		unsigned int dst_pitch_y = dst_surface->surfaceList[0].planeParams.pitch[0];
		unsigned int dst_pitch_uv = dst_surface->surfaceList[0].planeParams.pitch[1];
		uint8_t *dst_y  = (uint8_t *)dst_surface->surfaceList[0].mappedAddr.addr[0];
		uint8_t *dst_uv = (uint8_t *)dst_surface->surfaceList[0].mappedAddr.addr[1];
		uint8_t *src_y  = (uint8_t *)src_map;
		uint8_t *src_uv = (uint8_t *)src_map + (size_t)src_pitch * height;

		/* Copy Y plane: line-by-line to handle pitch differences */
		unsigned int copy_w = (width < src_pitch && width < dst_pitch_y)
			? width : (src_pitch < dst_pitch_y ? src_pitch : dst_pitch_y);
		unsigned int y;
		for (y = 0; y < height; y++)
			memcpy(dst_y + (size_t)y * dst_pitch_y,
			       src_y + (size_t)y * src_pitch, copy_w);

		/* Copy UV plane: interleaved U/V, half height */
		unsigned int copy_w_uv = (width < src_pitch && width < dst_pitch_uv)
			? width : (src_pitch < dst_pitch_uv ? src_pitch : dst_pitch_uv);
		for (y = 0; y < height / 2; y++)
			memcpy(dst_uv + (size_t)y * dst_pitch_uv,
			       src_uv + (size_t)y * src_pitch, copy_w_uv);

		/* Flush CPU caches to device before hardware reads the surface */
		NvBufSurfaceSyncForDevice(dst_surface, 0, -1);
		NvBufSurfaceUnMap(dst_surface, 0, -1);
		ret = 0;
	}
#else
	{
		/* Legacy API: NvBufferMemMap + NvBufferMemSyncForDevice per plane */
		void *dst_y_ptr = NULL, *dst_uv_ptr = NULL;
		NvBufferParams dst_params;

		if (NvBufferGetParams(dst_fd, &dst_params) < 0) {
			NVMPI_LOG(NVMPI_LOG_ERROR,
				  "nvmpi_surface_copy_from_dmabuf: NvBufferGetParams "
				  "failed (fd=%d)", dst_fd);
			ret = -1;
			goto unmap;
		}

		if (NvBufferMemMap(dst_fd, 0, NvBufferMem_Write, &dst_y_ptr) < 0 ||
		    NvBufferMemMap(dst_fd, 1, NvBufferMem_Write, &dst_uv_ptr) < 0) {
			NVMPI_LOG(NVMPI_LOG_ERROR,
				  "nvmpi_surface_copy_from_dmabuf: NvBufferMemMap "
				  "failed (fd=%d)", dst_fd);
			ret = -1;
			goto unmap;
		}

		unsigned int dst_pitch = dst_params.pitch[0];
		uint8_t *src_y  = (uint8_t *)src_map;
		uint8_t *src_uv = (uint8_t *)src_map + (size_t)src_pitch * height;
		unsigned int copy_w = width < src_pitch ? width : src_pitch;
		if (copy_w > dst_pitch) copy_w = dst_pitch;

		unsigned int y;
		for (y = 0; y < height; y++)
			memcpy((uint8_t *)dst_y_ptr + (size_t)y * dst_pitch,
			       src_y + (size_t)y * src_pitch, copy_w);
		for (y = 0; y < height / 2; y++)
			memcpy((uint8_t *)dst_uv_ptr + (size_t)y * dst_pitch,
			       src_uv + (size_t)y * src_pitch, copy_w);

		NvBufferMemSyncForDevice(dst_fd, 0, &dst_y_ptr);
		NvBufferMemSyncForDevice(dst_fd, 1, &dst_uv_ptr);
		NvBufferMemUnMap(dst_fd, 0, &dst_y_ptr);
		NvBufferMemUnMap(dst_fd, 1, &dst_uv_ptr);
		ret = 0;
	}
#endif

unmap:
	munmap(src_map, src_size);

end_sync:
	{
		/* DMA-BUF sync: end CPU read access on the source fd */
		struct dma_buf_sync sync_end;
		memset(&sync_end, 0, sizeof(sync_end));
		sync_end.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
		ioctl(src_fd, DMA_BUF_IOCTL_SYNC, &sync_end);
	}

	return ret;
}
