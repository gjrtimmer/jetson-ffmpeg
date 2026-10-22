/*
 * nvmpi_enc_runtime.c — FFmpeg encoder wrapper for libnvmpi (layer 2 of
 * jetson-ffmpeg, the FFmpeg integration layer): the encode hot path
 * (send_frame / receive_packet / flush), GLOBAL_HEADER extradata
 * pre-generation, and packet-pool teardown. Companion to nvmpi_enc.c
 * (init/close/AVOptions/registration + pool setup) — see
 * nvmpi_enc_ff_internal.h for the shared nvmpiEncodeContext struct.
 *
 * libnvmpi's encoder never allocates packet memory — nvPackets pooled
 * here are backed by real AVPacket buffers so delivering a packet to
 * FFmpeg is a zero-copy move; they are recycled after each
 * avcodec_receive_packet().
 */
/* Runtime-loaded via dlopen -- no link-time dependency on libnvmpi.so.
 * dynlink_nvmpi.h is self-contained (duplicates nvmpi.h types) and
 * provides macro redirects so all nvmpi_* calls dispatch through
 * function pointers resolved at codec init time. */
#include "dynlink_nvmpi.h"
#include "avcodec.h"
#include "libavutil/avutil.h"
#include "libavutil/common.h"
#include "libavutil/imgutils.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "libavutil/pixdesc.h"
/* AVDRMFrameDescriptor: extracts DMA-BUF fd + pitch from DRM_PRIME input
 * frames. DRM_PRIME frames are mmap'd for CPU read and fed through the
 * regular MMAP encoder path (put_frame) — this avoids DMABUF-mode driver
 * bugs (bBlitMode throughput deadlock, YUV420M segfault). */
#include "libavutil/hwcontext_drm.h"
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/dma-buf.h>

#include "encode.h"

//Shared nvmpiEncodeContext struct + cross-file declarations.
#include "nvmpi_enc_ff_internal.h"

/* dynlink_nvmpi.h declares all function pointers and nvmpi_dynlink_load()
 * as file-scoped static — each TU that includes it gets its own copy.
 * nvmpi_enc.c calls nvmpi_dynlink_load() in its .init, but that only
 * populates nvmpi_enc.c's copy.  This TU's pointers remain NULL unless
 * we call nvmpi_dynlink_load() ourselves.  The function is idempotent
 * per-TU (checks nvmpi_lib_handle), so the cost is a single pointer
 * compare after the first call. */

//libnvmpi exchanges timestamps in microseconds; used with av_rescale_q to
//convert to/from the stream's AVCodecContext time_base.
static const AVRational NVENC_TIMEBASE = {1, 1000000};

/* Fill nvFrame payload_size[]/linesize[] for a software frame, per plane:
 * rows = ceil(height / 2^log2_chroma_h) for chroma planes (a floor would
 * drop the last chroma row of odd-height frames), size = (rows-1)*linesize
 * + row_bytes, i.e. the tight extent libnvmpi >= 4.6.1 put_frame checks
 * and that is always inside the frame's buffer. Unused planes (NV12 plane
 * 2) get 0. Overflow-checked: linesize and dimensions come from the caller's
 * frame, so a hostile/corrupt value must not wrap size_t into an undersized
 * claim. Returns 0, or AVERROR(EINVAL) on a bad format/stride. */
