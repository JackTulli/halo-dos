/*
DIRECT.H

MSVC <direct.h> of the 9x build (port/win9x/src/w9x_crt_io.c), with the
POSIX names MSVC's oldnames library gives the platform layer
(port/linux/src/xbox_files.c calls rmdir). The Xbox build's crt/direct.h
includes this one and adds the game's names.
*/

#ifndef __HALO_WIN9X_DIRECT_H
#define __HALO_WIN9X_DIRECT_H

#include <stddef.h>

int _mkdir(const char *path);
int _rmdir(const char *path);
int _chdir(const char *path);
char *_getcwd(char *buffer, int size);

#ifndef rmdir
#define rmdir _rmdir
#endif
#ifndef chdir
#define chdir _chdir
#endif
#ifndef getcwd
#define getcwd _getcwd
#endif

#endif
