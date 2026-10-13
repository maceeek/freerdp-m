/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * AV1 Bitmap Compression, NVIDIA hardware backend (NVENC/NVDEC through FFmpeg)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <freerdp/config.h>

#include <limits.h>

#include <winpr/environment.h>
#include <winpr/synch.h>

#include <freerdp/log.h>
#include <freerdp/primitives.h>

#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>

#include "av1_ffmpeg.h"

#define AV1_NVENC_ENCODER "av1_nvenc"
#define AV1_HW_DECODER "av1"

/* The size of the encoder opened to find out whether NVENC can encode AV1. */
#define AV1_PROBE_SIZE 256

#define AV1_DEFAULT_QUALITY 70

/* FreeRDP clients ignore the colour description of the stream and convert with the YUV
 * primitives, which use BT.709 with full range. The encoder converts with the same primitives
 * and describes the stream accordingly for other decoders. */
#define AV1_COLORSPACE AVCOL_SPC_BT709
#define AV1_COLOR_RANGE AVCOL_RANGE_JPEG

#if !defined(av_err2str)
static inline char* error_string(char* errbuf, size_t errbuf_size, int errnum)
{
	av_strerror(errnum, errbuf, errbuf_size);
	return errbuf;
}

#define av_err2str(errnum) error_string((char[64])WINPR_C_ARRAY_INIT, 64, errnum)
#endif

struct S_AV1_FFMPEG_CONTEXT
{
	wLog* log;
	BOOL encoder;

	AVBufferRef* hwdevice;
	AVCodecContext* codec;
	AVFrame* frame;
	AVFrame* swframe;
	AVPacket* packet;

	UINT32 width;
	UINT32 height;

	/* Encoder settings, the encoder is opened again when they change. */
	UINT32 profile;
	UINT32 rateControl;
	UINT32 bitrate;
	UINT32 usage;
	UINT32 quality;
	BOOL dirty;
	BOOL i444;
	BOOL forceKeyframe;
	int64_t pts;

	/* Decoder: the chroma planes of the NV12 pictures of NVDEC, deinterleaved for the YUV
	 * primitives. */
	BYTE* chroma[2];
	size_t chromaSize;
	BOOL decoded;
};

static INIT_ONCE s_encoder_probe_once = INIT_ONCE_STATIC_INIT;
static BOOL s_encoder_available = FALSE;
static BOOL s_encoder_i444 = FALSE;

static UINT32 align2(UINT32 value)
{
	return (value + 1) & ~1u;
}

BOOL av1_ffmpeg_enabled(void)
{
	char value[16] = WINPR_C_ARRAY_INIT;
	const DWORD len = GetEnvironmentVariableA("FREERDP_AV1_HWACCEL", value, sizeof(value));
	if ((len == 0) || (len >= sizeof(value)))
		return TRUE;
	return (strcmp(value, "0") != 0) && (_stricmp(value, "off") != 0) &&
	       (_stricmp(value, "false") != 0);
}

static void av1_ffmpeg_close(AV1_FFMPEG_CONTEXT* ctx)
{
	WINPR_ASSERT(ctx);
	avcodec_free_context(&ctx->codec);
	av_frame_free(&ctx->frame);
	av_frame_free(&ctx->swframe);
	av_packet_free(&ctx->packet);
}

static int av1_ffmpeg_cq(const AV1_FFMPEG_CONTEXT* ctx)
{
	/* 100 -> 20 (visually lossless for desktop content), 0 -> 45 */
	const UINT32 quality = MIN(ctx->quality, 100);
	return 45 - (int)((quality * 25 + 50) / 100);
}