static int nvmpienc_fill_plane_sizes(nvFrame *nvf, enum AVPixelFormat fmt,
                                     int width, int height, const int linesize[4])
{
	const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(fmt);
	int p;

	if (!desc || width <= 0 || height <= 0)
		return AVERROR(EINVAL);

	for (p = 0; p < 3; p++) {
		size_t rows, row_bytes, size;
		int row_len;

		nvf->linesize[p] = 0;
		nvf->payload_size[p] = 0;
		if (p >= av_pix_fmt_count_planes(fmt))
			continue;

		/* Negative/zero stride: bottom-up or unset layout, not supported. */
		if (linesize[p] <= 0)
			return AVERROR(EINVAL);
		row_len = av_image_get_linesize(fmt, width, p);
		if (row_len <= 0 || linesize[p] < row_len)
			return AVERROR(EINVAL);

		rows = (p == 0) ? (size_t)height
		               : (size_t)AV_CEIL_RSHIFT(height, desc->log2_chroma_h);
		row_bytes = (size_t)row_len;
		if (__builtin_mul_overflow(rows - 1, (size_t)linesize[p], &size) ||
		    __builtin_add_overflow(size, row_bytes, &size))
			return AVERROR(EINVAL);

		nvf->linesize[p] = linesize[p];
		nvf->payload_size[p] = size;
	}
	return 0;
}

//Free one pool packet: both the backing AVPacket and the nvPacket shell.
//(nvmpienc_nvPacket_alloc lives in nvmpi_enc.c with the other pool setup.)
void nvmpienc_nvPacket_free(nvPacket* nPkt)
{
	AVPacket* pkt = nPkt->privData;
	av_packet_free(&pkt);
	free(nPkt);
}

//must call before nvmpi_encoder_close() too free buffers memory
//Drains BOTH pool queues (empty and filled) and frees every packet, since
//libnvmpi itself never deallocates packet memory. Safe only after the
//encoder has been fully drained (no packet checked out by the DQ thread).
int nvmpienc_deinitPktPool(AVCodecContext *avctx)
{
	nvmpi_dynlink_load(); /* populate this TU's static function pointers */
	nvmpiEncodeContext * nvmpi_context = avctx->priv_data;
	nvmpictx *ctx = nvmpi_context->ctx;
	nvPacket* nPkt;

	while(nvmpi_encoder_dqEmptyPacket(ctx, &nPkt) == 0)
	{
		AVPacket* pkt = nPkt->privData;
		av_packet_free(&pkt);
		free(nPkt);
	}
	while(nvmpi_encoder_get_packet(ctx, &nPkt, false) == 0)
	{
		AVPacket* pkt = nPkt->privData;
		av_packet_free(&pkt);
		free(nPkt);
	}

	//TODO check that all mem returned to nothing
	return 0;
}

