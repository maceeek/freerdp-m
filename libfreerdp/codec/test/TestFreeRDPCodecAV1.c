
#include <stdlib.h>

#include <winpr/environment.h>

#include <freerdp/freerdp.h>
#include <freerdp/codec/color.h>
#include <freerdp/codec/av1.h>

static void* allocRGB(uint32_t format, uint32_t width, uint32_t height, uint32_t* pstride)
{
	const size_t bpp = FreeRDPGetBytesPerPixel(format);
	const size_t stride = bpp * width + 32;
	WINPR_ASSERT(pstride);
	*pstride = WINPR_ASSERTING_INT_CAST(uint32_t, stride);

	uint8_t* rgb = calloc(stride, height);
	if (!rgb)
		return nullptr;

	for (size_t x = 0; x < height; x++)
	{
		if (winpr_RAND(&rgb[x * stride], width * bpp) < 0)
		{
			free(rgb);
			return nullptr;
		}
	}
	return rgb;
}

static BOOL testEncodeDecode(uint32_t format, uint32_t width, uint32_t height)
{
	BOOL rc = FALSE;
	void* src = nullptr;
	void* out = nullptr;
	RDPGFX_H264_METABLOCK meta = WINPR_C_ARRAY_INIT;
	FREERDP_AV1_CONTEXT* enc = freerdp_av1_context_new(TRUE);
	FREERDP_AV1_CONTEXT* dec = freerdp_av1_context_new(FALSE);
	if (!enc || !dec)
		goto fail;

	if (!freerdp_av1_context_reset(enc, width, height))
		goto fail;
	if (!freerdp_av1_context_reset(dec, width, height))
		goto fail;

	uint32_t stride = 0;
	uint32_t ostride = 0;
	src = allocRGB(format, width, height, &stride);
	out = allocRGB(format, width, height, &ostride);
	if (!src || !out || (stride < width) || (stride != ostride))
		goto fail;

	const RECTANGLE_16 rect = { .left = 0, .top = 0, .right = width, .bottom = height };
	uint8_t* dst = nullptr;
	uint32_t dstsize = 0;
	if (freerdp_av1_compress(enc, src, format, stride, width, height, &rect, &dst, &dstsize,
	                         &meta) < 0)
		goto fail;
	if ((dstsize == 0) || !dst)
		goto fail;

	/* AV1 is a lossy codec, so this only checks that the bitstream produced by the
	 * encoder (always libaom) round-trips through whichever decoder backend is active
	 * (dav1d or libaom) without error. It does not compare decoded pixels. */
	rc = freerdp_av1_decompress(dec, dst, dstsize, out, format, stride, width, height, &rect, 1) >=
	     0;

fail:
	freerdp_av1_context_free(enc);
	freerdp_av1_context_free(dec);
	free_h264_metablock(&meta);
	free(src);
	free(out);
	return rc;
}

#if defined(WITH_AV1_FFMPEG)
/* A smooth picture, unlike noise it survives lossy compression well enough to compare. */
static void* allocGradient(uint32_t format, uint32_t width, uint32_t height, uint32_t* pstride)
{
	const size_t bpp = FreeRDPGetBytesPerPixel(format);
	const size_t stride = bpp * width + 32;
	WINPR_ASSERT(pstride);
	*pstride = WINPR_ASSERTING_INT_CAST(uint32_t, stride);

	uint8_t* rgb = calloc(stride, height);
	if (!rgb)
		return nullptr;

	for (size_t y = 0; y < height; y++)
	{
		for (size_t x = 0; x < width; x++)
		{
			const BYTE r = (BYTE)(255 * x / width);
			const BYTE g = (BYTE)(255 * y / height);
			const BYTE b = (BYTE)(255 - (r + g) / 2);
			const UINT32 color = FreeRDPGetColor(format, r, g, b, 0xFF);
			if (!FreeRDPWriteColor(&rgb[y * stride + x * bpp], format, color))
			{
				free(rgb);
				return nullptr;
			}
		}
	}
	return rgb;
}

