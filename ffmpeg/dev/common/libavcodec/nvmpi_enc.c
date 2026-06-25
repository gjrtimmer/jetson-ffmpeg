/*
 * nvmpi_enc.c — FFmpeg encoder wrapper for libnvmpi (layer 2 of
 * jetson-ffmpeg, the FFmpeg integration layer).
 *
 * Like nvmpi_dec.c, this file is patched into a vanilla FFmpeg tree and
 * compiled inside FFmpeg. It implements the h264_nvmpi and hevc_nvmpi
 * encoders by delegating to the libnvmpi C API (src/nvmpi_enc.cpp).
 *
 * Key responsibility beyond plain forwarding: packet memory pooling.
 * libnvmpi's encoder never allocates packet memory — this wrapper
 * pre-allocates nvPackets whose payload is backed by real AVPacket buffers
 * (so delivering a packet to FFmpeg is a zero-copy move), queues them into
 * the encoder's empty pool, and recycles them after each
 * avcodec_receive_packet().
 *
 * One source supports FFmpeg 6.0 .. 8.0+ via preprocessor guards:
 *  - LIBAVCODEC_VERSION_MAJOR >= 60: FFCodec registration +
 *    ff_get_encode_buffer replaces ff_alloc_packet2.
 *  - lavc >= 62.11 (FFmpeg 8.0): FF_PROFILE_* renamed to AV_PROFILE_*.
 * See https://github.com/gjrtimmer/jetson-ffmpeg/wiki/Development-Guide "Wrapper code paths by FFmpeg version".
 */
#include <nvmpi.h>
#include "avcodec.h"
#include "internal.h"
#include <stdio.h>
#include "libavutil/avstring.h"
#include "libavutil/avutil.h"
#include "libavutil/common.h"
#include "libavutil/imgutils.h"
#include "libavutil/log.h"
#include "libavutil/opt.h"
#include "libavutil/mem.h"

#include "version.h"

//compatibility with ffmpeg 8.0+. FF_PROFILE renamed with AV_PROFILE
//(libavcodec 62.11 deprecated the FF_PROFILE_* names; mapping the old
//spellings here lets the rest of the file use FF_PROFILE_* everywhere.
//The ">= 100" clause is future-proofing in case the minor resets.)
#if (LIBAVCODEC_VERSION_MAJOR >= 62 && LIBAVCODEC_VERSION_MINOR>= 11) || (LIBAVCODEC_VERSION_MAJOR >= 100)
#define FF_PROFILE_H264_INTRA AV_PROFILE_H264_INTRA
#define FF_PROFILE_UNKNOWN AV_PROFILE_UNKNOWN
#define FF_PROFILE_H264_HIGH AV_PROFILE_H264_HIGH
#define FF_PROFILE_H264_BASELINE AV_PROFILE_H264_BASELINE
#define FF_PROFILE_H264_MAIN AV_PROFILE_H264_MAIN
#endif

#include "encode.h"
#include "codec_internal.h"

//libnvmpi exchanges timestamps in microseconds; used with av_rescale_q to
//convert to/from the stream's AVCodecContext time_base.
static const AVRational NVENC_TIMEBASE = {1, 1000000};

//valid range / default for the wait_timeout AVOption: how long (ms)
//nvmpi_encoder_get_packet() blocks in low-delay mode. Matches decoder.
#define OPT_wait_timeout_AUTO 0
#define OPT_wait_timeout_MAX  5000

//valid range / default for the packet_pool_size AVOption: how many encoded
//packets may pile up before the libnvmpi DQ thread starts dropping output.
#define OPT_packet_pool_size_MIN 1
#define OPT_packet_pool_size_MAX 32
#define OPT_packet_pool_size_DEFAULT 10

