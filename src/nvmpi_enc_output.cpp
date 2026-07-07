#include "nvmpi_enc_internal.h"

//Callback run on NvVideoEncoder's capture-plane DQ thread for every
//dequeued (encoded) buffer. Contract: return true to keep the thread
//running, false to stop it (EOS or fatal error).
//Steps: detect the zero-byte EOS buffer; fetch encode metadata (keyframe
//flag); take an "empty" nvPacket from the pool and memcpy the bitstream
//into it, then publish it "filled" for nvmpi_encoder_get_packet(); finally
//re-queue the V4L2 buffer so the encoder can reuse it. If the pool is
//empty the encoded data is DROPPED (warning printed) — the FFmpeg wrapper
//sizes the pool via the packet_pool_size option to avoid this.
bool encoder_capture_plane_dq_callback(struct v4l2_buffer *v4l2_buf, NvBuffer * buffer, NvBuffer * shared_buffer __attribute__((unused)), void *arg)
{
	nvmpictx *ctx = (nvmpictx*)arg;

	if (v4l2_buf == NULL)
	{
		NVMPI_LOG(NVMPI_LOG_ERROR, "error while dequeing buffer from output plane");
		return false;
	}

	if (buffer->planes[0].bytesused == 0)
	{
		ctx->capPlaneGotEOS.store(true, std::memory_order_release);
		NVMPI_LOG(NVMPI_LOG_DEBUG, "got 0-size buffer in capture (EOS signal)");
		return false;
	}

	v4l2_ctrl_videoenc_outputbuf_metadata enc_metadata;
	ctx->enc->getMetadata(v4l2_buf->index, enc_metadata);

	//nvPacket.payload --> AVPacket->data
	//nvPacket.privData --> AVPacket
	nvPacket* pkt = ctx->pktPool->dqEmptyBuf();
	if(!pkt)
	{
		//TODO wait for user to read buffer. make send_frame return AVERROR(EAGAIN) until avcodec_receive_packet() is called
		NVMPI_LOG(NVMPI_LOG_WARN, "EAGAIN: user must read output; encoder packet pool empty; packet dropped; there may be artifacts in output video");
	}
	else if (buffer->planes[0].bytesused > NVMPI_ENC_CHUNK_SIZE)
	{
		//pool packet buffers are NVMPI_ENC_CHUNK_SIZE bytes; copying a
		//larger encoded frame would overflow them
		NVMPI_LOG(NVMPI_LOG_ERROR, "encoded frame (%u bytes) exceeds packet buffer (%u); frame dropped; there will be artifacts in output video",
			buffer->planes[0].bytesused, (unsigned)(NVMPI_ENC_CHUNK_SIZE));
		ctx->pktPool->qEmptyBuf(pkt);
	}
	else
	{
		pkt->pts = (v4l2_buf->timestamp.tv_usec % 1000000) + (v4l2_buf->timestamp.tv_sec * 1000000UL);
		//AV_PKT_FLAG_KEY 0x0001. if current packet is keyframe then enc_metadata.KeyFrame should be 0x1, so it should be OK to just assign value
		pkt->flags = enc_metadata.KeyFrame;
		pkt->payload_size = buffer->planes[0].bytesused;
		memcpy(pkt->payload, buffer->planes[0].data, pkt->payload_size);

		ctx->pktPool->qFilledBuf(pkt);
	}

	if (ctx->enc->capture_plane.qBuffer(*v4l2_buf, NULL) < 0)
	{
		//TODO error handling
		ERROR_MSG("Error while Qing buffer at capture plane");
		return false;
	}

	return true;
}