static double meanError(uint32_t format, const BYTE* a, const BYTE* b, uint32_t stride,
                        uint32_t left, uint32_t top, uint32_t width, uint32_t height)
{
	const size_t bpp = FreeRDPGetBytesPerPixel(format);
	double sum = 0;
	for (size_t y = top; y < top + height; y++)
	{
		for (size_t x = left; x < left + width; x++)
		{
			BYTE ra = 0;
			BYTE ga = 0;
			BYTE ba = 0;
			BYTE rb = 0;
			BYTE gb = 0;
			BYTE bb = 0;
			FreeRDPSplitColor(FreeRDPReadColor(&a[y * stride + x * bpp], format), format, &ra, &ga,
			                  &ba, nullptr, nullptr);
			FreeRDPSplitColor(FreeRDPReadColor(&b[y * stride + x * bpp], format), format, &rb, &gb,
			                  &bb, nullptr, nullptr);
			sum += abs(ra - rb) + abs(ga - gb) + abs(ba - bb);
		}
	}
	return sum / (3.0 * width * height);
}

/* Encodes with the GPU and decodes with the decoder FREERDP_AV1_HWACCEL=decoder selects:
 * nullptr for the default, "0" in software, the way a FreeRDP client without a GPU does, or
 * "nvidia" / "vaapi". A GPU decoder that is not available is skipped. profile 0 is 4:2:0,
 * 1 is 4:4:4 if the GPU can, 4:2:0 otherwise. */
static BOOL testHardware(uint32_t format, uint32_t width, uint32_t height, const char* decoder,
                         uint32_t profile)
{
	BOOL rc = FALSE;
	BYTE* src = nullptr;
	BYTE* out = nullptr;
	RDPGFX_H264_METABLOCK meta = WINPR_C_ARRAY_INIT;
	FREERDP_AV1_CONTEXT* dec = nullptr;
	FREERDP_AV1_CONTEXT* enc = freerdp_av1_context_new(TRUE);
	if (!enc)
		goto fail;

	const BOOL software = decoder && (strcmp(decoder, "0") == 0);
	SetEnvironmentVariableA("FREERDP_AV1_HWACCEL", decoder);
	dec = freerdp_av1_context_new(FALSE);
	SetEnvironmentVariableA("FREERDP_AV1_HWACCEL", nullptr);
	if (!dec)
		goto fail;

	if (!freerdp_av1_context_set_option(enc, FREERDP_AV1_CONTEXT_OPTION_PROFILE, profile) ||
	    !freerdp_av1_context_reset(enc, width, height) ||
	    !freerdp_av1_context_set_option(enc, FREERDP_AV1_CONTEXT_OPTION_RATECONTROL,
	                                    FREERDP_AV1_CQ) ||
	    !freerdp_av1_context_set_option(enc, FREERDP_AV1_CONTEXT_OPTION_QUALITY, 90))
		goto fail;
	if (!freerdp_av1_context_reset(dec, width, height))
		goto fail;
	const UINT32 hwDecoder =
	    freerdp_av1_context_get_option(dec, FREERDP_AV1_CONTEXT_OPTION_HARDWARE);
	if ((freerdp_av1_context_get_option(enc, FREERDP_AV1_CONTEXT_OPTION_HARDWARE) != 1) ||
	    (software && (hwDecoder != 0)))
	{
		(void)fprintf(stderr, "[%s] unexpected AV1 backend\n", __func__);
		goto fail;
	}
	if (decoder && !software && (hwDecoder == 0))
	{
		(void)fprintf(stderr, "[%s] skipping the %s decoder, not available\n", __func__, decoder);
		rc = TRUE;
		goto fail;
	}

	uint32_t stride = 0;
	uint32_t ostride = 0;
	src = allocGradient(format, width, height, &stride);
	out = allocGradient(format, width, height, &ostride);
	if (!src || !out || (stride != ostride))
		goto fail;
	memset(out, 0, 1ull * ostride * height);

	const RECTANGLE_16 rect = { .left = 0, .top = 0, .right = width, .bottom = height };
	for (size_t frame = 0; frame < 4; frame++)
	{
		if (frame == 3)
		{
			if (!freerdp_av1_context_set_option(enc, FREERDP_AV1_CONTEXT_OPTION_KEYFRAME, 1))
				goto fail;
		}

		uint8_t* dst = nullptr;
		uint32_t dstsize = 0;
		free_h264_metablock(&meta);
		if (freerdp_av1_compress(enc, src, format, stride, width, height, &rect, &dst, &dstsize,
		                         &meta) <= 0)
			goto fail;
		if ((dstsize == 0) || !dst || (meta.numRegionRects != 1))
			goto fail;
		if (freerdp_av1_decompress(dec, dst, dstsize, out, format, ostride, width, height, &rect,
		                           1) < 0)
			goto fail;
		const double error = meanError(format, src, out, stride, 0, 0, width, height);
		/* The last column and row keep their colors with odd sizes too */
		const double edgeError = (meanError(format, src, out, stride, width - 1, 0, 1, height) +
		                          meanError(format, src, out, stride, 0, height - 1, width, 1)) /
		                         2.0;
		(void)fprintf(stderr,
		              "[%s] GPU profile %" PRIu32 " -> %s decoder %" PRIu32 "x%" PRIu32
		              " frame %" PRIuz ": %" PRIu32 " bytes, mean error %.2f, at the edges %.2f\n",
		              __func__, profile, decoder ? decoder : "default", width, height, frame,
		              dstsize, error, edgeError);
		if ((error > 3.0) || (edgeError > 6.0))
			goto fail;
	}
	rc = TRUE;

fail:
	freerdp_av1_context_free(enc);
	freerdp_av1_context_free(dec);
	free_h264_metablock(&meta);
	free(src);
	free(out);
	return rc;
}
#endif

