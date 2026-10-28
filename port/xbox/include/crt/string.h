/*
STRING.H

nxdk's <string.h> (PDCLib, with nxdk's _stricmp, _strnicmp and strdup)
plus the other MSVC names the game uses (port/xbox/src/msvc_crt.c).
*/

#ifndef __HALO_XBOX_STRING_H
#define __HALO_XBOX_STRING_H

#include_next <string.h>

char *_strlwr(char *string);
char *_strupr(char *string);

#define _strdup strdup
#define stricmp _stricmp
#define strcmpi _stricmp
#define _strcmpi _stricmp
#define strnicmp _strnicmp

#endif