//Alternate OUTPUT-plane setup used only when OUTPLANE_MEMTYPE_DMA is
//selected: REQBUFS as V4L2_MEMORY_DMABUF and self-allocate one pitch-linear
//YUV420 NvBufSurface per buffer, storing the fds in ctx->output_plane_fd
//(freed in nvmpi_encoder_close). NvUtils-only code path.
#if (OUTPLANE_MEMTYPE == OUTPLANE_MEMTYPE_DMA)
int setup_output_dmabuf(nvmpictx *ctx, uint32_t num_buffers )
{
    int ret=0;
    NvBufSurf::NvCommonAllocateParams cParams;
    int fd;
    ret = ctx->enc->output_plane.reqbufs(V4L2_MEMORY_DMABUF,num_buffers);
    if(ret)
    {
        NVMPI_LOG(NVMPI_LOG_ERROR, "reqbufs failed for output plane V4L2_MEMORY_DMABUF");
        return ret;
    }
    for (uint32_t i = 0; i < ctx->enc->output_plane.getNumBuffers(); i++)
    {
        cParams.width = ctx->width;
        cParams.height = ctx->height;
        cParams.layout = NVBUF_LAYOUT_PITCH;

        /*
        switch (ctx->cs)
        {
            case V4L2_COLORSPACE_REC709:
                cParams.colorFormat = ctx->enable_extended_colorformat ?
                    NVBUF_COLOR_FORMAT_YUV420_709_ER : NVBUF_COLOR_FORMAT_YUV420_709;
                break;
            case V4L2_COLORSPACE_SMPTE170M:
            default:
                cParams.colorFormat = ctx->enable_extended_colorformat ?
                    NVBUF_COLOR_FORMAT_YUV420_ER : NVBUF_COLOR_FORMAT_YUV420;
        }
        if (ctx->is_semiplanar)
        {
            cParams.colorFormat = NVBUF_COLOR_FORMAT_NV12;
        }
        if (ctx->encoder_pixfmt == V4L2_PIX_FMT_H264)
        {
            if (ctx->enableLossless)
            {
                if (ctx->is_semiplanar)
                    cParams.colorFormat = NVBUF_COLOR_FORMAT_NV24;
                else
                    cParams.colorFormat = NVBUF_COLOR_FORMAT_YUV444;
            }
        }


        else if (ctx->encoder_pixfmt == V4L2_PIX_FMT_H265)
        {
            if (ctx->chroma_format_idc == 3)
            {
                if (ctx->is_semiplanar)
                    cParams.colorFormat = NVBUF_COLOR_FORMAT_NV24;
                else
                    cParams.colorFormat = NVBUF_COLOR_FORMAT_YUV444;

                if (ctx->bit_depth == 10)
                    cParams.colorFormat = NVBUF_COLOR_FORMAT_NV24_10LE;
            }
            if (ctx->profile == V4L2_MPEG_VIDEO_H265_PROFILE_MAIN10 && (ctx->bit_depth == 10))
            {
                cParams.colorFormat = NVBUF_COLOR_FORMAT_NV12_10LE;
            }
        }
        */

        cParams.colorFormat = NVBUF_COLOR_FORMAT_YUV420;
        cParams.memtag = NvBufSurfaceTag_VIDEO_ENC;
        cParams.memType = NVBUF_MEM_SURFACE_ARRAY;
        // Create output plane fd for DMABUF io-mode
        ret = NvBufSurf::NvAllocate(&cParams, 1, &fd);
        if(ret < 0)
        {
            NVMPI_LOG(NVMPI_LOG_ERROR,
                  "failed to create NvBuffer at index %u/%u; "
                  "rolling back %u already-allocated fds",
                  i, ctx->enc->output_plane.getNumBuffers(), i);
            /* Rollback: destroy fds allocated in previous iterations
             * to prevent DMA-BUF / CMA leak on partial init failure. */
            for (uint32_t j = 0; j < i; j++) {
                if (ctx->output_plane_fd[j] >= 0) {
                    NvBufferDestroy(ctx->output_plane_fd[j]);
                    ctx->output_plane_fd[j] = -1;
                }
            }
            return ret;
        }
        ctx->output_plane_fd[i]=fd;
    }
    return ret;
}
#endif