//Called from nvmpi_encode_init() (nvmpi_enc.c). Spins up a THROWAWAY
//encoder, encodes one blank frame, and scans the bitstream for the first
//IDR NAL — everything before it (SPS/PPS, VPS for HEVC) becomes
//avctx->extradata. On error, frees nvmpi_context->frame (FFmpeg skips
//.close() after a failed .init()).
int nvmpi_encode_gen_global_header_extradata(AVCodecContext *avctx, nvEncParam *param)
{
	nvmpi_dynlink_load(); /* populate this TU's static function pointers */
	nvmpiEncodeContext * nvmpi_context = avctx->priv_data;
	uint8_t *dst[4];
	int linesize[4];
	nvFrame _nvframe={0};
	nvPacket *nPkt;
	nvmpictx *_ctx;
	int i;
	int ret;
	int put_ret;
	int64_t shiftPts = 1000000/param->fps_n;

	/* GLOBAL_HEADER temp encoder: allocate the blank frame as NV12
	 * when the real encoder uses DRM_PRIME (DRM_PRIME has no pixel
	 * description, so av_image_alloc would fail).  pixFormat is
	 * already NV_PIX_NV12 for DRM_PRIME input (set above). */
	enum AVPixelFormat alloc_fmt = (avctx->pix_fmt == AV_PIX_FMT_DRM_PRIME)
		? AV_PIX_FMT_NV12 : avctx->pix_fmt;

	if(avctx->codec->id == AV_CODEC_ID_H264) param->codingType = NV_VIDEO_CodingH264;
	else param->codingType = NV_VIDEO_CodingHEVC;
	/* Blank source frame, freed on every exit below (av_freep(&dst[0])).
	 * Align 1 still gives every plane at least one full row of bytes, so
	 * libnvmpi's put_frame linesize check (>= 4.5.0) accepts it. */
	if (av_image_alloc(dst, linesize, avctx->width, avctx->height, alloc_fmt, 1) < 0)
	{
		av_frame_free(&nvmpi_context->frame);
		return AVERROR(ENOMEM);
	}

	/* Force blocking mode for the extradata encoder — non-blocking is
	 * meaningless here (tight loop, no caller to return EAGAIN to) and
	 * the put_frame call below doesn't check the return value. */
	int saved_nonblocking = param->nonblocking;
	param->nonblocking = 0;
	nvmpi_context->ctx = nvmpi_create_encoder(param);
	param->nonblocking = saved_nonblocking;
	_ctx = nvmpi_context->ctx;
	if (!_ctx)
	{
		av_freep(&dst[0]);
		av_frame_free(&nvmpi_context->frame);
		/* AVERROR_EXTERNAL: device-level failure (EBUSY, ENODEV, etc.),
		 * not a memory allocation error. See #37. */
		return AVERROR_EXTERNAL;
	}
	{
		int pool_ret = nvmpienc_initPktPool(avctx, nvmpi_context->packet_pool_size);
		if (pool_ret < 0) {
			av_log(avctx, AV_LOG_ERROR,
			       "nvmpi: extradata encoder packet pool init failed\n");
			av_freep(&dst[0]);
			nvmpi_encoder_close(nvmpi_context->ctx);
			nvmpi_context->ctx = NULL;
			av_frame_free(&nvmpi_context->frame);
			return pool_ret;
		}
	}
	i=0;
	_nvframe.timestamp=0;

	while(true)
	{
		_nvframe.payload[0]=dst[0];
		_nvframe.payload[1]=dst[1];
		_nvframe.payload[2]=dst[2];
		/* Tight per-plane sizes (ceil chroma rows): see the helper. */
		if (nvmpienc_fill_plane_sizes(&_nvframe, alloc_fmt, avctx->width,
		                              avctx->height, linesize) < 0)
		{
			av_log(avctx, AV_LOG_ERROR,
			       "nvmpi: extradata encoder: bad blank frame layout\n");
			av_freep(&dst[0]);
			nvmpi_encoder_close(nvmpi_context->ctx);
			nvmpi_context->ctx = NULL;
			av_frame_free(&nvmpi_context->frame);
			return AVERROR(EINVAL);
		}
		_nvframe.timestamp+=shiftPts;

		/* libnvmpi >= 4.5.2 refuses a frame with NVMPI_ERR_EAGAIN (not
		 * queued) also in blocking mode once every pool packet is owed to
		 * an in-flight AU: then wait (bounded) for one of those packets
		 * instead of spinning. Any other error (e.g. NVMPI_ERR_INVAL for
		 * a rejected frame layout) can never succeed on retry, so fail
		 * init instead of looping forever. */
		put_ret = nvmpi_encoder_put_frame(_ctx,&_nvframe);
		if (put_ret < 0 && put_ret != NVMPI_ERR_EAGAIN)
		{
			av_log(avctx, AV_LOG_ERROR,
			       "nvmpi: extradata encoder rejected a frame (code=%d)\n", put_ret);
			av_freep(&dst[0]);
			nvmpienc_deinitPktPool(avctx);
			nvmpi_encoder_close(nvmpi_context->ctx);
			nvmpi_context->ctx = NULL;
			av_frame_free(&nvmpi_context->frame);
			return AVERROR_EXTERNAL;
		}

		ret=nvmpi_encoder_get_packet(_ctx,&nPkt,
			(avctx->flags & AV_CODEC_FLAG_LOW_DELAY) || put_ret == NVMPI_ERR_EAGAIN);

		if(ret<0)
			continue;

		//find idr index
		//Walk the Annex-B stream for the start code that begins the
		//IDR slice; 'i' then marks the end of the header NALs.
		while(i<nPkt->payload_size)
		{
			//check if nal start code (the start-code probe reads
			//payload[i+1..i+4], so stay 5 bytes inside the payload)
			if(i + 4 < nPkt->payload_size && nPkt->payload[i] == 0 && nPkt->payload[i+1] == 0 && nPkt->payload[i+2] == 0 && nPkt->payload[i+3] == 0x01)
			{
				if(param->codingType == NV_VIDEO_CodingH264)
				{
					//h264 idr
					if(nPkt->payload[i+4] == 0x65) break;
				}
				else // NV_VIDEO_CodingHEVC:
				{
					//h265 idr
					if(nPkt->payload[i+4] == 0x26 || nPkt->payload[i+4] == 0x28) break;
				}
			}
			i++;
		}

		avctx->extradata_size=i;
		avctx->extradata	= av_mallocz( avctx->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE );
		/* Guard against av_mallocz OOM — memcpy to NULL is
		 * undefined behavior (likely crash). */
		if (!avctx->extradata) {
			avctx->extradata_size = 0;
			nvmpienc_nvPacket_free(nPkt);
			av_freep(&dst[0]);
			nvmpienc_deinitPktPool(avctx);
			nvmpi_encoder_close(nvmpi_context->ctx);
			nvmpi_context->ctx = NULL;
			av_frame_free(&nvmpi_context->frame);
			return AVERROR(ENOMEM);
		}
		memcpy( avctx->extradata, nPkt->payload,avctx->extradata_size);
		memset( avctx->extradata + avctx->extradata_size, 0, AV_INPUT_BUFFER_PADDING_SIZE );

		nvmpienc_nvPacket_free(nPkt);
		nPkt = nvmpienc_nvPacket_alloc(avctx, NVMPI_ENC_CHUNK_SIZE);

		//return buffer to pool
		nvmpi_encoder_qEmptyPacket(nvmpi_context->ctx, nPkt);
		//send eos
		nvmpi_encoder_put_frame(nvmpi_context->ctx,NULL);

		break;
	}

	//drain encoder
	//(keep pulling until get_packet reports EOS so the temporary
	// encoder can shut down cleanly; recycled packets keep the pool
	// populated while draining)
	while(true)
	{
		ret=nvmpi_encoder_get_packet(_ctx,&nPkt, false);
		if(ret < 0)
		{
			if(ret == -2) break; //got eos
			//usleep(1000);
			continue;
		}
		nvmpienc_nvPacket_free(nPkt);
		nPkt = nvmpienc_nvPacket_alloc(avctx, NVMPI_ENC_CHUNK_SIZE);
		nvmpi_encoder_qEmptyPacket(nvmpi_context->ctx, nPkt);
	}

	av_freep(&dst[0]); //free allocated image planes
	nvmpienc_deinitPktPool(avctx);
	nvmpi_encoder_close(nvmpi_context->ctx);
	nvmpi_context->ctx = NULL;

	return 0;
}

