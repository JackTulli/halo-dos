/*
W9X_WIDE_FILES.C

The game's files opened by wide name (port/linux/src/msvc_wide.c:
_wfopen, _wfreopen, _wremove), which the Windows build takes from the UCRT
and the 9x build, whose C runtime has no wide functions, from msvc_wide.c
as the Linux and Xbox builds do. msvc_wide.c narrows the name and calls
the build's path-translating wrappers by their Linux names; on 9x those
are the Windows build's (port/windows/src/windows_crt.c).

The narrowed name is UTF-8, which is the system's code page only for
ASCII; the game's own names are.
*/

#include "platform.h"

#include <stdio.h>

FILE *halo_windows_fopen(const char *path, const char *mode);
FILE *halo_windows_freopen(const char *path, const char *mode, FILE *stream);
int halo_windows_remove(const char *path);

FILE *halo_linux_fopen(const char *path, const char *mode)
{
	return halo_windows_fopen(path, mode);
}

FILE *halo_linux_freopen(const char *path, const char *mode, FILE *stream)
{
	return halo_windows_freopen(path, mode, stream);
}

int halo_linux_remove(const char *path)
{
	return halo_windows_remove(path);
}