//Per-instance private context (priv_data of the AVCodecContext). The int
//option fields are populated from the AVOption table before .init runs.
typedef struct {
	const AVClass *class;       //must be first for the AVOption system
	nvmpictx* ctx;              //libnvmpi encoder handle
	int num_capture_buffers;    //V4L2 buffer count option
	int packet_pool_size;       //packet pool depth option
	int profile;                //FF_PROFILE_H264_* numeric value or UNKNOWN
	int level;                  //level option (10*level, 0 = auto)
	int rc;                     //rate control option: -1 default, 0 CBR, 1 VBR
	int preset;                 //hw preset option: 1=ultrafast .. 4=slow
	int max_perf;               //lift NVENC clock governor (default on)
	int poc_type;               //H.264 picture order count type (0=default, 2=low-latency)
	int insert_vui;             //embed VUI timing_info (fps) in the bitstream (default on)
	int insert_aud;             //insert Access Unit Delimiter NALs
	int cabac;                  //enable CABAC entropy coding (H.264 only)
	int lossless;               //enable lossless encoding (H.264 only, QP 0 + High 4:4:4)
	int wait_timeout;           //blocking-wait ceiling in ms (0 = default 500ms)
	int encoder_flushing;       //set after EOS was sent to libnvmpi
	int64_t last_bitrate;       //track last-set bitrate for dynamic change detection
	AVFrame *frame; //tmp frame
	                //(holds the pulled-but-not-yet-sent input frame in the
	                // new-API receive_packet path)
}nvmpiEncodeContext;

nvPacket* nvmpienc_nvPacket_alloc(AVCodecContext *avctx, int bufSize);
void nvmpienc_nvPacket_free(nvPacket* nPkt);
int nvmpienc_nvPacket_reset(nvPacket* nPkt, AVCodecContext *avctx, int bufSize);
int nvmpienc_initPktPool(AVCodecContext *avctx, int pktNum);
int nvmpienc_deinitPktPool(AVCodecContext *avctx);

//alloc nvPacket and AVPacket buffer;
//Creates one pool packet: an nvPacket whose payload points into a real
//AVPacket data buffer (kept in privData). This is what makes the later
//handoff to FFmpeg zero-copy. Returns NULL on allocation failure.
//ff_get_encode_buffer() is how encoders obtain packet buffers (libavcodec 60+).
nvPacket* nvmpienc_nvPacket_alloc(AVCodecContext *avctx, int bufSize)
{
	AVPacket* pkt = av_packet_alloc();
	nvPacket* nPkt = (nvPacket*)malloc(sizeof(nvPacket));
	int res;
	memset(nPkt, 0, sizeof(nvPacket));
	if((res = ff_get_encode_buffer(avctx, pkt, bufSize, 0)))
	{
		av_packet_free(&pkt);
		free(nPkt);
		return NULL;
	}
	nPkt->privData = pkt;
	nPkt->payload = pkt->data;
	return nPkt;
}

//Free one pool packet: both the backing AVPacket and the nvPacket shell.
void nvmpienc_nvPacket_free(nvPacket* nPkt)
{
	AVPacket* pkt = nPkt->privData;
	av_packet_free(&pkt);
	free(nPkt);
}

//Re-arm a pool packet after its previous buffer was moved into the user's
//AVPacket: acquire a fresh encode buffer for the (now empty) AVPacket and
//clear the nvPacket bookkeeping. Returns 0 or -1 on allocation failure.
int nvmpienc_nvPacket_reset(nvPacket* nPkt, AVCodecContext *avctx, int bufSize)
{
	AVPacket* pkt = nPkt->privData;
	int res;
	if((res = ff_get_encode_buffer(avctx, pkt, bufSize, 0)))
	{
		return -1;
	}
	nPkt->payload = pkt->data;
	nPkt->payload_size = 0;
	nPkt->flags = 0;
	nPkt->pts = 0;
	return 0;
}

