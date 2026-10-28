/* The 9x build's PDCLib glue: what its units share

   This file is part of the Public Domain C Library (PDCLib) glue of the
   Windows 98 SE / Me build of the game (port/win9x/README.md).

   The platform layer builds host paths with '/' (port/linux/src/
   xbox_files.c); the 9x file functions are documented for '\' only, so
   every name is copied with its separators turned before it reaches
   KERNEL32. A name too long for MAX_PATH fails with ENAMETOOLONG.
*/

#ifndef _PDCLIB_W9X_H
#define _PDCLIB_W9X_H

#include <stddef.h>

#define _PDCLIB_W9X_PATH_MAX 260

/* the name with '/' as '\'; NULL (errno set) when it does not fit */
char * _PDCLIB_w9x_path( const char * name, char * buffer );

/* the Win32 error as an errno value (_PDCLIB_w32errno.c) */
int _PDCLIB_w32errno( unsigned long werror );

#endif
