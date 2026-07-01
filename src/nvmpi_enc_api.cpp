/*
 * nvmpi_enc_api.cpp — encoder packet/control API of libnvmpi (layer 1).
 *
 * Implements the packet-pool operations, packet retrieval, and control
 * surface (force_idr, set_bitrate, flush, close) of the public C API in
 * include/nvmpi.h on top of NVIDIA's V4L2 NvVideoEncoder sample class.
 * Consumed by the FFmpeg wrapper in ffmpeg/dev/common/libavcodec/nvmpi_enc.c.
 *
 * V4L2 encoder model (M2M device, two queues — note the naming is from the
 * device's point of view, mirrored relative to the decoder):
 *   - OUTPUT plane  = input side: raw YUV frames go in.
 *   - CAPTURE plane = output side: encoded bitstream comes out.
 *
 * Data flow / threading:
 *   DQ thread:        NvVideoEncoder's capture-plane dequeue thread invokes
 *                     encoder_capture_plane_dq_callback() per encoded
 *                     buffer; the callback copies the bitstream into an
 *                     "empty" nvPacket from pktPool and publishes it as
 *                     "filled", then re-queues the V4L2 buffer.
 *   user thread:      nvmpi_encoder_get_packet() pops filled packets;
 *                     the caller returns them via qEmptyPacket().
 *
 * The nvPacket pool itself is filled by the caller (the FFmpeg wrapper
 * allocates packets backed by AVPacket buffers) — libnvmpi never allocates
 * or frees packet memory.
 *
 * Companion files:
 *   nvmpi_enc_internal.h  — shared includes, defines, nvmpictx struct,
 *                           and forward declarations.
 *   nvmpi_enc_init.cpp    — nvmpi_create_encoder() (device open, format
 *                           negotiation, plane/stream setup).
 *   nvmpi_enc_input.cpp   — copyFrameToNvBuf(), nvmpi_encoder_put_frame(),
 *                           nvmpi_encoder_put_frame_fd() (frame submission).
 *   nvmpi_enc_output.cpp  — encoder_capture_plane_dq_callback and
 *                           setup_output_dmabuf (OUTPUT-plane DMA path).
 */
#include "nvmpi_enc_internal.h"

//Public API: take an empty packet out of the pool (e.g. to free or replace
//it). Non-blocking: -1 when the empty queue is exhausted. The caller
//assumes ownership of the returned packet.
int nvmpi_encoder_dqEmptyPacket(nvmpictx* ctx,nvPacket** packet)
{
	nvPacket* pkt = ctx->pktPool->dqEmptyBuf();
	if(!pkt) return -1;
	*packet = pkt;
	return 0;
}

//Public API: donate an empty, caller-allocated packet to the pool so the
//capture DQ callback can fill it. The pool stores only the pointer; the
//caller remains responsible for the packet's eventual deallocation.
void nvmpi_encoder_qEmptyPacket(nvmpictx* ctx,nvPacket* packet)
{
	ctx->pktPool->qEmptyBuf(packet);
	return;
}

