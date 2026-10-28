/* <string.h> additions of the 9x build: the MSVC names the game and the
   platform layer call (port/win9x/src/w9x_crt.c) */

#ifndef _PDCLIB_W9X_STRING_EXT_H
#define _PDCLIB_W9X_STRING_EXT_H

char * strdup( const char * s );
int _stricmp( const char * s1, const char * s2 );
int _strnicmp( const char * s1, const char * s2, size_t n );

#define _strdup strdup

#endif
