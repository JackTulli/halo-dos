/* <stdlib.h> additions of the 9x build: the MSVC names (port/win9x/src) */

#ifndef _PDCLIB_W9X_STDLIB_EXT_H
#define _PDCLIB_W9X_STDLIB_EXT_H

unsigned short _byteswap_ushort( unsigned short value );
unsigned long _byteswap_ulong( unsigned long value );
unsigned long long _byteswap_uint64( unsigned long long value );

#define MB_CUR_MAX 1

#define __min(a,b) (((a) < (b)) ? (a) : (b))
#define __max(a,b) (((a) > (b)) ? (a) : (b))

#define _sys_nerr _PDCLIB_ERRNO_MAX

#endif
