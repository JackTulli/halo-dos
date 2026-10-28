/*
STDIO.H

<stdio.h> of the 9x build: PDCLib's, with the Xbox build's MSVC printf
(port/xbox/include/crt/stdio.h) for the game and the Xbox-facing platform
units. Game units then open their files through the Windows build's
wrappers, which translate the game's Xbox paths (d:\..., z:\...) the way
CreateFile does (port/windows/include/crt/halo_windows_file_names.h,
port/windows/src/windows_crt.c), instead of the Xbox build's log-aware
fopen.

The Windows-facing units (which see no prefix header, so no HALO_WINDOWS)
get PDCLib's <stdio.h> as it is.
*/

#ifndef __HALO_WIN9X_STDIO_H
#define __HALO_WIN9X_STDIO_H

#include <stdarg.h>
#include_next <stdio.h>

/* the MSVC printf without the Xbox build's renames (crt/stdio.h of the Xbox
build defines them for the game): fprintf to a stream through the same
preparation (port/xbox/src/msvc_printf.c) */
int halo_msvc_vfprintf(FILE *stream, const char *format, va_list arguments);

#ifdef HALO_LINUX_PLATFORM_LAYER
/* The Xbox-facing platform units print to streams too (platform_log's
halo.log and stderr, port/linux/src/xbox_kernel.c), with %f and %g, which
PDCLib's own printf neither prints nor takes from the arguments, so that a
%s after one reads the wrong argument: theirs go through the same
preparation as the game's (w9x_crt.c). printf itself keeps its name, which
platform.h's format attributes use; no platform unit calls it. */
int halo_xbox_fprintf(FILE *stream, const char *format, ...);
int halo_xbox_vfprintf(FILE *stream, const char *format, va_list arguments);
#define fprintf halo_xbox_fprintf
#define vfprintf halo_xbox_vfprintf
#endif

#if defined(HALO_WINDOWS) && !defined(HALO_LINUX_PLATFORM_LAYER)
#undef fopen
#include "../../../../chupathingyce/port/windows/include/crt/halo_windows_file_names.h"
#endif

#endif
