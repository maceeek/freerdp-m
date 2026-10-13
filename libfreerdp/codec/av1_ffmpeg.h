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

#ifndef FREERDP_LIB_CODEC_AV1_FFMPEG_H
#define FREERDP_LIB_CODEC_AV1_FFMPEG_H

#include <winpr/wlog.h>

#include <freerdp/api.h>
#include <freerdp/codec/av1.h>

typedef struct S_AV1_FFMPEG_CONTEXT AV1_FFMPEG_CONTEXT;

/** @brief Whether the hardware backend may be used, FREERDP_AV1_HWACCEL=0 turns it off,
 *  =nvidia or =vaapi limits it to one. */
WINPR_ATTR_NODISCARD
FREERDP_LOCAL BOOL av1_ffmpeg_enabled(void);

/** @brief Whether a GPU of this computer can encode AV1, probed once per process. */
WINPR_ATTR_NODISCARD
FREERDP_LOCAL BOOL av1_ffmpeg_encoder_available(void);

FREERDP_LOCAL void av1_ffmpeg_free(AV1_FFMPEG_CONTEXT* ctx);

/** @brief "NVENC", "NVDEC" or "VAAPI" */
WINPR_ATTR_NODISCARD
FREERDP_LOCAL const char* av1_ffmpeg_backend_name(const AV1_FFMPEG_CONTEXT* ctx);

/** @brief Create a GPU encoder or decoder: NVENC/NVDEC first, then VAAPI.
 *
 *  @return nullptr if the hardware is not available.
 */
WINPR_ATTR_MALLOC(av1_ffmpeg_free, 1)
FREERDP_LOCAL AV1_FFMPEG_CONTEXT* av1_ffmpeg_new(wLog* log, BOOL encoder);

WINPR_ATTR_NODISCARD
FREERDP_LOCAL BOOL av1_ffmpeg_reset(AV1_FFMPEG_CONTEXT* ctx, UINT32 width, UINT32 height);

WINPR_ATTR_NODISCARD
FREERDP_LOCAL BOOL av1_ffmpeg_set_option(AV1_FFMPEG_CONTEXT* ctx, FREERDP_AV1_CONTEXT_OPTION option,
                                         UINT32 value);

WINPR_ATTR_NODISCARD
FREERDP_LOCAL UINT32 av1_ffmpeg_get_option(AV1_FFMPEG_CONTEXT* ctx,
                                           FREERDP_AV1_CONTEXT_OPTION option);

/** @return 1 if a frame was encoded, 0 if the encoder has no output yet, <0 on error.
 *  The data stays valid until the next call. */
WINPR_ATTR_NODISCARD
FREERDP_LOCAL INT32 av1_ffmpeg_compress(AV1_FFMPEG_CONTEXT* ctx, const BYTE* pSrcData,
                                        DWORD SrcFormat, UINT32 nSrcStep, UINT32 nSrcWidth,
                                        UINT32 nSrcHeight, BYTE** ppDstData, UINT32* pDstSize);

/** @return >= 0 on success, <0 on error. */
WINPR_ATTR_NODISCARD
FREERDP_LOCAL INT32 av1_ffmpeg_decompress(AV1_FFMPEG_CONTEXT* ctx, const BYTE* pSrcData,
                                          UINT32 SrcSize, BYTE* pDstData, DWORD DstFormat,
                                          UINT32 nDstStep, UINT32 nDstWidth, UINT32 nDstHeight);

/** @brief Whether the decoder produced a picture yet. Until it did, a decoding error means the
 *  hardware cannot decode the stream (e.g. 4:4:4) and the software decoder should take over. */
WINPR_ATTR_NODISCARD
FREERDP_LOCAL BOOL av1_ffmpeg_has_decoded(const AV1_FFMPEG_CONTEXT* ctx);

#endif /* FREERDP_LIB_CODEC_AV1_FFMPEG_H */
