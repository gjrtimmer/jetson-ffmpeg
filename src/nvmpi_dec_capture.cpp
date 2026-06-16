#include "nvmpi_dec_internal.h"

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

	ctx->deinitFramePool();
	ctx->initFramePool();
	//get dst_dma buffer params and set corresponding frame size and linesize in nvmpictx
	ctx->updateFrameSizeParams();

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
//     "empty" NVMPI_frameBuf from the pool (format convert + optional
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
	int ret;
	NVMPI_frameBuf* fb = NULL;
	//std::thread transformWorkersPool[3];

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
               ctx->eos=true;
            }
        }
    }
    while ((v4l2Event.type != V4L2_EVENT_RESOLUTION_CHANGE) && !ctx->eos);

    /* Received the resolution change event, now can do respondToResolutionEvent. */
    if (!ctx->eos) respondToResolutionEvent(v4l2Format, v4l2Crop, ctx);

	while (!(ctx->eos || dec->isInError()))
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
		while(!ctx->eos)
		{
			struct v4l2_buffer v4l2_buf;
			struct v4l2_plane planes[MAX_PLANES];
			v4l2_buf.m.planes = planes;

			/* Dequeue a filled buffer. */
			//(0 retries: EAGAIN means nothing decoded yet, poll again;
			// the V4L2_BUF_FLAG_LAST flag marks the final buffer => EOS)
			if (dec->capture_plane.dqBuffer(v4l2_buf, &dec_buffer, NULL, 0))
			{
				if (errno == EAGAIN)
				{
					if (v4l2_buf.flags & V4L2_BUF_FLAG_LAST)
					{
						ERROR_MSG("Got EoS at capture plane");
						ctx->eos=true;
					}
					usleep(1000);
				}
				else
				{
					ERROR_MSG("Error while calling dequeue at capture plane");
					ctx->eos=true;
				}
				break;
			}

			//point the buffer at the DMA fd it was queued with
			dec_buffer->planes[0].fd = ctx->dmaBufferFileDescriptor[v4l2_buf.index];

			//grab a free destination buffer from the pool (non-blocking)
			fb = ctx->framePool->dqEmptyBuf();

			if(fb)
			{
				//hw transform: block-linear decoder output -> pitch-linear
				//dst buffer, converting format and scaling as configured
#ifdef WITH_NVUTILS
				ret = NvBufSurfTransform(ctx->dmaBufferSurface[v4l2_buf.index], fb->dst_dma_surface, &(ctx->transform_params));
#else
				ret = NvBufferTransform(dec_buffer->planes[0].fd, fb->dst_dma_fd, &(ctx->transform_params));
#endif
				TEST_ERROR(ret==-1, "Transform failed",ret);
				//carry the pts through (V4L2 timeval -> microseconds)
				fb->timestamp = (v4l2_buf.timestamp.tv_usec % 1000000) + (v4l2_buf.timestamp.tv_sec * 1000000UL);

				//hand the filled frame to the consumer (user thread)
				ctx->framePool->qFilledBuf(fb);
			}
			else
			{
				//no buffers available in the pool. wait for EOS or for user to read.
				//Backpressure: poll every 500us until the user returns a
				//buffer via nvmpi_decoder_get_frame(), then do the same
				//transform/publish as above. The V4L2 buffer is held back
				//meanwhile, eventually stalling the decoder.
				while(!ctx->eos)
				{
					std::this_thread::sleep_for(std::chrono::microseconds(500));
					fb = ctx->framePool->dqEmptyBuf();
					if(fb)
					{
#ifdef WITH_NVUTILS
						ret = NvBufSurfTransform(ctx->dmaBufferSurface[v4l2_buf.index], fb->dst_dma_surface, &(ctx->transform_params));
#else
						ret = NvBufferTransform(dec_buffer->planes[0].fd, fb->dst_dma_fd, &(ctx->transform_params));
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
	return;
}
