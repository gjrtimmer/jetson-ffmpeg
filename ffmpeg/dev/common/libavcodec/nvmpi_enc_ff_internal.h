#pragma once

/*
 * nvmpi_enc_ff_internal.h — shared declarations between the two halves of
 * the FFmpeg H.264/HEVC encoder wrapper:
 *
 *   nvmpi_enc.c         — codec init/close, AVOptions, codec registration.
 *   nvmpi_enc_runtime.c — encode hot path (send_frame/receive_packet/flush)
 *                         and the packet pool backing it.
 *
 * Both files operate on the same per-instance nvmpiEncodeContext, and
 * nvmpi_enc.c's GLOBAL_HEADER extradata-generation block (in .init) and
 * .close callback drive the packet pool implemented in nvmpi_enc_runtime.c
 * — so both the struct and the pool/entry-point declarations live here.
 */

#include "dynlink_nvmpi.h"
#include "avcodec.h"

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
	int nonblocking;            //non-blocking mode: put_frame returns EAGAIN instead of blocking
	int encoder_flushing;       //set after EOS was sent to libnvmpi
	int dmabuf_input;           //set when pix_fmt is DRM_PRIME (mmap+CPU-read path)
	int64_t last_bitrate;       //track last-set bitrate for dynamic change detection
	AVFrame *frame; //tmp frame
	                //(holds the pulled-but-not-yet-sent input frame in the
	                // new-API receive_packet path)
}nvmpiEncodeContext;

//Packet pool management, split across both files by line-count budget
//(both call each other's halves — see the definition site of each):
//  nvmpi_enc.c:         nvmpienc_nvPacket_alloc, _reset, initPktPool
//  nvmpi_enc_runtime.c: nvmpienc_nvPacket_free, deinitPktPool
nvPacket* nvmpienc_nvPacket_alloc(AVCodecContext *avctx, int bufSize);
void nvmpienc_nvPacket_free(nvPacket* nPkt);
int nvmpienc_nvPacket_reset(nvPacket* nPkt, AVCodecContext *avctx, int bufSize);
int nvmpienc_initPktPool(AVCodecContext *avctx, int pktNum);
int nvmpienc_deinitPktPool(AVCodecContext *avctx);

//Encode hot-path entry points — defined in nvmpi_enc_runtime.c, wired into
//the FFCodec struct by the NVMPI_ENC() registration macro in nvmpi_enc.c.
int ff_nvmpi_receive_packet_async(AVCodecContext *avctx, AVPacket *pkt);
void nvmpi_flush_encoder(AVCodecContext *avctx);

//GLOBAL_HEADER extradata pre-generation — defined in nvmpi_enc_runtime.c
//(it drives the same packet-pool/put_frame/get_packet machinery as the
//encode hot path), called from nvmpi_encode_init() in nvmpi_enc.c.
int nvmpi_encode_gen_global_header_extradata(AVCodecContext *avctx, nvEncParam *param);
