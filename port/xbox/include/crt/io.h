/*
IO.H

MSVC low-level I/O for the game's libtiff, over the Win32 file functions
(port/xbox/src/msvc_crt.c).
*/

#ifndef __HALO_XBOX_IO_H
#define __HALO_XBOX_IO_H

#include <stddef.h>

int _open(const char *path, int flags, ...);
int _close(int handle);
int _read(int handle, void *buffer, unsigned int count);
int _write(int handle, const void *buffer, unsigned int count);
long _lseek(int handle, long offset, int origin);
long _filelength(int handle);

#define open _open
#define close _close
#define read _read
#define write _write
#define lseek _lseek

#endif