static BOOL av1_ffmpeg_set_rate_control(AV1_FFMPEG_CONTEXT* ctx, AVDictionary** options)
{
	WINPR_ASSERT(ctx);
	WINPR_ASSERT(ctx->codec);

	const int64_t bitrate = 1000ll * ctx->bitrate; /* kbit/s like libaom */
	char cq[16] = WINPR_C_ARRAY_INIT;
	(void)_snprintf(cq, sizeof(cq), "%d", av1_ffmpeg_cq(ctx));

	switch (ctx->rateControl)
	{
		case FREERDP_AV1_VBR:
			av_dict_set(options, "rc", "vbr", 0);
			ctx->codec->bit_rate = bitrate;
			ctx->codec->rc_max_rate = 2 * bitrate;
			break;
		case FREERDP_AV1_CBR:
			av_dict_set(options, "rc", "cbr", 0);
			ctx->codec->bit_rate = bitrate;
			ctx->codec->rc_max_rate = bitrate;
			break;
		case FREERDP_AV1_CQ:
			/* Constant quality, limited to the bitrate if there is one. */
			av_dict_set(options, "rc", "vbr", 0);
			av_dict_set(options, "cq", cq, 0);
			ctx->codec->bit_rate = 0;
			ctx->codec->rc_max_rate = bitrate;
			break;
		case FREERDP_AV1_Q:
		{
			char qp[16] = WINPR_C_ARRAY_INIT;
			/* NVENC uses the AV1 quantizer index 0 - 255 for constqp */
			(void)_snprintf(qp, sizeof(qp), "%d", av1_ffmpeg_cq(ctx) * 4);
			av_dict_set(options, "rc", "constqp", 0);
			av_dict_set(options, "qp", qp, 0);
		}
		break;
		default:
			WLog_Print(ctx->log, WLOG_ERROR, "Unknown AV1 rate control %" PRIu32, ctx->rateControl);
			return FALSE;
	}

	if (ctx->codec->rc_max_rate > 0)
	{
		/* Half a second of video, enough for a key frame without delaying the stream. */
		ctx->codec->rc_buffer_size = (int)MIN(INT_MAX, ctx->codec->rc_max_rate / 2);
	}
	return TRUE;
}

static BOOL av1_ffmpeg_open_encoder_format(AV1_FFMPEG_CONTEXT* ctx, enum AVPixelFormat format,
                                           DWORD level)
{
	WINPR_ASSERT(ctx);
	av1_ffmpeg_close(ctx);

	const AVCodec* codec = avcodec_find_encoder_by_name(AV1_NVENC_ENCODER);
	if (!codec)
	{
		WLog_Print(ctx->log, level, "FFmpeg has no %s encoder", AV1_NVENC_ENCODER);
		return FALSE;
	}

	ctx->codec = avcodec_alloc_context3(codec);
	ctx->frame = av_frame_alloc();
	ctx->packet = av_packet_alloc();
	if (!ctx->codec || !ctx->frame || !ctx->packet)
		goto fail;

	/* 4:2:0 needs an even size, the decoder only converts the size of the surface. */
	ctx->codec->width = WINPR_ASSERTING_INT_CAST(int, align2(ctx->width));
	ctx->codec->height = WINPR_ASSERTING_INT_CAST(int, align2(ctx->height));
	ctx->codec->pix_fmt = format;
	ctx->codec->time_base = (AVRational){ 1, 60 };
	ctx->codec->framerate = (AVRational){ 60, 1 };
	/* The graphics pipeline is reliable, key frames are only needed when asked for. */
	ctx->codec->gop_size = INT_MAX;
	ctx->codec->max_b_frames = 0;
	ctx->codec->color_range = AV1_COLOR_RANGE;
	ctx->codec->colorspace = AV1_COLORSPACE;
	ctx->codec->color_primaries = AVCOL_PRI_BT709;
	ctx->codec->color_trc = AVCOL_TRC_BT709;

	AVDictionary* options = nullptr;
	av_dict_set(&options, "preset", "p4", 0);
	av_dict_set(&options, "tune", "ull", 0);
	av_dict_set(&options, "zerolatency", "1", 0);
	av_dict_set(&options, "delay", "0", 0);
	av_dict_set(&options, "forced-idr", "1", 0);
	if (!av1_ffmpeg_set_rate_control(ctx, &options))
	{
		av_dict_free(&options);
		goto fail;
	}

	const int rc = avcodec_open2(ctx->codec, codec, &options);
	av_dict_free(&options);
	if (rc < 0)
	{
		WLog_Print(ctx->log, level, "Could not open %s for %s at %" PRIu32 "x%" PRIu32 ": %s",
		           AV1_NVENC_ENCODER, av_get_pix_fmt_name(format), ctx->width, ctx->height,
		           av_err2str(rc));
		goto fail;
	}

	ctx->frame->format = format;
	ctx->frame->width = ctx->codec->width;
	ctx->frame->height = ctx->codec->height;
	ctx->frame->color_range = AV1_COLOR_RANGE;
	ctx->frame->colorspace = AV1_COLORSPACE;
	const int brc = av_frame_get_buffer(ctx->frame, 0);
	if (brc < 0)
	{
		WLog_Print(ctx->log, WLOG_ERROR, "av_frame_get_buffer: %s", av_err2str(brc));
		goto fail;
	}

	/* The padding to an even size is never shown, keep it black. */
	for (size_t plane = 0; plane < 3; plane++)
	{
		const int h = (plane == 0) || (format == AV_PIX_FMT_YUV444P) ? ctx->codec->height
		                                                             : ctx->codec->height / 2;
		memset(ctx->frame->data[plane], (plane == 0) ? 0 : 128,
		       1ull * WINPR_ASSERTING_INT_CAST(size_t, ctx->frame->linesize[plane]) *
		           WINPR_ASSERTING_INT_CAST(size_t, h));
	}

	ctx->i444 = format == AV_PIX_FMT_YUV444P;
	ctx->pts = 0;
	ctx->forceKeyframe = FALSE;
	ctx->dirty = FALSE;
	return TRUE;

fail:
	av1_ffmpeg_close(ctx);
	return FALSE;
}