//must call after nvmpi_create_encoder() to preallocate buffers
//Seeds libnvmpi's empty-packet pool with pktNum packets of
//NVMPI_ENC_CHUNK_SIZE each; the encoder's DQ thread fills them as encoded
//frames appear.
int nvmpienc_initPktPool(AVCodecContext *avctx, int pktNum)
{
	nvmpiEncodeContext * nvmpi_context = avctx->priv_data;
	for(int i=0;i<pktNum;i++)
	{
		nvPacket* nPkt = nvmpienc_nvPacket_alloc(avctx, NVMPI_ENC_CHUNK_SIZE);
		if (!nPkt)
		{
			/* Allocation failed mid-pool: drain and free all previously
			 * queued packets to avoid a partially populated pool. */
			av_log(avctx, AV_LOG_ERROR,
			       "nvmpi: packet pool allocation failed at %d/%d\n", i, pktNum);
			nvmpienc_deinitPktPool(avctx);
			return AVERROR(ENOMEM);
		}
		nvmpi_encoder_qEmptyPacket(nvmpi_context->ctx, nPkt);
	}
	return 0;
}

//must call before nvmpi_encoder_close() too free buffers memory
//Drains BOTH pool queues (empty and filled) and frees every packet, since
//libnvmpi itself never deallocates packet memory. Safe only after the
//encoder has been fully drained (no packet checked out by the DQ thread).
int nvmpienc_deinitPktPool(AVCodecContext *avctx)
{
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

//AVCodec/FFCodec .init callback: translate AVCodecContext settings and
//AVOptions into an nvEncParam, optionally pre-generate global extradata
//(SPS/PPS), then create the real libnvmpi encoder and seed its packet
//pool. The numeric profile/level/rc/preset conventions used here are
//decoded on the libnvmpi side (src/nvmpi_enc.cpp).
static av_cold int nvmpi_encode_init(AVCodecContext *avctx)
{
	nvmpiEncodeContext * nvmpi_context = avctx->priv_data;

	nvEncParam param={0};

	param.width=avctx->width;
	param.height=avctx->height;
	param.bitrate=avctx->bit_rate;
	param.vbv_buffer_size = avctx->rc_buffer_size;
	//TODO use rc_initial_buffer_occupancy or ignore?
	param.mode_vbr=0;
	param.idr_interval=60;
	param.iframe_interval=30;
	param.peak_bitrate=avctx->rc_max_rate;
	param.fps_n=avctx->framerate.num;
	param.fps_d=avctx->framerate.den;
	//strip the INTRA flag bit so only the base profile id is passed down
	param.profile=nvmpi_context->profile& ~FF_PROFILE_H264_INTRA;
	param.level=nvmpi_context->level;
	param.capture_num=nvmpi_context->num_capture_buffers;
	//param.packet_pool_size=nvmpi_context->packet_pool_size;
	param.hw_preset_type=nvmpi_context->preset;
	param.max_perf=nvmpi_context->max_perf;
	param.poc_type=nvmpi_context->poc_type;
	//with GLOBAL_HEADER the SPS/PPS live in extradata (generated below)
	//instead of being repeated in-band at every IDR
	param.insert_spspps_idr=(avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER)?0:1;
	param.insert_vui=nvmpi_context->insert_vui;
	param.insert_aud=nvmpi_context->insert_aud;
	param.enable_cabac=nvmpi_context->cabac;
	param.enableLossless=nvmpi_context->lossless;
	param.wait_timeout=nvmpi_context->wait_timeout;

	//Raw input layout: NV12 routes to libnvmpi's V4L2 NV12M path, otherwise
	//planar YUV420. Full-range (YUVJ420P / MJPEG) input is intentionally NOT
	//advertised in pix_fmts: the Jetson V4L2 encoder exposes no API to set
	//the VUI video_full_range_flag, so it always emits limited-range. Letting
	//FFmpeg's swscale convert full->limited range before the encoder yields
	//correct levels; passing full-range pixels through under a limited-range
	//flag would not (the latent bug in the bradcagle/xsacha forks).
	param.pixFormat = (avctx->pix_fmt == AV_PIX_FMT_NV12) ? NV_PIX_NV12 : NV_PIX_YUV420;

	nvmpi_context->frame = av_frame_alloc();
	if (!nvmpi_context->frame) return AVERROR(ENOMEM);

	if(nvmpi_context->rc==1){
		param.mode_vbr=1;
	}

	if(avctx->qmin >= 0 && avctx->qmax >= 0){
		param.qmin=avctx->qmin;
		param.qmax=avctx->qmax;
	}

	if (avctx->refs >= 0){
		param.refs=avctx->refs;

	}

	if(avctx->max_b_frames > 0 && avctx->max_b_frames < 3){
		param.max_b_frames=avctx->max_b_frames;
	}

	if(avctx->gop_size>0){
		param.idr_interval=param.iframe_interval=avctx->gop_size;

	}

	//TODO should replace it. must gen extradata directly without calling for encoder
	//Extradata generation for GLOBAL_HEADER: spin up a THROWAWAY encoder
	//instance, encode one blank frame, and scan the resulting bitstream
	//for the first IDR NAL — everything before it (SPS/PPS, VPS for HEVC)
	//becomes avctx->extradata. The temporary encoder is then drained and
	//destroyed; the real encoder is created afterwards.
	if ((avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER) && (avctx->codec->id == AV_CODEC_ID_H264 || avctx->codec->id == AV_CODEC_ID_H265))
	{
		uint8_t *dst[4];
		int linesize[4];
		nvFrame _nvframe={0};
		nvPacket *nPkt;
		nvmpictx *_ctx;
		int i;
		int ret;
		int64_t shiftPts = 1000000/param.fps_n;
		if(avctx->codec->id == AV_CODEC_ID_H264) param.codingType = NV_VIDEO_CodingH264;
		else param.codingType = NV_VIDEO_CodingHEVC;
		av_image_alloc(dst, linesize,avctx->width,avctx->height,avctx->pix_fmt,1);

		nvmpi_context->ctx = nvmpi_create_encoder(&param);
		_ctx = nvmpi_context->ctx;
		if (!_ctx)
		{
			av_freep(&dst[0]);
			av_frame_free(&nvmpi_context->frame);
			/* AVERROR_EXTERNAL: device-level failure (EBUSY, ENODEV, etc.),
			 * not a memory allocation error. See #37. */
			return AVERROR_EXTERNAL;
		}
		nvmpienc_initPktPool(avctx,nvmpi_context->packet_pool_size);
		i=0;
		_nvframe.timestamp=0;

		while(true)
		{
			_nvframe.payload[0]=dst[0];
			_nvframe.payload[1]=dst[1];
			_nvframe.payload[2]=dst[2];
			_nvframe.payload_size[0]=linesize[0]*avctx->height;
			_nvframe.payload_size[1]=linesize[1]*avctx->height/2;
			_nvframe.payload_size[2]=linesize[2]*avctx->height/2;
			_nvframe.linesize[0]=linesize[0];
			_nvframe.linesize[1]=linesize[1];
			_nvframe.linesize[2]=linesize[2];
			_nvframe.timestamp+=shiftPts;

			nvmpi_encoder_put_frame(_ctx,&_nvframe);

			ret=nvmpi_encoder_get_packet(_ctx,&nPkt, avctx->flags & AV_CODEC_FLAG_LOW_DELAY);

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
					if(param.codingType == NV_VIDEO_CodingH264)
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
	}

	//create the real encoder used for the rest of the session
	if(avctx->codec->id == AV_CODEC_ID_H264)
	{
		param.codingType = NV_VIDEO_CodingH264;
		nvmpi_context->ctx=nvmpi_create_encoder(&param);
	}
	else if(avctx->codec->id == AV_CODEC_ID_HEVC)
	{
		param.codingType = NV_VIDEO_CodingHEVC;
		nvmpi_context->ctx=nvmpi_create_encoder(&param);
	}
	//else TODO
	
	if(!nvmpi_context->ctx)
	{
		av_log(avctx, AV_LOG_ERROR, "nvmpi: encoder creation failed\n");
		av_frame_free(&nvmpi_context->frame);
		/* AVERROR_EXTERNAL: device-level failure (EBUSY, ENODEV, etc.),
		 * not a memory allocation error. See #37. */
		return AVERROR_EXTERNAL;
	}
	nvmpienc_initPktPool(avctx,nvmpi_context->packet_pool_size);

	/* Track initial bitrate for runtime change detection in send_frame. */
	nvmpi_context->last_bitrate = avctx->bit_rate;

	return 0;
}

//send_frame half of the encode API: wrap the AVFrame's planes in an
//nvFrame (no copy here — libnvmpi memcpy's into its V4L2 buffer inside
//put_frame, which may block briefly) and rescale pts to microseconds.
//frame==NULL initiates flushing (EOS to libnvmpi). Called internally by
//ff_nvmpi_receive_packet_async().
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

		_nvframe.payload[0]=frame->data[0];
		_nvframe.payload[1]=frame->data[1];
		_nvframe.payload[2]=frame->data[2];

		_nvframe.payload_size[0]=frame->linesize[0]*frame->height;
		_nvframe.payload_size[1]=frame->linesize[1]*frame->height/2;
		_nvframe.payload_size[2]=frame->linesize[2]*frame->height/2;

		_nvframe.linesize[0]=frame->linesize[0];
		_nvframe.linesize[1]=frame->linesize[1];
		_nvframe.linesize[2]=frame->linesize[2];

		//_nvframe.timestamp=frame->pts;
		_nvframe.timestamp=av_rescale_q(frame->pts, avctx->time_base, NVENC_TIMEBASE);
		//_nvframe.timestamp=frame->pts*avctx->time_base.num*1000*1000/avctx->time_base.den;

		res=nvmpi_encoder_put_frame(nvmpi_context->ctx,&_nvframe);

		if(res<0)
			return res;
	}
	else
	{
		nvmpi_context->encoder_flushing = 1;
		nvmpi_encoder_put_frame(nvmpi_context->ctx,NULL);
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
	 * waiting for the DQ thread to deliver final encoded packets. */
	res = nvmpi_encoder_get_packet(nvmpi_context->ctx, &nPkt,
		(avctx->flags & AV_CODEC_FLAG_LOW_DELAY) || nvmpi_context->encoder_flushing);
	if(res<0)
	{
		/* -2 = actual EOS from the V4L2 DQ thread (all packets drained).
		 * -1 = no packet ready yet (DQ thread still processing); return
		 * EAGAIN so FFmpeg keeps calling receive_packet during flush.
		 * Previously, -1 during flushing was treated as EOF, causing
		 * final packets to be dropped (see #15). */
		if(res == -2) return AVERROR_EOF;
		return AVERROR(EAGAIN);
	}
	
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
static int ff_nvmpi_receive_packet_async(AVCodecContext *avctx, AVPacket *pkt)
{
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
	
	if(needSendFrame)
	{
		res = ff_nvmpi_send_frame(avctx, frame);
		if (res < 0)
		{
			if(res != AVERROR_EOF && res != AVERROR(EAGAIN)) return res;
		}
		else av_frame_unref(frame);
	}

	res = ff_nvmpi_receive_packet(avctx, pkt);
	if(res<0) return res;

	return 0;
}

//AVCodec/FFCodec .flush callback: reset the V4L2 encoder pipeline for
//mid-stream restart (e.g. after seek). Resets the libnvmpi encoder
//(STREAMOFF/STREAMON, drain packet pool, restart DQ thread) and clears
//the FFmpeg-side flushing flag so send_frame accepts new input.
static void nvmpi_flush_encoder(AVCodecContext *avctx)
{
    nvmpiEncodeContext *nvmpi_context = avctx->priv_data;

    /* Reset the libnvmpi encoder pipeline. */
    nvmpi_encoder_flush(nvmpi_context->ctx);

    /* Clear the FFmpeg-side flushing flag so send_frame accepts frames. */
    nvmpi_context->encoder_flushing = 0;
}

//AVCodec/FFCodec .close callback: make sure EOS was sent, drain remaining
//packets (recycling pool buffers while doing so), then free the pool and
//close the libnvmpi encoder. Order matters — the pool must be drained
//before nvmpi_encoder_close(), which does not free packet memory.
static av_cold int nvmpi_encode_close(AVCodecContext *avctx)
{
	nvmpiEncodeContext *nvmpi_context = avctx->priv_data;

	//drain encoder
	{
		int ret;
		nvPacket *nPkt;
		if(!nvmpi_context->encoder_flushing)
		{
			nvmpi_context->encoder_flushing = 1;
			nvmpi_encoder_put_frame(nvmpi_context->ctx,NULL);
		}
		
		while(1)
		{
			ret=nvmpi_encoder_get_packet(nvmpi_context->ctx, &nPkt, false);
			if(ret < 0)
			{
				if(ret == -2) break; //got eos
				continue;
			}
			nvmpienc_nvPacket_free(nPkt);
			nPkt = nvmpienc_nvPacket_alloc(avctx, NVMPI_ENC_CHUNK_SIZE);
			if (!nPkt) break; /* OOM during close — stop draining */
			nvmpi_encoder_qEmptyPacket(nvmpi_context->ctx, nPkt);
		}
	}

	nvmpienc_deinitPktPool(avctx);
	nvmpi_encoder_close(nvmpi_context->ctx);
	av_frame_free(&nvmpi_context->frame);

	return 0;
}

//Default values for GENERIC AVCodecContext options (not the private
//AVOptions below): 2M bitrate, GOP 50, no B-frames, qmin/qmax unset, etc.
//Only the struct's name changed with the FFCodec split in libavcodec 60;
//the entries are identical.
static const FFCodecDefault defaults[] = {
	{ "b", "2M" },
	{ "qmin", "-1" },
	{ "qmax", "-1" },
	{ "qdiff", "-1" },
	{ "qblur", "-1" },
	{ "qcomp", "-1" },
	{ "g", "50" },
	{ "bf", "0" },
	{ "refs", "0" },
	{ NULL },
};


//AVOption table: private "-x264-style" options resolved into
//nvmpiEncodeContext fields before .init. AV_OPT_TYPE_CONST entries are
//named aliases for values of the preceding INT option (grouped by the
//trailing unit string, e.g. "profile"). The numeric values intentionally
//match what src/nvmpi_enc.cpp's switch statements expect: FFmpeg profile
//ids, level_idc*10, rc 0/1 = CBR/VBR, preset 1..4.
#define OFFSET(x) offsetof(nvmpiEncodeContext, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM

static const AVOption options[] = {
	{ "num_capture_buffers", "Number of buffers in the capture context", OFFSET(num_capture_buffers), AV_OPT_TYPE_INT, {.i64 = 10 }, 1, 32, VE, "num_capture_buffers" },
	{ "packet_pool_size", "Number of packets that could be buffered in the encoder before user must read it with avcodec_receive_packet()", OFFSET(packet_pool_size), AV_OPT_TYPE_INT, {.i64 = OPT_packet_pool_size_DEFAULT }, OPT_packet_pool_size_MIN, OPT_packet_pool_size_MAX, VE, "packet_pool_size" },
	/// Profile,

	{ "profile",  "Set the encoding profile", OFFSET(profile), AV_OPT_TYPE_INT,   { .i64 = FF_PROFILE_UNKNOWN },       FF_PROFILE_UNKNOWN, FF_PROFILE_H264_HIGH, VE, "profile" },
	{ "baseline", "",                         0,               AV_OPT_TYPE_CONST, { .i64 = FF_PROFILE_H264_BASELINE }, 0, 0, VE, "profile" },
	{ "main",     "",                         0,               AV_OPT_TYPE_CONST, { .i64 = FF_PROFILE_H264_MAIN },     0, 0, VE, "profile" },
	{ "high",     "",                         0,               AV_OPT_TYPE_CONST, { .i64 = FF_PROFILE_H264_HIGH },     0, 0, VE, "profile" },

	/// Profile Level (shared by H.264 and HEVC; level_idc convention: 51 = 5.1)
	{ "level",          "Encoding level (e.g. 4.1, 5.1)",  OFFSET(level),  AV_OPT_TYPE_INT,   { .i64 = 0  }, 0, 62, VE, "level" },
	{ "auto",           "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 0  }, 0, 0,  VE, "level" },
	{ "1.0",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 10 }, 0, 0,  VE, "level" },
	{ "1.1",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 11 }, 0, 0,  VE, "level" },
	{ "1.2",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 12 }, 0, 0,  VE, "level" },
	{ "1.3",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 13 }, 0, 0,  VE, "level" },
	{ "2.0",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 20 }, 0, 0,  VE, "level" },
	{ "2.1",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 21 }, 0, 0,  VE, "level" },
	{ "2.2",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 22 }, 0, 0,  VE, "level" },
	{ "3.0",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 30 }, 0, 0,  VE, "level" },
	{ "3.1",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 31 }, 0, 0,  VE, "level" },
	{ "3.2",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 32 }, 0, 0,  VE, "level" },
	{ "4.0",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 40 }, 0, 0,  VE, "level" },
	{ "4.1",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 41 }, 0, 0,  VE, "level" },
	{ "4.2",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 42 }, 0, 0,  VE, "level" },
	{ "5.0",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 50 }, 0, 0,  VE, "level" },
	{ "5.1",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 51 }, 0, 0,  VE, "level" },
	{ "5.2",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 52 }, 0, 0,  VE, "level" },
	{ "6.0",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 60 }, 0, 0,  VE, "level" },
	{ "6.1",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 61 }, 0, 0,  VE, "level" },
	{ "6.2",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 62 }, 0, 0,  VE, "level" },

	{ "rc",           "Override the preset rate-control",   OFFSET(rc),           AV_OPT_TYPE_INT,   { .i64 = -1 },                                  -1, INT_MAX, VE, "rc" },
	{ "cbr",          "Constant bitrate mode",              0,                    AV_OPT_TYPE_CONST, { .i64 = 0 },                       0, 0, VE, "rc" },
	{ "vbr",          "Variable bitrate mode",              0,                    AV_OPT_TYPE_CONST, { .i64 = 1 },                       0, 0, VE, "rc" },

	{ "preset",          "Set the encoding preset",            OFFSET(preset),       AV_OPT_TYPE_INT,   { .i64 = 3 }, 1, 4, VE, "preset" },
	{ "default",         "",                                   0,                    AV_OPT_TYPE_CONST, { .i64 = 3 }, 0, 0, VE, "preset" },
	{ "slow",            "",                        0,                    AV_OPT_TYPE_CONST, { .i64 = 4 },            0, 0, VE, "preset" },
	{ "medium",          "",                        0,                    AV_OPT_TYPE_CONST, { .i64 = 3 },            0, 0, VE, "preset" },
	{ "fast",            "",                        0,                    AV_OPT_TYPE_CONST, { .i64 = 2 },            0, 0, VE, "preset" },
	{ "ultrafast",       "",                        0,                    AV_OPT_TYPE_CONST, { .i64 = 1 },            0, 0, VE, "preset" },

	{ "max_perf", "Enable max performance mode (lifts NVENC clock governor)", OFFSET(max_perf), AV_OPT_TYPE_BOOL, {.i64 = 1 }, 0, 1, VE, "max_perf" },
	{ "poc_type", "H.264 picture order count type (0=default, 2=decode-order for low-latency)", OFFSET(poc_type), AV_OPT_TYPE_INT, {.i64 = 0 }, 0, 2, VE, "poc_type" },
	{ "insert_vui", "Embed VUI timing_info (fps) in the bitstream so players/muxers report the frame rate", OFFSET(insert_vui), AV_OPT_TYPE_BOOL, {.i64 = 1 }, 0, 1, VE, "insert_vui" },
	{ "aud", "Insert Access Unit Delimiter NALs (required by some TS/HLS workflows)", OFFSET(insert_aud), AV_OPT_TYPE_BOOL, {.i64 = 0 }, 0, 1, VE, "aud" },
	{ "cabac", "Enable CABAC entropy coding (H.264 only; ~10-15%% better compression than CAVLC)", OFFSET(cabac), AV_OPT_TYPE_BOOL, {.i64 = 0 }, 0, 1, VE, "cabac" },
	{ "lossless", "Enable lossless encoding (H.264 only, constant QP 0 + High 4:4:4 Predictive profile)", OFFSET(lossless), AV_OPT_TYPE_BOOL, {.i64 = 0 }, 0, 1, VE, "lossless" },
	{ "wait_timeout", "Blocking wait timeout in milliseconds for low-delay mode (0 = default 500ms)", OFFSET(wait_timeout), AV_OPT_TYPE_INT, {.i64 = OPT_wait_timeout_AUTO }, 0, OPT_wait_timeout_MAX, VE, "wait_timeout" },
	{ NULL }
};


