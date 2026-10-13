/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * AV1 Bitmap Compression, hardware backends through FFmpeg:
 * NVENC/NVDEC on NVIDIA GPUs and VAAPI on AMD (and Intel) GPUs
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
#include <winpr/path.h>
#include <winpr/synch.h>

#include <freerdp/log.h>
#include <freerdp/primitives.h>

#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>

#if defined(WITH_AV1_VAAPI)
#include <va/va.h>
#include <libavutil/hwcontext_vaapi.h>
#endif

#include "av1_ffmpeg.h"

#define AV1_NVENC_ENCODER "av1_nvenc"
#define AV1_VAAPI_ENCODER "av1_vaapi"
#define AV1_HW_DECODER "av1"

/* The size of the encoder opened to find out whether the GPU can encode AV1. */
#define AV1_PROBE_SIZE 256

#define AV1_DEFAULT_QUALITY 70

/* The graphics pipeline is reliable, key frames are only needed when asked for. VAAPI gets a
 * long but finite GOP, it computes with the GOP size. */
#define AV1_NVENC_GOP INT_MAX
#define AV1_VAAPI_GOP (1 << 20)

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

typedef enum
{
	AV1_HW_NONE = 0,
	AV1_HW_CUDA = 1,  /**< NVENC / NVDEC */
	AV1_HW_VAAPI = 2, /**< AMD and Intel GPUs through Mesa or the Intel media driver */
	AV1_HW_ALL = AV1_HW_CUDA | AV1_HW_VAAPI
} AV1_HW_TYPE;

struct S_AV1_FFMPEG_CONTEXT
{
	wLog* log;
	BOOL encoder;
	AV1_HW_TYPE type;

	AVBufferRef* hwdevice;
	AVCodecContext* codec;
	AVFrame* frame;   /**< NVENC: the input picture, VAAPI and decoders: the GPU picture */
	AVFrame* swframe; /**< VAAPI encoder: the NV12 picture uploaded, decoders: downloaded */
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

	/* Planar chroma for the YUV primitives, the GPUs use interleaved NV12. */
	BYTE* chroma[2];
	size_t chromaSize;
	BOOL decoded;
};

static INIT_ONCE s_encoder_probe_once = INIT_ONCE_STATIC_INIT;
static struct
{
	AV1_HW_TYPE type;
	BOOL i444;
	char vaapiDevice[64];
} s_encoder;

static UINT32 align2(UINT32 value)
{
	return (value + 1) & ~1u;
}

/* FREERDP_AV1_HWACCEL: unset or auto (default), 0/off, nvidia, vaapi (also amd, intel) */
static UINT32 av1_hw_allowed(void)
{
	char value[16] = WINPR_C_ARRAY_INIT;
	const DWORD len = GetEnvironmentVariableA("FREERDP_AV1_HWACCEL", value, sizeof(value));
	if ((len == 0) || (len >= sizeof(value)))
		return AV1_HW_ALL;
	if ((strcmp(value, "0") == 0) || (_stricmp(value, "off") == 0) ||
	    (_stricmp(value, "false") == 0) || (_stricmp(value, "no") == 0))
		return AV1_HW_NONE;
	if ((_stricmp(value, "nvidia") == 0) || (_stricmp(value, "cuda") == 0) ||
	    (_stricmp(value, "nvenc") == 0))
		return AV1_HW_CUDA;
	if ((_stricmp(value, "vaapi") == 0) || (_stricmp(value, "amd") == 0) ||
	    (_stricmp(value, "intel") == 0))
		return AV1_HW_VAAPI;
	return AV1_HW_ALL;
}

BOOL av1_ffmpeg_enabled(void)
{
	return av1_hw_allowed() != AV1_HW_NONE;
}

static const char* av1_hw_name(AV1_HW_TYPE type, BOOL encoder)
{
	switch (type)
	{
		case AV1_HW_CUDA:
			return encoder ? "NVENC" : "NVDEC";
		case AV1_HW_VAAPI:
			return "VAAPI";
		default:
			return "none";
	}
}

