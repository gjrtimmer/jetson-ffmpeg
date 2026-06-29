#include "nvmpi_dec_internal.h"
#include <pthread.h>

/* Defined in nvmpi_vic.cpp — serializes NvBufSurfTransform calls to
 * prevent driver deadlock between capture thread and filter thread. */
extern pthread_mutex_t nvmpi_transform_mutex;

void respondToResolutionEvent(v4l2_format &format, v4l2_crop &crop,nvmpictx* ctx)
{
	int ret=0;

    /* Get capture plane format from the decoder.
       This may change after resolution change event.
       Refer ioctl VIDIOC_G_FMT */
	ret = ctx->dec->capture_plane.getFormat(format);
	TEST_ERROR(ret < 0, "Error: Could not get format from decoder capture plane", ret);

    /* Get the display resolution from the decoder.
       Refer ioctl VIDIOC_G_CROP */
	ret = ctx->dec->capture_plane.getCrop(crop);
	TEST_ERROR(ret < 0, "Error: Could not get crop from decoder capture plane", ret);

	//output resolution = resize target if requested, else stream resolution
	ctx->coded_width = crop.c.width;
	ctx->coded_height = crop.c.height;
	ctx->output_width = ctx->resized.width ? ctx->resized.width : crop.c.width;
	ctx->output_height = ctx->resized.height ? ctx->resized.height : crop.c.height;

	//init/reinit DecoderCapturePlane
#ifndef WITH_NVUTILS
	bool hadCapture = ctx->numberCaptureBuffers > 0;
#endif
	ctx->deinitDecoderCapturePlane();
	ctx->initDecoderCapturePlane(format);

	/* override default seesion. Without overriding session we wil
	   get seg. fault if decoding in forked process*/
#ifdef WITH_NVUTILS
	ctx->session.compute_mode = NvBufSurfTransformCompute_VIC;
	ctx->session.gpu_id = 0;
	ctx->session.cuda_stream = 0;
	NvBufSurfTransformSetSessionParams(&(ctx->session));
#else
	if (hadCapture)
		NvBufferSessionDestroy(ctx->session);
	ctx->session = NvBufferSessionCreate();
#endif

	/* Skip frame pool rebuild if the pool was pre-allocated at the same
	 * dimensions by nvmpi_create_decoder (hint path). The first resolution-change
	 * event will match when container headers were accurate — saves one
	 * alloc/free cycle and reduces first-frame latency. On mismatch (or
	 * mid-stream resolution changes) the pool is rebuilt at the correct size. */
	if (!ctx->allocatedFrameBufs.empty() &&
	    ctx->output_width == (ctx->resized.width ? ctx->resized.width : crop.c.width) &&
	    ctx->output_height == (ctx->resized.height ? ctx->resized.height : crop.c.height))
	{
		NVMPI_LOG(NVMPI_LOG_DEBUG, "frame pool already allocated at %ux%u, skipping rebuild",
		          ctx->output_width, ctx->output_height);
	}
	else
	{
		ctx->deinitFramePool();
		ctx->initFramePool();
		ctx->updateFrameSizeParams();
	}

	//reset buffer transformation params based on new resolution data
	ctx->updateBufferTransformParams();

	return;
}

/*
struct transFormWorker
{
	std::thread _workerThr;
	nvmpictx* _ctx = NULL;

	//void init(nvmpictx* ctx);

	void start(nvmpictx* ctx);
	void stop();

	bool isWaiting();

	void workerFnc();
};

void transFormWorker::start(nvmpictx* ctx)
{
	_ctx = ctx;
	_workerThr = std::thread(&transFormWorker::workerFnc,this);
	return;
}

void transFormWorker::workerFnc()
{

}
*/

