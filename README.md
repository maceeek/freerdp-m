# FreeRDP-M

FreeRDP-M is a fork of [FreeRDP](https://github.com/FreeRDP/FreeRDP) 3.32 that
encodes and decodes the AV1 codec of the graphics pipeline with NVIDIA
hardware: NVENC on the server, NVDEC on the client. It is used by
[KRdp-M](https://github.com/maceeek/krdp-m) and builds as a drop-in
replacement of the `freerdp` package, for Arch Linux and CachyOS see
[Installing on Arch Linux and CachyOS](#installing-on-arch-linux-and-cachyos).

## AV1

AV1 is a FreeRDP extension of the graphics pipeline ([MS-RDPEGFX]): the
client advertises the capability set `RDPGFX_CAPVERSION_FRDP_1` and the
server sends surface commands with the codec `RDPGFX_CODECID_AV1`. Only
FreeRDP based clients and servers support it, Windows and mstsc keep using
H.264 or the other codecs.

Upstream FreeRDP only has a slow software encoder (libaom) and software
decoders (dav1d, libaom) for it, and its packages are built without AV1.
FreeRDP-M builds with AV1 and picks the codec implementation when a context
is created:

| | NVIDIA GPU with AV1 support | Otherwise |
|---|---|---|
| Encoding (server) | NVENC | AV1 is not offered (libaom if built with `-DWITH_AOM=ON`) |
| Decoding (client) | NVDEC | dav1d |

- NVENC encodes AV1 since the GeForce RTX 40 series, NVDEC decodes it since
  the RTX 30 series. Whether NVENC can encode AV1 is checked once per process.
- AV1 4:4:4 (`/gfx:AV1:i444`, the default profile) is encoded when NVENC can,
  otherwise 4:2:0. NVDEC only decodes 4:2:0, a 4:4:4 stream from another
  server is decoded with dav1d.
- If NVDEC can not decode a stream at all, dav1d takes over with the first
  frame.
- The colour conversion is the one of the other FreeRDP AV1 implementations
  (BT.709, full range), so NVENC streams decode correctly in upstream
  FreeRDP clients built with AV1 and the other way around.
- `FREERDP_AV1_HWACCEL=0` turns NVENC and NVDEC off, e.g. to compare with
  the software implementation.
- `/gfx:AV1` enables AV1 in the clients (it is on by default with
  `/gfx`). Upstream FreeRDP 3.32 turned it *off* with a plain `/gfx:AV1`,
  that is fixed here.

### API

`freerdp/codec/av1.h` defines `FREERDP_AV1_HARDWARE_SUPPORT` and adds these
options to `freerdp_av1_context_set_option()` /
`freerdp_av1_context_get_option()`:

| Option | |
|---|---|
| `FREERDP_AV1_CONTEXT_OPTION_QUALITY` | 0 (worst) - 100 (best), used by the `FREERDP_AV1_CQ` and `FREERDP_AV1_Q` rate controls |
| `FREERDP_AV1_CONTEXT_OPTION_KEYFRAME` | Set only: encode the next frame as a key frame |
| `FREERDP_AV1_CONTEXT_OPTION_HARDWARE` | Get only: 1 if the context encodes with NVENC or decodes with NVDEC |

A server, e.g. KRdp-M, encodes with:

```c
FREERDP_AV1_CONTEXT* av1 = freerdp_av1_context_new(TRUE); /* nullptr: no NVENC */
freerdp_av1_context_reset(av1, width, height);
freerdp_av1_context_set_option(av1, FREERDP_AV1_CONTEXT_OPTION_RATECONTROL, FREERDP_AV1_CQ);
freerdp_av1_context_set_option(av1, FREERDP_AV1_CONTEXT_OPTION_QUALITY, 80);
...
RDPGFX_AVC420_BITMAP_STREAM stream = { 0 };
if (freerdp_av1_compress(av1, bgra, PIXEL_FORMAT_BGRX32, stride, width, height, &rect,
                         &stream.data, &stream.length, &stream.meta) > 0)
{
    /* send a surface command with codecId RDPGFX_CODECID_AV1 and extra = &stream */
}
free_h264_metablock(&stream.meta);
```

and confirms `RDPGFX_CAPVERSION_FRDP_1` when the client advertises it. The
flag `RDPGFX_CAPS_FLAG_AV1_I444_DISABLED` of that capability set means the
client wants 4:2:0 only, set `FREERDP_AV1_CONTEXT_OPTION_PROFILE` to 0 then.

## Installing on Arch Linux and CachyOS

```sh
cd packaging/archlinux
makepkg -si
```

This builds the `main` branch of this repository and replaces `freerdp`, so
KRdp-M, KRDC and Remmina use it after they are restarted.

## Updating from upstream

The fork is based on the release tags of FreeRDP:

```sh
git remote add upstream https://github.com/FreeRDP/FreeRDP.git
git fetch upstream --tags
git rebase <new tag>
```

The changes are in `libfreerdp/codec/av1_ffmpeg.c` (NVENC/NVDEC) and small
hooks in `libfreerdp/codec/av1.c`, `include/freerdp/codec/av1.h`,
`client/common/cmdline.c` and `channels/rdpgfx/server/rdpgfx_main.c`.

---

# FreeRDP: A Remote Desktop Protocol Implementation

FreeRDP is a free implementation of the Remote Desktop Protocol (RDP), released under the Apache license.
Enjoy the freedom of using your software wherever you want, the way you want it, in a world where
interoperability can finally liberate your computing experience.

## Code Quality Status

[![abi-checker](https://github.com/FreeRDP/FreeRDP/actions/workflows/abi-checker.yml/badge.svg)](https://github.com/FreeRDP/FreeRDP/actions/workflows/abi-checker.yml)
[![clang-tidy-review](https://github.com/FreeRDP/FreeRDP/actions/workflows/clang-tidy.yml/badge.svg?event=pull_request_target)](https://github.com/FreeRDP/FreeRDP/actions/workflows/clang-tidy.yml)
[![CodeQL](https://github.com/FreeRDP/FreeRDP/actions/workflows/codeql-analysis.yml/badge.svg?branch=master)](https://github.com/FreeRDP/FreeRDP/actions/workflows/codeql-analysis.yml)
[![mingw-builder](https://github.com/FreeRDP/FreeRDP/actions/workflows/mingw.yml/badge.svg)](https://github.com/FreeRDP/FreeRDP/actions/workflows/mingw.yml)
[![macos-builder](https://github.com/FreeRDP/FreeRDP/actions/workflows/macos.yml/badge.svg)](https://github.com/FreeRDP/FreeRDP/actions/workflows/macos.yml)
[![[arm,ppc,ricsv] architecture builds](https://github.com/FreeRDP/FreeRDP/actions/workflows/alt-architectures.yml/badge.svg)](https://github.com/FreeRDP/FreeRDP/actions/workflows/alt-architectures.yml)
[![[freebsd] architecture builds](https://github.com/FreeRDP/FreeRDP/actions/workflows/freebsd.yml/badge.svg)](https://github.com/FreeRDP/FreeRDP/actions/workflows/freebsd.yml)
[![coverity](https://scan.coverity.com/projects/616/badge.svg)](https://scan.coverity.com/projects/freerdp)

## Resources

Project website: https://www.freerdp.com/

Issue tracker: https://github.com/FreeRDP/FreeRDP/issues

Sources: https://github.com/FreeRDP/FreeRDP/

Downloads: https://pub.freerdp.com/releases/

Wiki: https://github.com/FreeRDP/FreeRDP/wiki

API documentation: https://pub.freerdp.com/api/

Security policy: https://github.com/FreeRDP/FreeRDP/security/policy

FAQ: https://github.com/FreeRDP/FreeRDP/wiki/FAQ

### Contact

* Matrix room : `#FreeRDP:matrix.org` (main)
  * IRC channel : `#freerdp @ irc.oftc.net` (bridged)
* Mailing list: https://lists.sourceforge.net/lists/listinfo/freerdp-devel

## Microsoft Open Specifications

Information regarding the Microsoft Open Specifications can be found at:
https://www.microsoft.com/openspecifications/

A list of reference documentation is maintained here:
https://github.com/FreeRDP/FreeRDP/wiki/Reference-Documentation

## Compilation

Instructions on how to get started compiling FreeRDP can be found on the wiki:
https://github.com/FreeRDP/FreeRDP/wiki/Compilation