static BOOL av1_ffmpeg_open_encoder(AV1_FFMPEG_CONTEXT* ctx, DWORD level)
{
	WINPR_ASSERT(ctx);

	/* AV1 4:4:4 (high profile) is not supported by every NVENC, 4:2:0 always is.
	 * The client decodes either. */
	if ((ctx->profile == 1) && s_encoder_i444)
	{
		if (av1_ffmpeg_open_encoder_format(ctx, AV_PIX_FMT_YUV444P, WLOG_DEBUG))
			goto opened;
		WLog_Print(ctx->log, WLOG_DEBUG, "NVENC can not encode AV1 4:4:4, using 4:2:0");
	}
	if (!av1_ffmpeg_open_encoder_format(ctx, AV_PIX_FMT_YUV420P, level))
		return FALSE;

opened:
	WLog_Print(ctx->log, WLOG_DEBUG, "Opened %s %s at %" PRIu32 "x%" PRIu32, AV1_NVENC_ENCODER,
	           av_get_pix_fmt_name(ctx->codec->pix_fmt), ctx->width, ctx->height);
	return TRUE;
}

static enum AVPixelFormat av1_ffmpeg_get_format(AVCodecContext* codec,
                                                const enum AVPixelFormat* formats)
{
	WINPR_ASSERT(codec);
	AV1_FFMPEG_CONTEXT* ctx = codec->opaque;
	WINPR_ASSERT(ctx);

	for (const enum AVPixelFormat* format = formats; *format != AV_PIX_FMT_NONE; format++)
	{
		if (*format == AV_PIX_FMT_CUDA)
			return AV_PIX_FMT_CUDA;
	}

	/* E.g. 4:4:4, which NVDEC does not decode */
	WLog_Print(ctx->log, WLOG_INFO, "NVDEC can not decode this AV1 stream (profile %d)",
	           codec->profile);
	return AV_PIX_FMT_NONE;
}