//send_frame half of the encode API: wrap the AVFrame's planes in an
//nvFrame (no copy here — libnvmpi memcpy's into its V4L2 buffer inside
//put_frame) and rescale pts to microseconds. In blocking mode (default),
//put_frame may block briefly; in non-blocking mode, returns AVERROR(EAGAIN)
//when no OUTPUT-plane buffer is available. frame==NULL initiates flushing
//(EOS to libnvmpi). Called internally by ff_nvmpi_receive_packet_async().
static int ff_nvmpi_send_frame(AVCodecContext *avctx,const AVFrame *frame)
{
	nvmpiEncodeContext * nvmpi_context = avctx->priv_data;
	nvFrame _nvframe={0};
	int res;

	if (nvmpi_context->encoder_flushing)
		return AVERROR_EOF;

	if(frame)
	{
		/* Runtime control: force IDR when FFmpeg signals a keyframe request
		 * via pict_type (e.g. -force_key_frames or scene-change detection).
		 * Must be called BEFORE queueing the frame so the V4L2 encoder
		 * applies the IDR flag to this specific frame. */
		if (frame->pict_type == AV_PICTURE_TYPE_I)
			nvmpi_encoder_force_idr(nvmpi_context->ctx);

		/* Runtime control: adaptive bitrate — detect changes to
		 * avctx->bit_rate between frames and propagate to the hw encoder.
		 * Comparison uses int64_t to match AVCodecContext.bit_rate type. */
		if (avctx->bit_rate > 0 && avctx->bit_rate != nvmpi_context->last_bitrate) {
			nvmpi_encoder_set_bitrate(nvmpi_context->ctx, (uint32_t)avctx->bit_rate);
			nvmpi_context->last_bitrate = avctx->bit_rate;
		}

		/* DRM_PRIME path: mmap the DMA-BUF fd for CPU read, build an
		 * nvFrame from the NV12 plane data, and feed it through the
		 * regular MMAP encoder path (put_frame).
		 *
		 * This avoids DMABUF-mode driver bugs entirely — the encoder
		 * uses its own MMAP buffers, so there is no NvBufSurface vs
		 * V4L2 format mismatch and bBlitMode never triggers.
		 *
		 * The dup'd fd from objects[0] works for mmap — it points to
		 * the same kernel DMA-BUF as the original. */
		if (nvmpi_context->dmabuf_input) {
			AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor *)frame->data[0];
			int drm_fd;
			int pitch;
			size_t y_size, uv_size, uv_off, total_size, uv_rows;
			void *map;
			struct dma_buf_sync sync;

			if (!desc || desc->nb_objects < 1) {
				av_log(avctx, AV_LOG_ERROR,
				       "nvmpi: DRM_PRIME frame missing descriptor or objects\n");
				return AVERROR(EINVAL);
			}

			drm_fd = desc->objects[0].fd;
			pitch  = desc->layers[0].planes[0].pitch;

			/* NV12 layout: Y plane (pitch * height) + UV plane (pitch * height/2).
			 * The UV plane starts at the descriptor's real chroma offset:
			 * decoders whose surfaces pad the plane height (JPEG decoder,
			 * odd/unaligned sizes) report an offset above pitch * height.
			 * Video-decoder frames report exactly pitch * height, so their
			 * behaviour is unchanged. */
			/* Tight extents (last row = width bytes, chroma = ceil(h/2)
			 * rows): same contract as nvmpienc_fill_plane_sizes(). */
			if (pitch <= 0 || pitch < frame->width ||
			    frame->width <= 0 || frame->height <= 0) {
				av_log(avctx, AV_LOG_ERROR,
				       "nvmpi: DRM_PRIME frame has an invalid pitch\n");
				return AVERROR(EINVAL);
			}
			uv_rows = (size_t)AV_CEIL_RSHIFT(frame->height, 1);
			y_size  = (size_t)pitch * (frame->height - 1) + frame->width;
			uv_size = (size_t)pitch * (uv_rows - 1) +
			          2 * (size_t)AV_CEIL_RSHIFT(frame->width, 1);
			uv_off  = (size_t)pitch * frame->height;
			if (desc->nb_layers > 0 && desc->layers[0].nb_planes > 1 &&
			    desc->layers[0].planes[1].offset > 0)
				uv_off = (size_t)desc->layers[0].planes[1].offset;
			/* Bounds: the chroma plane must start after luma and the whole
			 * mapping must fit the dma-buf, else mmap'd reads would run past
			 * it (SIGBUS) on a corrupt/hostile descriptor. */
			if (uv_off < y_size || uv_off > SIZE_MAX - uv_size ||
			    (desc->objects[0].size &&
			     uv_off + uv_size > desc->objects[0].size)) {
				av_log(avctx, AV_LOG_ERROR,
				       "nvmpi: DRM_PRIME frame has an inconsistent NV12 layout\n");
				return AVERROR(EINVAL);
			}
			total_size = uv_off + uv_size;

			/* DMA-BUF sync: ensure VIC/decoder writes are visible to CPU */
			memset(&sync, 0, sizeof(sync));
			sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ;
			ioctl(drm_fd, DMA_BUF_IOCTL_SYNC, &sync);

			map = mmap(NULL, total_size, PROT_READ, MAP_SHARED, drm_fd, 0);
			if (map == MAP_FAILED) {
				av_log(avctx, AV_LOG_ERROR,
				       "nvmpi: mmap DRM_PRIME fd=%d failed\n", drm_fd);
				sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
				ioctl(drm_fd, DMA_BUF_IOCTL_SYNC, &sync);
				return AVERROR_EXTERNAL;
			}

			/* Build nvFrame pointing into the mapped NV12 data.
			 * payload[0] = Y, payload[1] = interleaved UV.
			 * linesize = pitch (may differ from width due to alignment). */
			_nvframe.payload[0] = map;
			_nvframe.payload[1] = (uint8_t *)map + uv_off;
			_nvframe.payload[2] = NULL;
			_nvframe.payload_size[0] = y_size;
			_nvframe.payload_size[1] = uv_size;
			_nvframe.payload_size[2] = 0;
			_nvframe.linesize[0] = pitch;
			_nvframe.linesize[1] = pitch;
			_nvframe.linesize[2] = 0;
			_nvframe.timestamp = av_rescale_q(frame->pts, avctx->time_base, NVENC_TIMEBASE);

			res = nvmpi_encoder_put_frame(nvmpi_context->ctx, &_nvframe);

			munmap(map, total_size);
			sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
			ioctl(drm_fd, DMA_BUF_IOCTL_SYNC, &sync);

			/* Non-blocking mode: map libnvmpi EAGAIN to FFmpeg EAGAIN */
			if (res == NVMPI_ERR_EAGAIN) return AVERROR(EAGAIN);
			return res < 0 ? res : 0;
		}

		/* Software frame path: copy plane pointers into nvFrame for
		 * libnvmpi to memcpy into its V4L2 buffer. */
		_nvframe.payload[0]=frame->data[0];
		_nvframe.payload[1]=frame->data[1];
		_nvframe.payload[2]=frame->data[2];

		/* Odd heights: chroma has ceil(h/2) rows; a floor here makes libnvmpi
		 * (>= 4.6.1) refuse the frame with NVMPI_ERR_INVAL (#138). */
		res = nvmpienc_fill_plane_sizes(&_nvframe, frame->format, frame->width,
		                                frame->height, frame->linesize);
		if (res < 0) {
			av_log(avctx, AV_LOG_ERROR, "nvmpi: unsupported frame layout\n");
			return res;
		}

		_nvframe.timestamp=av_rescale_q(frame->pts, avctx->time_base, NVENC_TIMEBASE);

		res=nvmpi_encoder_put_frame(nvmpi_context->ctx,&_nvframe);

		/* Non-blocking mode: map libnvmpi EAGAIN to FFmpeg EAGAIN */
		if (res == NVMPI_ERR_EAGAIN) return AVERROR(EAGAIN);
		if(res<0)
			return res;
	}
	else
	{
		/* EOS: signal end-of-stream to libnvmpi.  Both DRM_PRIME and
		 * software paths use MMAP mode — put_frame(NULL) works for both.
		 *
		 * In non-blocking mode, put_frame(NULL) can return NVMPI_ERR_EAGAIN
		 * if all OUTPUT-plane buffers are busy. Only set encoder_flushing
		 * on success — otherwise the retry path in receive_packet_async
		 * would see encoder_flushing=1 and short-circuit send_frame with
		 * AVERROR_EOF, never actually submitting the EOS to the encoder.
		 * This would cause an infinite EAGAIN loop and lost final packets. */
		res = nvmpi_encoder_put_frame(nvmpi_context->ctx, NULL);
		if (res == NVMPI_ERR_EAGAIN)
			return AVERROR(EAGAIN);
		nvmpi_context->encoder_flushing = 1;
	}

	return 0;
}

