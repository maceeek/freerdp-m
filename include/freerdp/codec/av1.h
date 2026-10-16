/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * AV1 Bitmap Compression
 *
 * Copyright 2026 Armin Novak <anovak@thincast.com>
 * Copyright 2026 Thincast Technologies GmbH
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

#pragma once

#include <winpr/wlog.h>

#include <freerdp/api.h>
#include <freerdp/types.h>
#include <freerdp/channels/rdpgfx.h>

#ifdef __cplusplus
extern "C"
{
#endif

	typedef struct S_FREERDP_AV1_CONTEXT FREERDP_AV1_CONTEXT;

	typedef enum
	{
		FREERDP_AV1_VBR, /**< Variable Bit Rate (VBR) mode */
		FREERDP_AV1_CBR, /**< Constant Bit Rate (CBR) mode */
		FREERDP_AV1_CQ,  /**< Constrained Quality (CQ)  mode */
		FREERDP_AV1_Q    /**< Constant Quality (Q) mode */
	} FREERDP_AV1_RATECONTROL;

	typedef enum
	{
		FREERDP_AV1_CONTEXT_OPTION_PROFILE,
		FREERDP_AV1_CONTEXT_OPTION_RATECONTROL,
		FREERDP_AV1_CONTEXT_OPTION_BITRATE,
		FREERDP_AV1_CONTEXT_OPTION_USAGETYPE,
		FREERDP_AV1_CONTEXT_OPTION_QUALITY,  /**< freerdp-m: 0 (worst) - 100 (best) for the
		                                      * FREERDP_AV1_CQ and FREERDP_AV1_Q rate controls */
		FREERDP_AV1_CONTEXT_OPTION_KEYFRAME, /**< freerdp-m, set only: encode the next frame as a
		                                      * key frame */
		FREERDP_AV1_CONTEXT_OPTION_HARDWARE  /**< freerdp-m, get only: 1 if a GPU encodes or decodes
		                                      * (NVENC/NVDEC or VAAPI) */
	} FREERDP_AV1_CONTEXT_OPTION;

/** freerdp-m: the AV1 codec encodes and decodes with the GPU when the computer can (NVENC/NVDEC on
 *  NVIDIA, VAAPI on AMD and Intel), see
 *  FREERDP_AV1_CONTEXT_OPTION_QUALITY, FREERDP_AV1_CONTEXT_OPTION_KEYFRAME and
 *  FREERDP_AV1_CONTEXT_OPTION_HARDWARE. */
#define FREERDP_AV1_HARDWARE_SUPPORT 1

	WINPR_ATTR_NODISCARD
	FREERDP_API BOOL freerdp_av1_context_set_option(FREERDP_AV1_CONTEXT* av1,
	                                                FREERDP_AV1_CONTEXT_OPTION option,
	                                                UINT32 value);

	WINPR_ATTR_NODISCARD
	FREERDP_API UINT32 freerdp_av1_context_get_option(FREERDP_AV1_CONTEXT* av1,
	                                                  FREERDP_AV1_CONTEXT_OPTION option);

	WINPR_ATTR_NODISCARD
	FREERDP_API INT32 freerdp_av1_compress(FREERDP_AV1_CONTEXT* freerdp_av1, const BYTE* pSrcData,
	                                       DWORD SrcFormat, UINT32 nSrcStep, UINT32 nSrcWidth,
	                                       UINT32 nSrcHeight, const RECTANGLE_16* regionRect,
	                                       BYTE** ppDstData, UINT32* pDstSize,
	                                       RDPGFX_H264_METABLOCK* meta);

	/** @return >0 when a picture was produced, 0 when input was consumed without
	 * an output picture, or <0 on a decoding error. */
	WINPR_ATTR_NODISCARD
	FREERDP_API INT32 freerdp_av1_decompress(FREERDP_AV1_CONTEXT* freerdp_av1, const BYTE* pSrcData,
	                                         UINT32 SrcSize, BYTE* pDstData, DWORD DstFormat,
	                                         UINT32 nDstStep, UINT32 nDstWidth, UINT32 nDstHeight,
	                                         const RECTANGLE_16* regionRects, UINT32 numRegionRect);

	WINPR_ATTR_NODISCARD
	FREERDP_API BOOL freerdp_av1_context_reset(FREERDP_AV1_CONTEXT* av1, UINT32 width,
	                                           UINT32 height);

	FREERDP_API void freerdp_av1_context_free(FREERDP_AV1_CONTEXT* av1);

	/** @brief create a new encoder/decoder instance for AV1 streams
	 *
	 *  @param Compressor \b TRUE if an encoder should be created, \b FALSE for a decoder
	 *
	 *  @return An allocated instance or \b nullptr if failed.
	 *  @since version 3.25.0
	 */
	WINPR_ATTR_MALLOC(freerdp_av1_context_free, 1)
	FREERDP_API FREERDP_AV1_CONTEXT* freerdp_av1_context_new(BOOL Compressor);

#ifdef __cplusplus
}
#endif
