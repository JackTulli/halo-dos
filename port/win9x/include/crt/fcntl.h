/*
FCNTL.H

The MSVC open() flags of the Xbox build, plus the ones the Windows build's
platform units name (port/windows/include/crt/fcntl.h): 9x has no handle
inheritance worth refusing, so _O_NOINHERIT (and O_CLOEXEC with it) is
accepted and ignored (port/win9x/src/w9x_crt_io.c).
*/

#ifndef __HALO_WIN9X_FCNTL_H
#define __HALO_WIN9X_FCNTL_H

#include "../../../xbox/include/crt/fcntl.h"

#define _O_RANDOM 0x0010
#define _O_SEQUENTIAL 0x0020
#define _O_TEMPORARY 0x0040
#define _O_NOINHERIT 0x0080
#define _O_SHORT_LIVED 0x1000

#ifndef O_ACCMODE
#define O_ACCMODE (_O_RDONLY | _O_WRONLY | _O_RDWR)
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC _O_NOINHERIT
#endif
#define O_NOINHERIT _O_NOINHERIT
#define O_TEMPORARY _O_TEMPORARY
#define O_SEQUENTIAL _O_SEQUENTIAL
#define O_RANDOM _O_RANDOM

#endif