//Public API: fetch the next encoded packet.
//While encoding: non-blocking, -1 when nothing is ready (wrapper maps this
//When wait=false: non-blocking, returns -1 immediately if no packet ready
//(maps to EAGAIN). When wait=true: blocks up to wait_timeout_ms using the
//pool's CV-based tiered wait. While flushing: poll every 1ms until either
//a packet arrives (0) or the DQ callback saw the EOS buffer (-2).
//On success the caller holds the packet until re-queueing it via
//nvmpi_encoder_qEmptyPacket().
int nvmpi_encoder_get_packet(nvmpictx* ctx, nvPacket** packet, bool wait)
{
	nvPacket* pkt;

	if (wait)
		pkt = ctx->pktPool->dqFilledBuf(
			std::chrono::milliseconds(ctx->wait_timeout_ms));
	else
		pkt = ctx->pktPool->dqFilledBuf();

	if(!pkt)
	{
		if(!ctx->flushing.load(std::memory_order_acquire)) return -1;
		bool spin = true;
		while(spin)
		{
			pkt = ctx->pktPool->dqFilledBuf();
			if(pkt || ctx->capPlaneGotEOS.load(std::memory_order_acquire)) spin = false;
			else std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		if(!pkt) return -2; //if got eos
	}

	*packet = pkt;
	return 0;
}

//Public API: force the next encoded frame to be an IDR (keyframe on demand).
//Calls NvVideoEncoder::forceIDR() which sets the V4L2 control
//V4L2_CID_MPEG_MFC51_VIDEO_FORCE_FRAME_TYPE. Safe to call from the user
//thread while the DQ thread is running — the V4L2 ioctl is thread-safe.
//Returns 0 on success, -1 on ioctl failure.
int nvmpi_encoder_force_idr(nvmpictx* ctx)
{
	int ret = ctx->enc->forceIDR();
	if (ret < 0) {
		NVMPI_LOG(NVMPI_LOG_ERROR, "Could not force IDR frame");
		return -1;
	}
	return 0;
}

//Public API: change the encoder target bitrate mid-stream.
//Calls NvVideoEncoder::setBitrate() which updates V4L2_CID_MPEG_VIDEO_BITRATE.
//The new bitrate takes effect from the next encoded frame. Also updates
//ctx->bitrate so the internal state stays consistent. For VBR mode the caller
//should also adjust peak_bitrate externally if needed.
//Returns 0 on success, -1 on ioctl failure.
int nvmpi_encoder_set_bitrate(nvmpictx* ctx, unsigned int bitrate)
{
	int ret = ctx->enc->setBitrate(bitrate);
	if (ret < 0) {
		NVMPI_LOG(NVMPI_LOG_ERROR, "Could not set bitrate to %u", bitrate);
		return -1;
	}
	/* Keep internal state in sync; freed in nvmpi_encoder_close(). */
	ctx->bitrate = bitrate;
	return 0;
}

//Public API: stop the encoder and free the context.
//Mid-stream flush: reset the V4L2 encoder pipeline so encoding can continue
//after a seek or stream restart without closing/reopening the codec context.
//
//Sequence (mirrors the decoder flush in nvmpi_dec_api.cpp):
//  1. Stop the DQ thread — prevents callbacks during STREAMOFF.
//  2. STREAMOFF both planes — tears down V4L2 queues, returns all buffers.
//  3. Drain the packet pool — discard any pending encoded packets by moving
//     them from the filled queue back to the empty queue.
//  4. Reset flushing/EOS atomics — re-enable put_frame/get_packet.
//  5. STREAMON both planes — restart the V4L2 pipeline.
//  6. Re-queue capture plane buffers — the encoder needs empty buffers to
//     write encoded output into.
//  7. Restart the DQ thread — resume capture-plane processing.
//  8. Reset the output-plane buffer index — next put_frame starts from 0.
//
//The caller (FFmpeg wrapper) is responsible for resetting its own
//encoder_flushing flag and forcing the next frame to IDR if needed.
int nvmpi_encoder_flush(nvmpictx* ctx)
{
	int ret;

	/* 1. Stop DQ thread — must complete before STREAMOFF to avoid
	 *    callbacks on torn-down queues. */
	if (ctx->blocking_mode && ctx->dq_thread_started) {
		ctx->enc->capture_plane.stopDQThread();
		ctx->enc->capture_plane.waitForDQThread(1000);
		ctx->dq_thread_started = false;
	}

	/* 2. STREAMOFF both planes — returns all queued buffers. */
	ctx->enc->capture_plane.setStreamStatus(false);
	ctx->enc->output_plane.setStreamStatus(false);

	/* 3. Drain packet pool: move filled packets back to empty queue.
	 *    The pool must be reset() first to clear any shutdown state
	 *    from a prior EOS drain, then drained. */
	ctx->pktPool->reset();
	{
		nvPacket* pkt;
		while ((pkt = ctx->pktPool->dqFilledBuf()))
			ctx->pktPool->qEmptyBuf(pkt);
	}

	/* 4. Reset EOS/flushing atomics so put_frame/get_packet work again. */
	ctx->flushing.store(false, std::memory_order_release);
	ctx->capPlaneGotEOS.store(false, std::memory_order_release);

	/* 5. STREAMON both planes — restart the V4L2 pipeline. */
	ret = ctx->enc->output_plane.setStreamStatus(true);
	if (ret < 0) {
		NVMPI_LOG(NVMPI_LOG_ERROR, "Error in output plane streamon after flush");
		return ret;
	}

	ret = ctx->enc->capture_plane.setStreamStatus(true);
	if (ret < 0) {
		NVMPI_LOG(NVMPI_LOG_ERROR, "Error in capture plane streamon after flush");
		return ret;
	}

	/* 6. Re-queue all capture plane buffers so the encoder has
	 *    somewhere to write encoded output. */
	for (uint32_t i = 0; i < ctx->enc->capture_plane.getNumBuffers(); i++) {
		struct v4l2_buffer v4l2_buf;
		struct v4l2_plane planes[MAX_PLANES];
		memset(&v4l2_buf, 0, sizeof(v4l2_buf));
		memset(planes, 0, MAX_PLANES * sizeof(struct v4l2_plane));

		v4l2_buf.index = i;
		v4l2_buf.m.planes = planes;

		ret = ctx->enc->capture_plane.qBuffer(v4l2_buf, NULL);
		if (ret < 0) {
			NVMPI_LOG(NVMPI_LOG_ERROR,
				  "Error while queueing buffer at capture plane after flush (buf %u)", i);
			return ret;
		}
	}

	/* 7. Restart DQ thread — resume capture-plane processing. */
	if (ctx->blocking_mode) {
		ctx->enc->capture_plane.setDQThreadCallback(encoder_capture_plane_dq_callback);
		ret = ctx->enc->capture_plane.startDQThread(ctx);
		if (ret < 0) {
			NVMPI_LOG(NVMPI_LOG_ERROR, "Could not restart DQ thread after flush");
			return ret;
		}
		ctx->dq_thread_started = true;
	}

	/* 8. Reset output-plane buffer index — next put_frame starts
	 *    from buffer 0 (first N frames use getNthBuffer directly). */
	ctx->index = 0;

	return 0;
}

//Stops/joins the capture DQ thread, releases self-allocated OUTPUT-plane
//dmabufs (DMA mode only), then destroys the device and the (by now empty)
//packet pool. Packets still inside the pool are NOT freed here — the
//FFmpeg wrapper drains and frees them first (nvmpienc_deinitPktPool).
int nvmpi_encoder_close(nvmpictx* ctx)
{
	/* Shutdown the packet pool to unblock any thread waiting in
	 * dqFilledBuf(timeout) — prevents hangs on early termination
	 * (e.g. ffmpeg -t) when blocking wait is active. */
	if (ctx->pktPool)
		ctx->pktPool->shutdown();

	/* Guard: only stop/join the DQ thread if it was actually started.
	 * A half-initialized encoder (create failed after STREAMON but before
	 * startDQThread) or a non-blocking-mode encoder never starts the DQ
	 * thread. Calling stopDQThread/waitForDQThread on an un-started
	 * thread invokes pthread_join on an uninitialized handle → UB/crash. */
	if(ctx->blocking_mode && ctx->dq_thread_started)
	{
		ctx->enc->capture_plane.stopDQThread();
		ctx->enc->capture_plane.waitForDQThread(1000);
	}
	else
	{
		//sem_destroy(&ctx.pollthread_sema);
		//sem_destroy(&ctx.encoderthread_sema);
	}

	/* Cleanup internal DMABUF surfaces. Applies to both compile-time
	 * DMA mode (OUTPLANE_MEMTYPE_DMA) and runtime external DMABUF mode
	 * (dmabuf_external). Check fd validity (-1 = unallocated slot). */
	if (ctx->enc && ctx->output_plane_fd) {
		for (uint32_t i = 0; i < ctx->enc->output_plane.getNumBuffers(); i++) {
			if (ctx->output_plane_fd[i] >= 0) {
				ctx->enc->output_plane.unmapOutputBuffers(i,
					ctx->output_plane_fd[i]);
				NvBufferDestroy(ctx->output_plane_fd[i]);
				ctx->output_plane_fd[i] = -1;
			}
		}
	}
	delete[] ctx->output_plane_fd;
	ctx->output_plane_fd = nullptr;

	/* unique_ptr: release the NvVideoEncoder after the DQ thread is
	 * joined and all V4L2 buffers are unmapped. */
	ctx->enc.reset();
	delete ctx->pktPool;
	delete ctx;
	return 0;
}
