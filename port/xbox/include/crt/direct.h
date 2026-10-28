/*
DIRECT.H

nxdk's <direct.h> (_mkdir, _rmdir, _chdir, _getcwd) under the game's
names.
*/

#ifndef __HALO_XBOX_DIRECT_H
#define __HALO_XBOX_DIRECT_H

#include_next <direct.h>

#define mkdir(path) _mkdir(path)
#define rmdir _rmdir
#define chdir _chdir
#define getcwd _getcwd

#endif
