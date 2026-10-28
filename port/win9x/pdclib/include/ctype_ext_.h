/* <ctype.h> additions of the 9x build: the MSVC names */

#ifndef _PDCLIB_W9X_CTYPE_EXT_H
#define _PDCLIB_W9X_CTYPE_EXT_H

#define isascii(c) ((unsigned)(c) < 128)
#define __isascii(c) isascii(c)
#define toascii(c) ((c) & 0x7F)

#endif
