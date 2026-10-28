/*
TCHAR.H

MSVC <tchar.h>: the 9x build is ANSI throughout (9x implements almost none
of the W functions), so the generic-text names are the narrow ones.
dlmalloc's Win32 configuration includes it (port/win9x/src/w9x_malloc.c).
*/

#ifndef __HALO_WIN9X_TCHAR_H
#define __HALO_WIN9X_TCHAR_H

#ifndef _T
#define _T(text) text
#endif
#ifndef _TEXT
#define _TEXT(text) text
#endif

#endif
