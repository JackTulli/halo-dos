/*
STDLIB.H

nxdk's <stdlib.h> (PDCLib) plus MSVC names and constants
(port/xbox/src/msvc_crt.c).
*/

#ifndef __HALO_XBOX_STDLIB_H
#define __HALO_XBOX_STDLIB_H

#include_next <stdlib.h>

#define _MAX_PATH 260
#define _MAX_DRIVE 3
#define _MAX_DIR 256
#define _MAX_FNAME 256
#define _MAX_EXT 256

char *_itoa(int value, char *string, int radix);
char *_ltoa(long value, char *string, int radix);
char *_ultoa(unsigned long value, char *string, int radix);
void _splitpath(const char *path, char *drive, char *directory, char *name, char *extension);
void _makepath(char *path, const char *drive, const char *directory, const char *name, const char *extension);

#define itoa _itoa
#define ltoa _ltoa
#define ultoa _ultoa

#endif
