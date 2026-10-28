/* _PDCLIB_w9x_path( const char *, char * )

   This file is part of the Public Domain C Library (PDCLib) glue of the
   Windows 98 SE / Me build of the game. See _PDCLIB_w9x.h.
*/

#include <errno.h>

#include "_PDCLIB_w9x.h"

char * _PDCLIB_w9x_path( const char * name, char * buffer )
{
    size_t i;

    for ( i = 0; name[i]; ++i )
    {
        if ( i + 1 >= _PDCLIB_W9X_PATH_MAX )
        {
            errno = ENAMETOOLONG;
            return NULL;
        }
        buffer[i] = ( name[i] == '/' ) ? '\\' : name[i];
    }
    buffer[i] = '\0';
    return buffer;
}