static BOOL av1_ffmpeg_open_decoder(AV1_FFMPEG_CONTEXT* ctx)
{
	WINPR_ASSERT(ctx);
	av1_ffmpeg_close(ctx);

	/* The native AV1 decoder of FFmpeg only decodes in hardware. */
	const AVCodec* codec = avcodec_find_decoder_by_name(AV1_HW_DECODER);
	if (!codec)
	{
		WLog_Print(ctx->log, WLOG_DEBUG, "FFmpeg has no %s decoder", AV1_HW_DECODER);
		return FALSE;
	}

	ctx->codec = avcodec_alloc_context3(codec);
	ctx->frame = av_frame_alloc();
	ctx->swframe = av_frame_alloc();
	ctx->packet = av_packet_alloc();
	if (!ctx->codec || !ctx->frame || !ctx->swframe || !ctx->packet)
		goto fail;

	ctx->codec->opaque = ctx;
	ctx->codec->get_format = av1_ffmpeg_get_format;
	ctx->codec->hw_device_ctx = av_buffer_ref(ctx->hwdevice);
	ctx->codec->thread_count = 1;
	ctx->codec->flags |= AV_CODEC_FLAG_LOW_DELAY;
	if (!ctx->codec->hw_device_ctx)
		goto fail;

	const int rc = avcodec_open2(ctx->codec, codec, nullptr);
	if (rc < 0)
	{
		WLog_Print(ctx->log, WLOG_WARN, "Could not open the %s decoder: %s", AV1_HW_DECODER,
		           av_err2str(rc));
		goto fail;
	}
	ctx->decoded = FALSE;
	return TRUE;

fail:
	av1_ffmpeg_close(ctx);
	return FALSE;
}

static BOOL CALLBACK av1_ffmpeg_probe_encoder(WINPR_ATTR_UNUSED PINIT_ONCE once,
                                              WINPR_ATTR_UNUSED PVOID param,
                                              WINPR_ATTR_UNUSED PVOID* context)
{
	wLog* log = WLog_Get(FREERDP_TAG("codec.av1"));
	AV1_FFMPEG_CONTEXT probe = { .log = log,
	                             .encoder = TRUE,
	                             .width = AV1_PROBE_SIZE,
	                             .height = AV1_PROBE_SIZE,
	                             .rateControl = FREERDP_AV1_CQ,
	                             .quality = AV1_DEFAULT_QUALITY };
	s_encoder_available = av1_ffmpeg_open_encoder_format(&probe, AV_PIX_FMT_YUV420P, WLOG_DEBUG);
	if (s_encoder_available)
		s_encoder_i444 = av1_ffmpeg_open_encoder_format(&probe, AV_PIX_FMT_YUV444P, WLOG_DEBUG);
	av1_ffmpeg_close(&probe);
	WLog_Print(log, WLOG_INFO, "NVENC AV1 encoding is %s",
	           s_encoder_available ? (s_encoder_i444 ? "available with 4:4:4" : "available (4:2:0)")
	                               : "not available");
	return TRUE;
}

BOOL av1_ffmpeg_encoder_available(void)
{
	if (!av1_ffmpeg_enabled())
		return FALSE;
	if (!InitOnceExecuteOnce(&s_encoder_probe_once, av1_ffmpeg_probe_encoder, nullptr, nullptr))
		return FALSE;
	return s_encoder_available;
}

void av1_ffmpeg_free(AV1_FFMPEG_CONTEXT* ctx)
{
	if (!ctx)
		return;
	av1_ffmpeg_close(ctx);
	av_buffer_unref(&ctx->hwdevice);
	winpr_aligned_free(ctx->chroma[0]);
	winpr_aligned_free(ctx->chroma[1]);
	free(ctx);
}

AV1_FFMPEG_CONTEXT* av1_ffmpeg_new(wLog* log, BOOL encoder)
{
	if (!av1_ffmpeg_enabled())
		return nullptr;
	if (encoder && !av1_ffmpeg_encoder_available())
		return nullptr;

	AV1_FFMPEG_CONTEXT* ctx = calloc(1, sizeof(AV1_FFMPEG_CONTEXT));
	if (!ctx)
		return nullptr;
	ctx->log = log;
	ctx->encoder = encoder;
	ctx->profile = 1;
	ctx->rateControl = FREERDP_AV1_CQ;
	ctx->quality = AV1_DEFAULT_QUALITY;

	if (!encoder)
	{
		const AVCodec* codec = avcodec_find_decoder_by_name(AV1_HW_DECODER);
		BOOL cuda = FALSE;
		for (int x = 0; codec; x++)
		{
			const AVCodecHWConfig* config = avcodec_get_hw_config(codec, x);
			if (!config)
				break;
			if ((config->device_type == AV_HWDEVICE_TYPE_CUDA) &&
			    ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0))
				cuda = TRUE;
		}
		if (!cuda)
		{
			WLog_Print(log, WLOG_DEBUG, "FFmpeg can not decode AV1 with NVDEC");
			goto fail;
		}

		const int rc =
		    av_hwdevice_ctx_create(&ctx->hwdevice, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0);
		if (rc < 0)
		{
			WLog_Print(log, WLOG_DEBUG, "No CUDA device for NVDEC: %s", av_err2str(rc));
			goto fail;
		}
	}
	return ctx;