const char* av1_ffmpeg_backend_name(const AV1_FFMPEG_CONTEXT* ctx)
{
	WINPR_ASSERT(ctx);
	return av1_hw_name(ctx->type, ctx->encoder);
}

#if defined(WITH_AV1_VAAPI)
static BOOL av1_vaapi_supports(AVBufferRef* device, BOOL encoder)
{
	WINPR_ASSERT(device);
	const AVHWDeviceContext* dev = (const AVHWDeviceContext*)device->data;
	const AVVAAPIDeviceContext* va = dev->hwctx;

	const int max = vaMaxNumEntrypoints(va->display);
	if (max <= 0)
		return FALSE;
	VAEntrypoint* entrypoints = calloc((size_t)max, sizeof(VAEntrypoint));
	if (!entrypoints)
		return FALSE;

	BOOL supported = FALSE;
	int count = 0;
	if (vaQueryConfigEntrypoints(va->display, VAProfileAV1Profile0, entrypoints, &count) ==
	    VA_STATUS_SUCCESS)
	{
		for (int x = 0; x < count; x++)
		{
			if (encoder)
				supported |= (entrypoints[x] == VAEntrypointEncSlice) ||
				             (entrypoints[x] == VAEntrypointEncSliceLP);
			else
				supported |= entrypoints[x] == VAEntrypointVLD;
		}
	}
	free(entrypoints);
	return supported;
}

/* The first render node whose driver can encode or decode AV1, or the one in
 * FREERDP_AV1_VAAPI_DEVICE. */
static AVBufferRef* av1_vaapi_open_device(wLog* log, BOOL encoder, char* path, size_t pathSize)
{
	char node[64] = WINPR_C_ARRAY_INIT;
	const DWORD len = GetEnvironmentVariableA("FREERDP_AV1_VAAPI_DEVICE", node, sizeof(node));
	const BOOL fixed = (len > 0) && (len < sizeof(node));

	for (int x = 0; x < (fixed ? 1 : 64); x++)
	{
		if (!fixed)
			(void)_snprintf(node, sizeof(node), "/dev/dri/renderD%d", 128 + x);
		if (!winpr_PathFileExists(node))
			continue;

		AVBufferRef* device = nullptr;
		const int rc = av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_VAAPI, node, nullptr, 0);
		if (rc < 0)
		{
			WLog_Print(log, WLOG_DEBUG, "No VAAPI on %s: %s", node, av_err2str(rc));
			continue;
		}

		const AVHWDeviceContext* dev = (const AVHWDeviceContext*)device->data;
		const AVVAAPIDeviceContext* va = dev->hwctx;
		const char* vendor = vaQueryVendorString(va->display);
		if (av1_vaapi_supports(device, encoder))
		{
			WLog_Print(log, WLOG_DEBUG, "VAAPI %s AV1 on %s (%s)", encoder ? "encodes" : "decodes",
			           node, vendor ? vendor : "?");
			if (path)
				(void)_snprintf(path, pathSize, "%s", node);
			return device;
		}
		WLog_Print(log, WLOG_DEBUG, "VAAPI can not %s AV1 on %s (%s)",
		           encoder ? "encode" : "decode", node, vendor ? vendor : "?");
		av_buffer_unref(&device);
	}
	return nullptr;
}
#endif

static void av1_ffmpeg_close(AV1_FFMPEG_CONTEXT* ctx)
{
	WINPR_ASSERT(ctx);
	avcodec_free_context(&ctx->codec);
	av_frame_free(&ctx->frame);
	av_frame_free(&ctx->swframe);
	av_packet_free(&ctx->packet);
}