//Body of the decoder capture thread (started by nvmpi_create_decoder).
//Responsibilities:
//  1. block until the first resolution-change event so buffer sizes are
//     known, then set up the CAPTURE plane and frame pool;
//  2. loop: dequeue decoded CAPTURE buffers, VIC-transform each into an
//     "empty" nvmpi_frame_buffer from the pool (format convert + optional
//     scale), stamp the pts, publish it as "filled", and re-queue the V4L2
//     buffer to the decoder;
//  3. handle mid-stream resolution changes and EOS/error shutdown.
//Exits when ctx->eos is set (EOS buffer, fatal error, or close()).
void dec_capture_loop_fcn(void *arg)
{
	nvmpictx* ctx=(nvmpictx*)arg;
	NvVideoDecoder *dec = ctx->dec;

	struct v4l2_format v4l2Format;
	struct v4l2_crop v4l2Crop;
	struct v4l2_event v4l2Event;
	memset(&v4l2Event, 0, sizeof(v4l2Event));
	int ret;
	nvmpi_frame_buffer* fb = NULL;
	bool got_resolution_change = false;

    /* Need to wait for the first Resolution change event, so that
       the decoder knows the stream resolution and can allocate appropriate
       buffers when we call REQBUFS. */
    do
    {
        /* Refer ioctl VIDIOC_DQEVENT */
        ret = dec->dqEvent(v4l2Event, 500);
        if (ret < 0)
        {
            if (errno == EAGAIN)
            {
                continue;
            }
            else
            {
               ERROR_MSG("Error in dequeueing decoder event");
               ctx->eos.store(true);
            }
        }
        if (v4l2Event.type == V4L2_EVENT_RESOLUTION_CHANGE)
            got_resolution_change = true;
    }
    while (!got_resolution_change && !ctx->eos.load());

    /* Race guard: after flush/seek, FFmpeg's threaded decoder model
     * (libavcodec >= 61 / FFmpeg 7.0+) may queue all remaining packets
     * including the zero-sized EOS marker before the V4L2 decoder has
     * emitted the resolution-change event triggered by re-primed
     * extradata. put_packet sets ctx->eos on the zero-sized marker,
     * causing the loop above to exit before the event arrives.
     *
     * Without a retry the capture plane stays uninitialized: no REQBUFS,
     * no STREAMON, no DMA buffers. The Tegra V4L2 driver then segfaults
     * when decoded frames have no destination — it does not gracefully
     * handle the "capture plane never set up" state.
     *
     * One additional 500 ms dqEvent covers the gap with margin; the
     * extradata-to-event latency is normally single-digit ms. For
     * nvmpi_decoder_close() the extra wait is within join tolerance. */
    if (!got_resolution_change && ctx->eos.load()) {
        ret = dec->dqEvent(v4l2Event, 500);
        if (ret == 0 && v4l2Event.type == V4L2_EVENT_RESOLUTION_CHANGE)
            got_resolution_change = true;
    }

    /* Initialize capture plane if and only if we received the event.
     * Keyed on got_resolution_change (not eos): even when EOS arrived
     * early, the capture plane MUST be set up so the V4L2 decoder has
     * valid destination buffers for any frames it has already decoded. */
    if (got_resolution_change) {
        respondToResolutionEvent(v4l2Format, v4l2Crop, ctx);
    }

	while (!(ctx->eos.load() || dec->isInError()))
	{
		NvBuffer *dec_buffer;

		// Check for Resolution change again.
		ret = dec->dqEvent(v4l2Event, false);
		if (ret == 0)
		{
			switch (v4l2Event.type)
			{
				case V4L2_EVENT_RESOLUTION_CHANGE:
					respondToResolutionEvent(v4l2Format, v4l2Crop, ctx);
					continue;
			}
		}

		/* Decoder capture loop */
		while(!ctx->eos.load())
		{
			struct v4l2_buffer v4l2_buf;
			struct v4l2_plane planes[MAX_PLANES];
			v4l2_buf.m.planes = planes;

			/* Dequeue a filled buffer.
			 * 0 retries: non-blocking. On EAGAIN we sleep and retry.
			 * The sleep interval (10 ms) is 10× longer than the original
			 * 1 ms, reducing the capture thread's poll rate from ~1 kHz
			 * to ~100 Hz and cutting its CPU overhead proportionally.
			 * This fixes the monotonic CPU rise reported in GitHub #24
			 * and upstream Keylost#41.
			 *
			 * DevicePoll() (kernel-blocking wait) was considered but
			 * rejected: the Tegra V4L2 driver does not unblock DevicePoll
			 * after the final EOS frame is dequeued, causing an indefinite
			 * hang once all capture buffers are drained. The NVIDIA sample
			 * works around this with a dedicated poll thread + semaphore
			 * coordination, but that is too invasive for this fix. */
			if (dec->capture_plane.dqBuffer(v4l2_buf, &dec_buffer, NULL, 0))
			{
				if (errno == EAGAIN)
				{
					if (v4l2_buf.flags & V4L2_BUF_FLAG_LAST)
					{
						ERROR_MSG("Got EoS at capture plane");
						ctx->eos.store(true);
					}
					usleep(10000);
				}
				else
				{
					ERROR_MSG("Error while calling dequeue at capture plane");
					ctx->eos.store(true);
				}
				break;
			}

			//point the buffer at the DMA fd it was queued with
			dec_buffer->planes[0].fd = ctx->dmaBufferFileDescriptor[v4l2_buf.index];

			//grab a free destination buffer from the pool (non-blocking)
			fb = ctx->framePool->dqEmptyBuf();

			if(fb)
			{
				/* hw transform: block-linear decoder output → pitch-linear
			 * dst buffer, converting format and scaling as configured.
			 * Lock the global transform mutex to prevent concurrent
			 * NvBufSurfTransform calls from the filter thread (VIC/GPU
			 * deadlock — see nvmpi_vic.cpp for rationale). */
#ifdef WITH_NVUTILS
				pthread_mutex_lock(&nvmpi_transform_mutex);
				NvBufSurfTransformSetSessionParams(&(ctx->session));
				ret = NvBufSurfTransform(ctx->dmaBufferSurface[v4l2_buf.index], fb->dst_dma_surface, &(ctx->transform_params));
				pthread_mutex_unlock(&nvmpi_transform_mutex);
#else
				pthread_mutex_lock(&nvmpi_transform_mutex);
				ret = NvBufferTransform(dec_buffer->planes[0].fd, fb->dst_dma_fd, &(ctx->transform_params));
				pthread_mutex_unlock(&nvmpi_transform_mutex);
#endif
				TEST_ERROR(ret==-1, "Transform failed",ret);
				//carry the pts through (V4L2 timeval -> microseconds)
				fb->timestamp = (v4l2_buf.timestamp.tv_usec % 1000000) + (v4l2_buf.timestamp.tv_sec * 1000000UL);

				//hand the filled frame to the consumer (user thread)
				ctx->framePool->qFilledBuf(fb);
			}
			else
			{
				/* Backpressure: the frame pool is exhausted because the
				 * consumer (user thread calling nvmpi_decoder_get_frame)
				 * hasn't returned a buffer yet. Block on the pool's
				 * empty-queue CV (100 ms timeout) instead of the previous
				 * 500 µs busy-spin — eliminates ~2000 wakeups/s while
				 * back-pressured. The V4L2 capture buffer is held back
				 * meanwhile, eventually stalling the decoder hardware. */
				while(!ctx->eos.load())
				{
					fb = ctx->framePool->dqEmptyBuf(std::chrono::milliseconds(100));
					if(fb)
					{
#ifdef WITH_NVUTILS
						pthread_mutex_lock(&nvmpi_transform_mutex);
						NvBufSurfTransformSetSessionParams(&(ctx->session));
						ret = NvBufSurfTransform(ctx->dmaBufferSurface[v4l2_buf.index], fb->dst_dma_surface, &(ctx->transform_params));
						pthread_mutex_unlock(&nvmpi_transform_mutex);
#else
						pthread_mutex_lock(&nvmpi_transform_mutex);
						ret = NvBufferTransform(dec_buffer->planes[0].fd, fb->dst_dma_fd, &(ctx->transform_params));
						pthread_mutex_unlock(&nvmpi_transform_mutex);
#endif
						TEST_ERROR(ret==-1, "Transform failed",ret);
						fb->timestamp = (v4l2_buf.timestamp.tv_usec % 1000000) + (v4l2_buf.timestamp.tv_sec * 1000000UL);

						ctx->framePool->qFilledBuf(fb);
						break;
					}
				}
			}

			//return the DMA buffer to the decoder so it can be refilled
			v4l2_buf.m.planes[0].m.fd = ctx->dmaBufferFileDescriptor[v4l2_buf.index];
			if (dec->capture_plane.qBuffer(v4l2_buf, NULL) < 0)
			{
				ERROR_MSG("Error while queueing buffer at decoder capture plane");
			}
		}
	}
	/* Unblock any consumer waiting in dqFilledBuf(timeout) — all exit
	 * paths converge here, so one shutdown() call covers every case. */
	ctx->framePool->shutdown();
	return;
}