fail:
	av1_ffmpeg_free(ctx);
	return nullptr;
}

BOOL av1_ffmpeg_reset(AV1_FFMPEG_CONTEXT* ctx, UINT32 width, UINT32 height)
{
	WINPR_ASSERT(ctx);
	ctx->width = width;
	ctx->height = height;

	if (ctx->encoder)
		return av1_ffmpeg_open_encoder(ctx, WLOG_WARN);

	const size_t chromaSize = 1ull * (align2(width) / 2) * (align2(height) / 2);
	if (chromaSize > ctx->chromaSize)
	{
		for (size_t x = 0; x < ARRAYSIZE(ctx->chroma); x++)
		{
			winpr_aligned_free(ctx->chroma[x]);
			ctx->chroma[x] = winpr_aligned_malloc(chromaSize, 64);
			if (!ctx->chroma[x])
			{
				ctx->chromaSize = 0;
				return FALSE;
			}
		}
		ctx->chromaSize = chromaSize;
	}
	return av1_ffmpeg_open_decoder(ctx);
}

BOOL av1_ffmpeg_set_option(AV1_FFMPEG_CONTEXT* ctx, FREERDP_AV1_CONTEXT_OPTION option, UINT32 value)
{
	WINPR_ASSERT(ctx);
	if (!ctx->encoder)
		return FALSE;

	switch (option)
	{
		case FREERDP_AV1_CONTEXT_OPTION_PROFILE:
			if (value > 1)
				return FALSE;
			ctx->dirty |= ctx->profile != value;
			ctx->profile = value;
			return TRUE;
		case FREERDP_AV1_CONTEXT_OPTION_RATECONTROL:
			ctx->dirty |= ctx->rateControl != value;
			ctx->rateControl = value;
			return TRUE;
		case FREERDP_AV1_CONTEXT_OPTION_BITRATE:
			ctx->dirty |= ctx->bitrate != value;
			ctx->bitrate = value;
			return TRUE;
		case FREERDP_AV1_CONTEXT_OPTION_USAGETYPE:
			/* libaom only, NVENC always encodes for low latency */
			ctx->usage = value;
			return TRUE;
		case FREERDP_AV1_CONTEXT_OPTION_QUALITY:
			if (value > 100)
				return FALSE;
			ctx->dirty |= ctx->quality != value;
			ctx->quality = value;
			return TRUE;
		case FREERDP_AV1_CONTEXT_OPTION_KEYFRAME:
			ctx->forceKeyframe = value != 0;
			return TRUE;
		default:
			WLog_Print(ctx->log, WLOG_ERROR, "Unknown FREERDP_AV1_CONTEXT_OPTION[0x%08" PRIx32 "]",
			           option);
			return FALSE;
	}
}

UINT32 av1_ffmpeg_get_option(AV1_FFMPEG_CONTEXT* ctx, FREERDP_AV1_CONTEXT_OPTION option)
{
	WINPR_ASSERT(ctx);
	switch (option)
	{
		case FREERDP_AV1_CONTEXT_OPTION_HARDWARE:
			return 1;
		case FREERDP_AV1_CONTEXT_OPTION_PROFILE:
			/* What is encoded, not what was asked for */
			return (ctx->codec && !ctx->dirty) ? (ctx->i444 ? 1 : 0) : ctx->profile;
		case FREERDP_AV1_CONTEXT_OPTION_RATECONTROL:
			return ctx->rateControl;
		case FREERDP_AV1_CONTEXT_OPTION_BITRATE:
			return ctx->bitrate;
		case FREERDP_AV1_CONTEXT_OPTION_USAGETYPE:
			return ctx->usage;
		case FREERDP_AV1_CONTEXT_OPTION_QUALITY:
			return ctx->quality;
		default:
			return 0;
	}
}