static BOOL av1_ffmpeg_alloc_chroma(AV1_FFMPEG_CONTEXT* ctx)
{
	WINPR_ASSERT(ctx);
	const size_t chromaSize = 1ull * (align2(ctx->width) / 2) * (align2(ctx->height) / 2);
	if (chromaSize <= ctx->chromaSize)
		return TRUE;

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
	return TRUE;
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
	const BOOL nvenc = ctx->type == AV1_HW_CUDA;
	/* The AV1 quantizer index 0 - 255 for constant QP */
	const int qindex = av1_ffmpeg_cq(ctx) * 4;
	char value[16] = WINPR_C_ARRAY_INIT;

	switch (ctx->rateControl)
	{
		case FREERDP_AV1_VBR:
			av_dict_set(options, nvenc ? "rc" : "rc_mode", nvenc ? "vbr" : "VBR", 0);
			ctx->codec->bit_rate = bitrate;
			ctx->codec->rc_max_rate = 2 * bitrate;
			break;
		case FREERDP_AV1_CBR:
			av_dict_set(options, nvenc ? "rc" : "rc_mode", nvenc ? "cbr" : "CBR", 0);
			ctx->codec->bit_rate = bitrate;
			ctx->codec->rc_max_rate = bitrate;
			break;
		case FREERDP_AV1_CQ:
			if (nvenc)
			{
				/* Constant quality, limited to the bitrate if there is one. */
				(void)_snprintf(value, sizeof(value), "%d", av1_ffmpeg_cq(ctx));
				av_dict_set(options, "rc", "vbr", 0);
				av_dict_set(options, "cq", value, 0);
				ctx->codec->bit_rate = 0;
				ctx->codec->rc_max_rate = bitrate;
				break;
			}
			/* Not every VAAPI driver has a constant quality mode with a bitrate limit
			 * (QVBR), constant QP works everywhere. */
			WINPR_FALLTHROUGH
		case FREERDP_AV1_Q:
			(void)_snprintf(value, sizeof(value), "%d", qindex);
			if (nvenc)
			{
				av_dict_set(options, "rc", "constqp", 0);
				av_dict_set(options, "qp", value, 0);
			}
			else
			{
				av_dict_set(options, "rc_mode", "CQP", 0);
				ctx->codec->global_quality = qindex;
			}
			ctx->codec->bit_rate = 0;
			ctx->codec->rc_max_rate = 0;
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

/* The padding to an even size is never shown, keep it black. */
static void av1_ffmpeg_blank(AVFrame* frame)
{
	WINPR_ASSERT(frame);
	const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(frame->format);
	if (!desc)
		return;

	for (int plane = 0; plane < 4; plane++)
	{
		if (!frame->data[plane] || (frame->linesize[plane] <= 0))
			continue;
		const int h =
		    (plane == 0) ? frame->height : AV_CEIL_RSHIFT(frame->height, desc->log2_chroma_h);
		memset(frame->data[plane], (plane == 0) ? 0 : 128,
		       1ull * (size_t)frame->linesize[plane] * (size_t)h);
	}
}

static BOOL av1_ffmpeg_open_encoder_format(AV1_FFMPEG_CONTEXT* ctx, BOOL i444, DWORD level)
{
	WINPR_ASSERT(ctx);
	av1_ffmpeg_close(ctx);

	const BOOL nvenc = ctx->type == AV1_HW_CUDA;
	/* AV1 4:4:4 needs the high profile, VAAPI drivers only encode the main profile. */
	if (i444 && !nvenc)
		return FALSE;

	const char* name = nvenc ? AV1_NVENC_ENCODER : AV1_VAAPI_ENCODER;
	const AVCodec* codec = avcodec_find_encoder_by_name(name);
	if (!codec)
	{
		WLog_Print(ctx->log, level, "FFmpeg has no %s encoder", name);
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
	ctx->codec->time_base = (AVRational){ 1, 60 };
	ctx->codec->framerate = (AVRational){ 60, 1 };
	ctx->codec->gop_size = nvenc ? AV1_NVENC_GOP : AV1_VAAPI_GOP;
	ctx->codec->max_b_frames = 0;
	ctx->codec->color_range = AV1_COLOR_RANGE;
	ctx->codec->colorspace = AV1_COLORSPACE;
	ctx->codec->color_primaries = AVCOL_PRI_BT709;
	ctx->codec->color_trc = AVCOL_TRC_BT709;

	AVDictionary* options = nullptr;
	if (nvenc)
	{
		ctx->codec->pix_fmt = i444 ? AV_PIX_FMT_YUV444P : AV_PIX_FMT_YUV420P;
		av_dict_set(&options, "preset", "p4", 0);
		av_dict_set(&options, "tune", "ull", 0);
		av_dict_set(&options, "zerolatency", "1", 0);
		av_dict_set(&options, "delay", "0", 0);
		av_dict_set(&options, "forced-idr", "1", 0);
	}
	else
	{
		/* The picture is converted on the CPU and uploaded as NV12. */
		ctx->codec->pix_fmt = AV_PIX_FMT_VAAPI;
		ctx->codec->hw_frames_ctx = av_hwframe_ctx_alloc(ctx->hwdevice);
		if (!ctx->codec->hw_frames_ctx)
			goto fail;
		AVHWFramesContext* frames = (AVHWFramesContext*)ctx->codec->hw_frames_ctx->data;
		frames->format = AV_PIX_FMT_VAAPI;
		frames->sw_format = AV_PIX_FMT_NV12;
		frames->width = ctx->codec->width;
		frames->height = ctx->codec->height;
		const int frc = av_hwframe_ctx_init(ctx->codec->hw_frames_ctx);
		if (frc < 0)
		{
			WLog_Print(ctx->log, level, "Could not create VAAPI surfaces: %s", av_err2str(frc));
			goto fail;
		}
		/* Output every frame right away. */
		av_dict_set(&options, "async_depth", "1", 0);
	}
	if (!av1_ffmpeg_set_rate_control(ctx, &options))
	{
		av_dict_free(&options);
		goto fail;
	}

	const int rc = avcodec_open2(ctx->codec, codec, &options);
	av_dict_free(&options);
	if (rc < 0)
	{
		WLog_Print(ctx->log, level, "Could not open %s for %s at %" PRIu32 "x%" PRIu32 ": %s", name,
		           i444 ? "4:4:4" : "4:2:0", ctx->width, ctx->height, av_err2str(rc));
		goto fail;
	}

	AVFrame* input = ctx->frame;
	if (!nvenc)
	{
		ctx->swframe = av_frame_alloc();
		if (!ctx->swframe)
			goto fail;
		input = ctx->swframe;
	}
	input->format = nvenc ? ctx->codec->pix_fmt : AV_PIX_FMT_NV12;
	input->width = ctx->codec->width;
	input->height = ctx->codec->height;
	input->color_range = AV1_COLOR_RANGE;
	input->colorspace = AV1_COLORSPACE;
	const int brc = av_frame_get_buffer(input, 0);
	if (brc < 0)
	{
		WLog_Print(ctx->log, WLOG_ERROR, "av_frame_get_buffer: %s", av_err2str(brc));
		goto fail;
	}
	av1_ffmpeg_blank(input);

	ctx->i444 = i444;
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

	/* AV1 4:4:4 (high profile) is not supported by every GPU, 4:2:0 always is.
	 * The client decodes either. */
	if ((ctx->profile == 1) && s_encoder.i444)
	{
		if (av1_ffmpeg_open_encoder_format(ctx, TRUE, WLOG_DEBUG))
			goto opened;
		WLog_Print(ctx->log, WLOG_DEBUG, "%s can not encode AV1 4:4:4, using 4:2:0",
		           av1_ffmpeg_backend_name(ctx));
	}
	if (!av1_ffmpeg_open_encoder_format(ctx, FALSE, level))
		return FALSE;

opened:
	WLog_Print(ctx->log, WLOG_DEBUG, "Opened the %s AV1 encoder for %s at %" PRIu32 "x%" PRIu32,
	           av1_ffmpeg_backend_name(ctx), ctx->i444 ? "4:4:4" : "4:2:0", ctx->width,
	           ctx->height);
	return TRUE;
}

static enum AVPixelFormat av1_ffmpeg_get_format(AVCodecContext* codec,
                                                const enum AVPixelFormat* formats)
{
	WINPR_ASSERT(codec);
	AV1_FFMPEG_CONTEXT* ctx = codec->opaque;
	WINPR_ASSERT(ctx);

	const enum AVPixelFormat wanted =
	    (ctx->type == AV1_HW_CUDA) ? AV_PIX_FMT_CUDA : AV_PIX_FMT_VAAPI;
	for (const enum AVPixelFormat* format = formats; *format != AV_PIX_FMT_NONE; format++)
	{
		if (*format == wanted)
			return wanted;
	}

	/* E.g. 4:4:4, which the GPUs do not decode */
	WLog_Print(ctx->log, WLOG_INFO, "%s can not decode this AV1 stream (profile %d)",
	           av1_ffmpeg_backend_name(ctx), codec->profile);
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
	const UINT32 allowed = av1_hw_allowed();
	AV1_FFMPEG_CONTEXT probe = { .log = log,
	                             .encoder = TRUE,
	                             .width = AV1_PROBE_SIZE,
	                             .height = AV1_PROBE_SIZE,
	                             .rateControl = FREERDP_AV1_CQ,
	                             .quality = AV1_DEFAULT_QUALITY };

	if ((allowed & AV1_HW_CUDA) != 0)
	{
		probe.type = AV1_HW_CUDA;
		if (av1_ffmpeg_open_encoder_format(&probe, FALSE, WLOG_DEBUG))
		{
			s_encoder.type = AV1_HW_CUDA;
			s_encoder.i444 = av1_ffmpeg_open_encoder_format(&probe, TRUE, WLOG_DEBUG);
		}
		av1_ffmpeg_close(&probe);
	}

#if defined(WITH_AV1_VAAPI)
	if ((s_encoder.type == AV1_HW_NONE) && ((allowed & AV1_HW_VAAPI) != 0))
	{
		probe.type = AV1_HW_VAAPI;
		probe.hwdevice =
		    av1_vaapi_open_device(log, TRUE, s_encoder.vaapiDevice, sizeof(s_encoder.vaapiDevice));
		if (probe.hwdevice && av1_ffmpeg_open_encoder_format(&probe, FALSE, WLOG_DEBUG))
			s_encoder.type = AV1_HW_VAAPI;
		av1_ffmpeg_close(&probe);
		av_buffer_unref(&probe.hwdevice);
	}
#endif

	switch (s_encoder.type)
	{
		case AV1_HW_CUDA:
			WLog_Print(log, WLOG_INFO, "AV1 encoding with NVENC (%s)",
			           s_encoder.i444 ? "4:4:4 and 4:2:0" : "4:2:0");
			break;
		case AV1_HW_VAAPI:
			WLog_Print(log, WLOG_INFO, "AV1 encoding with VAAPI on %s (4:2:0)",
			           s_encoder.vaapiDevice);
			break;
		default:
			WLog_Print(log, WLOG_INFO, "No GPU encodes AV1");
			break;
	}
	return TRUE;
}

BOOL av1_ffmpeg_encoder_available(void)
{
	if (!av1_ffmpeg_enabled())
		return FALSE;
	if (!InitOnceExecuteOnce(&s_encoder_probe_once, av1_ffmpeg_probe_encoder, nullptr, nullptr))
		return FALSE;
	return (s_encoder.type & av1_hw_allowed()) != 0;
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

static BOOL av1_decoder_has_hw_config(enum AVHWDeviceType type)
{
	const AVCodec* codec = avcodec_find_decoder_by_name(AV1_HW_DECODER);
	for (int x = 0; codec; x++)
	{
		const AVCodecHWConfig* config = avcodec_get_hw_config(codec, x);
		if (!config)
			break;
		if ((config->device_type == type) &&
		    ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0))
			return TRUE;
	}
	return FALSE;
}

static BOOL av1_ffmpeg_open_decoder_device(AV1_FFMPEG_CONTEXT* ctx, UINT32 allowed)
{
	WINPR_ASSERT(ctx);

	if (((allowed & AV1_HW_CUDA) != 0) && av1_decoder_has_hw_config(AV_HWDEVICE_TYPE_CUDA))
	{
		const int rc =
		    av_hwdevice_ctx_create(&ctx->hwdevice, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0);
		if (rc >= 0)
		{
			ctx->type = AV1_HW_CUDA;
			return TRUE;
		}
		WLog_Print(ctx->log, WLOG_DEBUG, "No CUDA device for NVDEC: %s", av_err2str(rc));
	}

#if defined(WITH_AV1_VAAPI)
	if (((allowed & AV1_HW_VAAPI) != 0) && av1_decoder_has_hw_config(AV_HWDEVICE_TYPE_VAAPI))
	{
		ctx->hwdevice = av1_vaapi_open_device(ctx->log, FALSE, nullptr, 0);
		if (ctx->hwdevice)
		{
			ctx->type = AV1_HW_VAAPI;
			return TRUE;
		}
	}
#endif

	WLog_Print(ctx->log, WLOG_DEBUG, "No GPU decodes AV1");
	return FALSE;
}

AV1_FFMPEG_CONTEXT* av1_ffmpeg_new(wLog* log, BOOL encoder)
{
	const UINT32 allowed = av1_hw_allowed();
	if (allowed == AV1_HW_NONE)
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

	if (encoder)
	{
		ctx->type = s_encoder.type;
#if defined(WITH_AV1_VAAPI)
		if (ctx->type == AV1_HW_VAAPI)
		{
			const int rc = av_hwdevice_ctx_create(&ctx->hwdevice, AV_HWDEVICE_TYPE_VAAPI,
			                                      s_encoder.vaapiDevice, nullptr, 0);
			if (rc < 0)
			{
				WLog_Print(log, WLOG_WARN, "Could not open VAAPI on %s: %s", s_encoder.vaapiDevice,
				           av_err2str(rc));
				goto fail;
			}
		}
#endif
	}
	else if (!av1_ffmpeg_open_decoder_device(ctx, allowed))
		goto fail;

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

	if (!av1_ffmpeg_alloc_chroma(ctx))
		return FALSE;
	if (ctx->encoder)
		return av1_ffmpeg_open_encoder(ctx, WLOG_WARN);
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
			/* libaom only, the GPUs always encode for low latency */
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

/* Converts the picture into ctx->frame for NVENC, or into NV12 and uploads it for VAAPI. */
static BOOL av1_ffmpeg_prepare_input(AV1_FFMPEG_CONTEXT* ctx, const BYTE* pSrcData, DWORD SrcFormat,
                                     UINT32 nSrcStep, UINT32 nSrcWidth, UINT32 nSrcHeight)
{
	WINPR_ASSERT(ctx);

	primitives_t* prims = primitives_get();
	if (!prims)
		return FALSE;

	const prim_size_t roi = { .width = nSrcWidth, .height = nSrcHeight };
	if (ctx->type == AV1_HW_CUDA)
	{
		const int wrc = av_frame_make_writable(ctx->frame);
		if (wrc < 0)
		{
			WLog_Print(ctx->log, WLOG_ERROR, "av_frame_make_writable: %s", av_err2str(wrc));
			return FALSE;
		}

		BYTE* planes[3] = { ctx->frame->data[0], ctx->frame->data[1], ctx->frame->data[2] };
		const UINT32 strides[3] = { (UINT32)ctx->frame->linesize[0],
		                            (UINT32)ctx->frame->linesize[1],
		                            (UINT32)ctx->frame->linesize[2] };
		const pstatus_t crc =
		    ctx->i444 ? prims->RGBToI444_8u(pSrcData, SrcFormat, nSrcStep, planes, strides, &roi)
			          : prims->RGBToYUV420_8u_P3AC4R(pSrcData, SrcFormat, nSrcStep, planes, strides,
			                                         &roi);
		if (crc != PRIMITIVES_SUCCESS)
		{
			WLog_Print(ctx->log, WLOG_ERROR, "RGB to YUV conversion failed: %d", crc);
			return FALSE;
		}
		return TRUE;
	}

	/* VAAPI: luma straight into the NV12 picture, chroma planar first, then interleaved */
	const UINT32 chromaWidth = (nSrcWidth + 1) / 2;
	const UINT32 chromaHeight = (nSrcHeight + 1) / 2;
	BYTE* planes[3] = { ctx->swframe->data[0], ctx->chroma[0], ctx->chroma[1] };
	const UINT32 strides[3] = { (UINT32)ctx->swframe->linesize[0], chromaWidth, chromaWidth };
	const pstatus_t crc =
	    prims->RGBToYUV420_8u_P3AC4R(pSrcData, SrcFormat, nSrcStep, planes, strides, &roi);
	if (crc != PRIMITIVES_SUCCESS)
	{
		WLog_Print(ctx->log, WLOG_ERROR, "RGB to YUV conversion failed: %d", crc);
		return FALSE;
	}
	for (UINT32 y = 0; y < chromaHeight; y++)
	{
		BYTE* dst = &ctx->swframe->data[1][1ull * y * (size_t)ctx->swframe->linesize[1]];
		const BYTE* u = &ctx->chroma[0][1ull * y * chromaWidth];
		const BYTE* v = &ctx->chroma[1][1ull * y * chromaWidth];
		for (UINT32 x = 0; x < chromaWidth; x++)
		{
			dst[2 * x] = u[x];
			dst[2 * x + 1] = v[x];
		}
	}

	av_frame_unref(ctx->frame);
	int rc = av_hwframe_get_buffer(ctx->codec->hw_frames_ctx, ctx->frame, 0);
	if (rc >= 0)
		rc = av_hwframe_transfer_data(ctx->frame, ctx->swframe, 0);
	if (rc < 0)
	{
		WLog_Print(ctx->log, WLOG_ERROR, "Could not upload the picture to VAAPI: %s",
		           av_err2str(rc));
		return FALSE;
	}
	ctx->frame->color_range = AV1_COLOR_RANGE;
	ctx->frame->colorspace = AV1_COLORSPACE;
	return TRUE;
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

	if (!av1_ffmpeg_prepare_input(ctx, pSrcData, SrcFormat, nSrcStep, nSrcWidth, nSrcHeight))
		return -1;

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
	ctx->swframe->format = AV_PIX_FMT_NV12;
	const int trc = av_hwframe_transfer_data(ctx->swframe, ctx->frame, 0);
	if (trc < 0)
	{
		WLog_Print(ctx->log, WLOG_ERROR, "Could not download the %s picture as NV12: %s",
		           av1_ffmpeg_backend_name(ctx), av_err2str(trc));
		return -1;
	}

	/* The decoded picture may be smaller than the surface, only convert what it has. */
	const UINT32 width = MIN(nDstWidth, (UINT32)ctx->swframe->width);
	const UINT32 height = MIN(nDstHeight, (UINT32)ctx->swframe->height);
	const UINT32 chromaWidth = (width + 1) / 2;
	const UINT32 chromaHeight = (height + 1) / 2;
	if (1ull * chromaWidth * chromaHeight > ctx->chromaSize)
	{
		WLog_Print(ctx->log, WLOG_ERROR, "%s picture %dx%d is larger than the surface",
		           av1_ffmpeg_backend_name(ctx), ctx->swframe->width, ctx->swframe->height);
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
