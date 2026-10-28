/* <stdio.h> additions of the 9x build: MSVC's names for the C99 functions
   (crt/stdio.h of the Xbox build maps them again, to its MSVC printf) */

#ifndef _PDCLIB_W9X_STDIO_EXT_H
#define _PDCLIB_W9X_STDIO_EXT_H

#ifndef _snprintf
#define _snprintf snprintf
#endif
#ifndef _vsnprintf
#define _vsnprintf vsnprintf
#endif

#endif