INT32 av1_ffmpeg_compress(AV1_FFMPEG_CONTEXT* ctx, const BYTE* pSrcData, DWORD SrcFormat,
                          UINT32 nSrcStep, UINT32 nSrcWidth, UINT32 nSrcHeight, BYTE** ppDstData,
                          UINT32* pDstSize)
{
	WINPR_ASSERT(ctx);
	WINPR_ASSERT(ppDstData);
	WINPR_ASSERT(pDstSize);

	if ((nSrcWidth != ctx->width) || (nSrcHeight != ctx->height))
	{
		WLog_Print(ctx->log, WLOG_ERROR,
		           "Frame %" PRIu32 "x%" PRIu32 " does not match the encoder %" PRIu32 "x%" PRIu32,
		           nSrcWidth, nSrcHeight, ctx->width, ctx->height);
		return -1;
	}

	if ((ctx->dirty || !ctx->codec) && !av1_ffmpeg_open_encoder(ctx, WLOG_ERROR))
		return -1;

	const int wrc = av_frame_make_writable(ctx->frame);
	if (wrc < 0)
	{
		WLog_Print(ctx->log, WLOG_ERROR, "av_frame_make_writable: %s", av_err2str(wrc));
		return -1;
	}

	primitives_t* prims = primitives_get();
	if (!prims)
		return -1;

	BYTE* planes[3] = { ctx->frame->data[0], ctx->frame->data[1], ctx->frame->data[2] };
	const UINT32 strides[3] = { (UINT32)ctx->frame->linesize[0], (UINT32)ctx->frame->linesize[1],
	                            (UINT32)ctx->frame->linesize[2] };
	const prim_size_t roi = { .width = nSrcWidth, .height = nSrcHeight };
	const pstatus_t crc =
	    ctx->i444
	        ? prims->RGBToI444_8u(pSrcData, SrcFormat, nSrcStep, planes, strides, &roi)
	        : prims->RGBToYUV420_8u_P3AC4R(pSrcData, SrcFormat, nSrcStep, planes, strides, &roi);
	if (crc != PRIMITIVES_SUCCESS)
	{
		WLog_Print(ctx->log, WLOG_ERROR, "RGB to YUV conversion failed: %d", crc);
		return -1;
	}

	ctx->frame->pts = ctx->pts++;
	ctx->frame->pict_type = ctx->forceKeyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
	ctx->forceKeyframe = FALSE;

	const int src = avcodec_send_frame(ctx->codec, ctx->frame);
	if (src < 0)
	{
		WLog_Print(ctx->log, WLOG_ERROR, "avcodec_send_frame: %s", av_err2str(src));
		return -1;
	}

	av_packet_unref(ctx->packet);
	const int rrc = avcodec_receive_packet(ctx->codec, ctx->packet);
	if (rrc == AVERROR(EAGAIN))
		return 0;
	if (rrc < 0)
	{
		WLog_Print(ctx->log, WLOG_ERROR, "avcodec_receive_packet: %s", av_err2str(rrc));
		return -1;
	}

	*ppDstData = ctx->packet->data;
	*pDstSize = WINPR_ASSERTING_INT_CAST(UINT32, ctx->packet->size);
	return 1;
}

