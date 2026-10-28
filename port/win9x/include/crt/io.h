/*
IO.H

MSVC low-level I/O of the 9x build (port/win9x/src/w9x_crt_io.c): the
Xbox build's declarations for the game's libtiff, plus what the Windows
build's platform units take from the UCRT (port/windows/src/win32_files.c,
win32_posix.c, windows_crt.c), on descriptors over KERNEL32 file handles.
*/

#ifndef __HALO_WIN9X_IO_H
#define __HALO_WIN9X_IO_H

#include <direct.h>
#include <stdint.h>

#include "../../../xbox/include/crt/io.h"

int _access(const char *path, int mode);
int _unlink(const char *path);
intptr_t _get_osfhandle(int handle);
long long _lseeki64(int handle, long long offset, int origin);
long long _filelengthi64(int handle);
int _chsize(int handle, long size);
int _chsize_s(int handle, long long size);
int _commit(int handle);
int _set_fmode(int mode);
int _get_fmode(int *mode);
int _setmode(int handle, int mode);

#define unlink _unlink
#define access _access

#endif