//Per-codec AVClass binding the option table above to each encoder
//instance (referenced via priv_class in the registration struct).
#define NVMPI_ENC_CLASS(NAME) \
	static const AVClass nvmpi_ ## NAME ## _enc_class = { \
		.class_name = #NAME "_nvmpi_encoder", \
		.item_name  = av_default_item_name, \
		.option     = options, \
		.version    = LIBAVUTIL_VERSION_INT, \
	};


//Codec registration, stamped out per codec by NVMPI_ENC(). Covers paths C/D:
//  - lavc >= 60 (FFmpeg 6.0–7.x, path C): FFCodec struct, pull-based receive_packet.
//  - lavc >= 62.11 (FFmpeg 8.0, path D): same, plus AV_PROFILE_* aliases above.
//Matching allcodecs.c extern is "extern const FFCodec".
#define NVMPI_ENC(NAME, LONGNAME, CODEC) \
	NVMPI_ENC_CLASS(NAME) \
	const FFCodec ff_ ## NAME ## _nvmpi_encoder = { \
		.p.name           = #NAME "_nvmpi" , \
		CODEC_LONG_NAME("nvmpi " LONGNAME " encoder wrapper"), \
		.p.type           = AVMEDIA_TYPE_VIDEO, \
		.p.id             = CODEC , \
		.priv_data_size = sizeof(nvmpiEncodeContext), \
		.p.priv_class     = &nvmpi_ ## NAME ##_enc_class, \
		.init           = nvmpi_encode_init, \
		FF_CODEC_RECEIVE_PACKET_CB(ff_nvmpi_receive_packet_async), \
		.close          = nvmpi_encode_close, \
		.flush          = nvmpi_flush_encoder, \
		.p.pix_fmts       = (const enum AVPixelFormat[]) { AV_PIX_FMT_YUV420P, AV_PIX_FMT_NV12, AV_PIX_FMT_NONE },\
		.p.capabilities   = AV_CODEC_CAP_HARDWARE | AV_CODEC_CAP_DELAY, \
		.defaults       = defaults,\
		.p.wrapper_name   = "nvmpi", \
	};

//Instantiate the two nvmpi encoders. Each expansion must have a matching
//extern in allcodecs.c and CONFIG_*_NVMPI_ENCODER Makefile/configure
//entries (added by ffpatch.sh / the version overlays).
NVMPI_ENC(h264, "H.264", AV_CODEC_ID_H264);
NVMPI_ENC(hevc, "HEVC", AV_CODEC_ID_HEVC);