//receive_packet half of the encode API: pop a filled nvPacket from
//libnvmpi, rescale the pts back to the codec time_base, shrink the backing
//AVPacket to the actual payload size and MOVE its buffer into the user's
//pkt (zero-copy). The pool packet is then re-armed with a fresh buffer and
//returned to the empty queue. EAGAIN when nothing is ready; AVERROR_EOF
//once flushing has drained.
static int ff_nvmpi_receive_packet(AVCodecContext *avctx, AVPacket *pkt)
{
	nvmpiEncodeContext * nvmpi_context = avctx->priv_data;
	nvPacket *nPkt;
	AVPacket *aPkt;
	int res;

	/* During flushing, use blocking get_packet to avoid CPU spin while
	 * waiting for the DQ thread to deliver final encoded packets.
	 * Also block when the last send_frame was refused with EAGAIN
	 * (libnvmpi back-pressure, jetson-nvmpi#125): the packet pool is full of
	 * in-flight AUs, so a packet is guaranteed to arrive. With a tiny
	 * packet_pool_size a non-blocking get_packet could return EAGAIN here
	 * too, leaving FFmpeg with EAGAIN on both send and receive (stall).
	 * Bounded by libnvmpi's wait_timeout, so it cannot hang. */
	res = nvmpi_encoder_get_packet(nvmpi_context->ctx, &nPkt,
		(avctx->flags & AV_CODEC_FLAG_LOW_DELAY) || nvmpi_context->encoder_flushing
		|| nvmpi_context->send_eagain);
	if(res<0)
	{
		/* Blocking-mode back-pressure wait came back empty: libnvmpi
		 * returns at once when no AU is in flight, else after
		 * wait_timeout. Either way our owed count is stale (an AU was
		 * dropped on an error path, or B-frame delay >= pool size);
		 * clear it so the pending frame is sent next call instead of
		 * livelocking on a packet that will never come. */
		if(res == -1 && nvmpi_context->owed_wait)
			nvmpi_context->aus_owed = 0;
		/* -2 = actual EOS from the V4L2 DQ thread (all packets drained).
		 * -1 = no packet ready yet (DQ thread still processing); return
		 * EAGAIN so FFmpeg keeps calling receive_packet during flush.
		 * Previously, -1 during flushing was treated as EOF, causing
		 * final packets to be dropped (see #15). */
		if(res == -2) return AVERROR_EOF;
		return AVERROR(EAGAIN);
	}

	/* One owed AU delivered: its pool packet is recycled below. */
	if(nvmpi_context->aus_owed > 0)
		nvmpi_context->aus_owed--;

	aPkt = (AVPacket*)(nPkt->privData);
	//aPkt->dts=aPkt->pts=nPkt->pts;
	aPkt->dts = aPkt->pts = av_rescale_q(nPkt->pts, NVENC_TIMEBASE, avctx->time_base);
	av_shrink_packet(aPkt, nPkt->payload_size);
	if(nPkt->flags& AV_PKT_FLAG_KEY) aPkt->flags = AV_PKT_FLAG_KEY;
	av_packet_move_ref(pkt, aPkt);

	if(nvmpienc_nvPacket_reset(nPkt, avctx, NVMPI_ENC_CHUNK_SIZE))
	{
		nvmpienc_nvPacket_free(nPkt);
		return AVERROR(ENOMEM);
	}

	nvmpi_encoder_qEmptyPacket(nvmpi_context->ctx, nPkt);

	return 0;
}