int TestFreeRDPCodecAV1(int argc, char* argv[])
{
	WINPR_UNUSED(argc);
	WINPR_UNUSED(argv);

#if defined(WITH_AV1_FFMPEG)
	{
		FREERDP_AV1_CONTEXT* probe = freerdp_av1_context_new(TRUE);
		const BOOL gpu =
		    probe &&
		    (freerdp_av1_context_get_option(probe, FREERDP_AV1_CONTEXT_OPTION_HARDWARE) == 1);
		freerdp_av1_context_free(probe);
		if (!gpu)
			(void)fprintf(stderr, "[%s] skipping GPU encoding, not available\n", __func__);
		else
		{
			const struct
			{
				uint32_t width;
				uint32_t height;
			} sizes[] = { { 1920, 1080 }, { 1279, 719 } };
			const UINT32 formats[] = { PIXEL_FORMAT_BGRA32, PIXEL_FORMAT_RGBX32 };
			for (size_t f = 0; f < ARRAYSIZE(formats); f++)
			{
				for (size_t x = 0; x < ARRAYSIZE(sizes); x++)
				{
					const char* decoders[] = { nullptr, "0", "nvidia", "vaapi" };
					for (size_t d = 0; d < ARRAYSIZE(decoders); d++)
					{
						for (uint32_t profile = 0; profile < 2; profile++)
						{
							if (!testHardware(formats[f], sizes[x].width, sizes[x].height,
							                  decoders[d], profile))
								return -1;
						}
					}
				}
			}
		}
	}
#endif

#if !defined(WITH_LIBAOM)
	(void)fprintf(stderr, "[%s] skipping libaom, not compiled in\n", __func__);
	return 0;
#else
	const UINT32 width = 124;
	const UINT32 height = 54;
	const UINT32 formats[] = { PIXEL_FORMAT_BGRA32, PIXEL_FORMAT_RGB16 };

	for (size_t x = 0; x < ARRAYSIZE(formats); x++)
	{
		if (!testEncodeDecode(formats[x], width, height))
			return -1;
	}

	return 0;
#endif
}