static INT32 av1_ffmpeg_convert(AV1_FFMPEG_CONTEXT* ctx, BYTE* pDstData, DWORD DstFormat,
                                UINT32 nDstStep, UINT32 nDstWidth, UINT32 nDstHeight)
{
	WINPR_ASSERT(ctx);

	av_frame_unref(ctx->swframe);
	const int trc = av_hwframe_transfer_data(ctx->swframe, ctx->frame, 0);
	if (trc < 0)
	{
		WLog_Print(ctx->log, WLOG_ERROR, "av_hwframe_transfer_data: %s", av_err2str(trc));
		return -1;
	}
	if (ctx->swframe->format != AV_PIX_FMT_NV12)
	{
		WLog_Print(ctx->log, WLOG_ERROR, "NVDEC picture format %s is not supported",
		           av_get_pix_fmt_name(ctx->swframe->format));
		return -1;
	}

	/* The decoded picture may be smaller than the surface, only convert what it has. */
	const UINT32 width = MIN(nDstWidth, (UINT32)ctx->swframe->width);
	const UINT32 height = MIN(nDstHeight, (UINT32)ctx->swframe->height);
	const UINT32 chromaWidth = (width + 1) / 2;
	const UINT32 chromaHeight = (height + 1) / 2;
	if (1ull * chromaWidth * chromaHeight > ctx->chromaSize)
	{
		WLog_Print(ctx->log, WLOG_ERROR, "NVDEC picture %dx%d is larger than the surface",
		           ctx->swframe->width, ctx->swframe->height);
		return -1;
	}

	for (UINT32 y = 0; y < chromaHeight; y++)
	{
		const BYTE* src = &ctx->swframe->data[1][1ull * y * (size_t)ctx->swframe->linesize[1]];
		BYTE* u = &ctx->chroma[0][1ull * y * chromaWidth];
		BYTE* v = &ctx->chroma[1][1ull * y * chromaWidth];
		for (UINT32 x = 0; x < chromaWidth; x++)
		{
			u[x] = src[2 * x];
			v[x] = src[2 * x + 1];
		}
	}

	primitives_t* prims = primitives_get();
	if (!prims)
		return -1;

	const BYTE* planes[3] = { ctx->swframe->data[0], ctx->chroma[0], ctx->chroma[1] };
	const UINT32 strides[3] = { (UINT32)ctx->swframe->linesize[0], chromaWidth, chromaWidth };
	const prim_size_t roi = { .width = width, .height = height };
	const pstatus_t rc =
	    prims->YUV420ToRGB_8u_P3AC4R(planes, strides, pDstData, nDstStep, DstFormat, &roi);
	if (rc != PRIMITIVES_SUCCESS)
	{
		WLog_Print(ctx->log, WLOG_ERROR, "YUV to RGB conversion failed: %d", rc);
		return -1;
	}
	ctx->decoded = TRUE;
	return 1;
}

INT32 av1_ffmpeg_decompress(AV1_FFMPEG_CONTEXT* ctx, const BYTE* pSrcData, UINT32 SrcSize,
                            BYTE* pDstData, DWORD DstFormat, UINT32 nDstStep, UINT32 nDstWidth,
                            UINT32 nDstHeight)
{
	WINPR_ASSERT(ctx);
	if (!ctx->codec)
		return -1;
	if (SrcSize > INT_MAX)
		return -1;

	/* Not reference counted, so FFmpeg copies the data. */
	av_packet_unref(ctx->packet);
	ctx->packet->data = WINPR_CAST_CONST_PTR_AWAY(pSrcData, uint8_t*);
	ctx->packet->size = (int)SrcSize;

	int rc = avcodec_send_packet(ctx->codec, ctx->packet);
	ctx->packet->data = nullptr;
	ctx->packet->size = 0;
	if (rc < 0)
	{
		WLog_Print(ctx->log, WLOG_WARN, "avcodec_send_packet: %s", av_err2str(rc));
		return -1;
	}

	INT32 status = 0;
	for (;;)
	{
		rc = avcodec_receive_frame(ctx->codec, ctx->frame);
		if ((rc == AVERROR(EAGAIN)) || (rc == AVERROR_EOF))
			break;
		if (rc < 0)
		{
			WLog_Print(ctx->log, WLOG_WARN, "avcodec_receive_frame: %s", av_err2str(rc));
			return -1;
		}
		status = av1_ffmpeg_convert(ctx, pDstData, DstFormat, nDstStep, nDstWidth, nDstHeight);
		av_frame_unref(ctx->frame);
		if (status < 0)
			return status;
	}
	return status;
}

BOOL av1_ffmpeg_has_decoded(const AV1_FFMPEG_CONTEXT* ctx)
{
	WINPR_ASSERT(ctx);
	return ctx->decoded;
}
