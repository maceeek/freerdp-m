/*
 * SPDX-FileCopyrightText: 2026 FreeRDP-M contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include <freerdp/codec/av1.h>
#include <freerdp/codec/color.h>
#include <winpr/environment.h>

/* Two generated 256x256 red frames, AV1 main profile, first a key frame and
 * then an inter frame. No GPU or software encoder is needed to run this test. */
static const BYTE keyframe[] = {
	0x12, 0x00, 0x0a, 0x0b, 0x00, 0x00, 0x00, 0x03, 0xbf, 0xff, 0xf9, 0xb5, 0xf2, 0x00, 0x80,
	0x32, 0x1b, 0x10, 0x00, 0x8f, 0x00, 0x00, 0x00, 0x90, 0x00, 0x00, 0x03, 0xce, 0x22, 0x3d,
	0x26, 0xa6, 0x53, 0x75, 0x43, 0xa9, 0x40, 0x3c, 0x23, 0x20, 0x7d, 0x49, 0xac, 0xc0,
};
static const BYTE interframe[] = {
	0x12, 0x00, 0x32, 0x14, 0x30, 0x03, 0xc0, 0x80, 0x00, 0x00, 0x46, 0x8f,
	0x00, 0x00, 0x00, 0x90, 0x00, 0x08, 0x00, 0x99, 0x23, 0xa8, 0x08, 0x39,
};

static BOOL testRecovery(const char* backend)
{
	BOOL success = FALSE;
	SetEnvironmentVariableA("FREERDP_AV1_HWACCEL", backend);
	FREERDP_AV1_CONTEXT* decoder = freerdp_av1_context_new(FALSE);
	SetEnvironmentVariableA("FREERDP_AV1_HWACCEL", nullptr);
	BYTE* pixels = calloc(256 * 256, 4);
	const RECTANGLE_16 rect = { 0, 0, 256, 256 };
	if (!decoder || !pixels || !freerdp_av1_context_reset(decoder, 256, 256))
		goto out;
	const UINT32 hardware =
	    freerdp_av1_context_get_option(decoder, FREERDP_AV1_CONTEXT_OPTION_HARDWARE);
	/* A surface reset can leave an old inter frame in flight. It must not
	 * permanently blacklist a working hardware decoder. */
	const INT32 initial = freerdp_av1_decompress(decoder, interframe, sizeof(interframe), pixels,
	                                             PIXEL_FORMAT_BGRX32, 256 * 4, 256, 256, &rect, 1);
	if (initial > 0)
		goto out;
	if (freerdp_av1_context_get_option(decoder, FREERDP_AV1_CONTEXT_OPTION_HARDWARE) != hardware)
		goto out;
	if (freerdp_av1_decompress(decoder, keyframe, sizeof(keyframe), pixels, PIXEL_FORMAT_BGRX32,
	                           256 * 4, 256, 256, &rect, 1) <= 0)
		goto out;
	if (freerdp_av1_decompress(decoder, interframe, sizeof(interframe), pixels, PIXEL_FORMAT_BGRX32,
	                           256 * 4, 256, 256, &rect, 1) <= 0)
		goto out;
	BYTE r = 0, g = 0, b = 0;
	FreeRDPSplitColor(FreeRDPReadColor(pixels, PIXEL_FORMAT_BGRX32), PIXEL_FORMAT_BGRX32, &r, &g,
	                  &b, nullptr, nullptr);
	success = (r > 200) && (g < 40) && (b < 40);
out:
	freerdp_av1_context_free(decoder);
	free(pixels);
	return success;
}

int TestFreeRDPCodecAV1Recovery(int argc, char* argv[])
{
	WINPR_UNUSED(argc);
	WINPR_UNUSED(argv);
	return testRecovery("0") && testRecovery(nullptr) ? 0 : 1;
}
