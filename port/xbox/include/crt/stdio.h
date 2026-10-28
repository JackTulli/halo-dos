/*
STDIO.H

nxdk's <stdio.h> (PDCLib) plus the MSVC names the game uses, and the game's
debug log: the game writes d:\debug.txt, which is the disc; the platform
layer sends that file to the cache partition and echoes it to the screen
(port/xbox/src/msvc_crt.c).
*/

#ifndef __HALO_XBOX_STDIO_H
#define __HALO_XBOX_STDIO_H

#include <stdarg.h>
#include_next <stdio.h>

/* declared here for units compiled as C89, which PDCLib's header leaves
without the C99 functions */
int snprintf(char *buffer, size_t count, const char *format, ...);
int vsnprintf(char *buffer, size_t count, const char *format, va_list arguments);

/* MSVC's _snprintf does not terminate a truncated string; always
terminating is strictly safer for callers written against it. */
#define _snprintf snprintf
#define _vsnprintf vsnprintf

/* no descriptors stand behind PDCLib's streams: always NULL */
FILE *fdopen(int handle, const char *mode);
#define _fdopen fdopen

/* The game's formats carry MSVC's length modifiers (%I64d, %I32x), which
PDCLib does not read, and PDCLib neither formats floating point (%f, %e, %g)
nor takes it from the argument list: either takes the following arguments
out of step (a %s after a %f read half a double as its pointer). The printf
family here (msvc_crt.c) translates the one and formats the other, for the
game and the platform layer alike. printf goes to the screen and the log. */
int halo_xbox_vsnprintf(char *buffer, size_t count, const char *format, va_list arguments);
int halo_xbox_snprintf(char *buffer, size_t count, const char *format, ...);
int halo_xbox_vsprintf(char *buffer, const char *format, va_list arguments);
int halo_xbox_sprintf(char *buffer, const char *format, ...);

#if defined(HALO_LINUX_PLATFORM_LAYER) && !defined(HALO_XBOX_PRINTF_IMPLEMENTATION)
#undef _snprintf
#undef _vsnprintf
#define snprintf halo_xbox_snprintf
#define vsnprintf halo_xbox_vsnprintf
#define _snprintf halo_xbox_snprintf
#define _vsnprintf halo_xbox_vsnprintf
#define sprintf halo_xbox_sprintf
#define vsprintf halo_xbox_vsprintf
#endif

#ifndef HALO_LINUX_PLATFORM_LAYER
int halo_xbox_vprintf(const char *format, va_list arguments);
int halo_xbox_printf(const char *format, ...);
FILE *halo_xbox_fopen(const char *path, const char *mode);
int halo_xbox_fprintf(FILE *stream, const char *format, ...);
int halo_xbox_vfprintf(FILE *stream, const char *format, va_list arguments);
int halo_xbox_fputs(const char *string, FILE *stream);
#undef _snprintf
#undef _vsnprintf
#define snprintf halo_xbox_snprintf
#define vsnprintf halo_xbox_vsnprintf
#define _snprintf halo_xbox_snprintf
#define _vsnprintf halo_xbox_vsnprintf
#define sprintf halo_xbox_sprintf
#define vsprintf halo_xbox_vsprintf
#define printf halo_xbox_printf
#define vprintf halo_xbox_vprintf
#define fopen halo_xbox_fopen
#define fprintf halo_xbox_fprintf
#define vfprintf halo_xbox_vfprintf
#define fputs halo_xbox_fputs
#endif

#endif