//Single-callback encoder entry point: FFmpeg
//only calls receive_packet, and the encoder pulls its own input via
//ff_encode_get_frame(). Flow per call:
//  1. if no frame is pending, pull one (EAGAIN = nothing to send yet;
//     EOF = flush by sending NULL);
//  2. push it to libnvmpi via ff_nvmpi_send_frame() and unref on success
//     (on failure the frame stays in nvmpi_context->frame for retry);
//  3. try to return an encoded packet via ff_nvmpi_receive_packet().
int ff_nvmpi_receive_packet_async(AVCodecContext *avctx, AVPacket *pkt)
{
	nvmpi_dynlink_load(); /* populate this TU's static function pointers */
	int res;
	nvmpiEncodeContext * nvmpi_context = avctx->priv_data;
	AVFrame *frame = nvmpi_context->frame;
	bool needSendFrame = true;

	if (!frame->buf[0])
	{
		res = ff_encode_get_frame(avctx, frame);
		if (res < 0)
		{
			if(res == AVERROR(EAGAIN)) needSendFrame = false;
			else if(res == AVERROR_EOF) frame = NULL;
			else return res;
		}
	}

	/* Cleared every call; set below only if this call's send is refused. */
	nvmpi_context->send_eagain = 0;
	nvmpi_context->owed_wait = 0;

	/* Blocking-mode packet-pool back-pressure. Every AU needs one empty
	 * pool packet when libnvmpi's DQ thread dequeues it, and with the pool
	 * empty the AU is DROPPED (frames missing from the output). In blocking
	 * mode put_frame only blocks on OUTPUT-plane buffers, so up to
	 * 2 * num_capture_buffers AUs (OUTPUT + CAPTURE queues) can be in
	 * flight when this thread is starved of CPU while the DQ thread runs:
	 * more than the default packet_pool_size (10). Never let more frames
	 * be in flight than the pool holds: keep the frame pending (it stays in
	 * nvmpi_context->frame for the next call) and wait for a packet
	 * instead. Non-blocking mode needs no check here: libnvmpi refuses
	 * the frame itself (EAGAIN) under the same condition. EOS (frame==NULL)
	 * yields no AU and is never held back. */
	if(needSendFrame && frame && !nvmpi_context->nonblocking &&
	   nvmpi_context->aus_owed >= nvmpi_context->packet_pool_size)
	{
		needSendFrame = false;
		nvmpi_context->send_eagain = 1;  //receive_packet waits for a packet
		nvmpi_context->owed_wait = 1;
	}

	if(needSendFrame)
	{
		res = ff_nvmpi_send_frame(avctx, frame);
		if (res < 0)
		{
			/* Back-pressure: frame stays pending; receive_packet must wait. */
			if(res == AVERROR(EAGAIN)) nvmpi_context->send_eagain = 1;
			if(res != AVERROR_EOF && res != AVERROR(EAGAIN)) return res;
		}
		else
		{
			/* A real frame now owes one AU (and one pool packet). */
			if(frame) nvmpi_context->aus_owed++;
			av_frame_unref(frame);
		}
	}

	res = ff_nvmpi_receive_packet(avctx, pkt);
	if(res<0) return res;

	return 0;
}

//AVCodec/FFCodec .flush callback: reset the V4L2 encoder pipeline for
//mid-stream restart (e.g. after seek). Resets the libnvmpi encoder
//(STREAMOFF/STREAMON, drain packet pool, restart DQ thread) and clears
//the FFmpeg-side flushing flag so send_frame accepts new input.
void nvmpi_flush_encoder(AVCodecContext *avctx)
{
    nvmpi_dynlink_load(); /* populate this TU's static function pointers */
    nvmpiEncodeContext *nvmpi_context = avctx->priv_data;

    /* Reset the libnvmpi encoder pipeline. */
    nvmpi_encoder_flush(nvmpi_context->ctx);

    /* Clear the FFmpeg-side flushing flag so send_frame accepts frames. */
    nvmpi_context->encoder_flushing = 0;
    nvmpi_context->send_eagain = 0;
    /* STREAMOFF discarded every in-flight AU: nothing is owed any more. */
    nvmpi_context->aus_owed = 0;
    nvmpi_context->owed_wait = 0;
}
