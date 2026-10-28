/* remove( const char * )

   This file is part of the Public Domain C Library (PDCLib).
   Permission is granted to use, modify, and / or redistribute at will.
*/

/* nxdk's implementation (lib/pdclib/platform/xbox): DeleteFileA, with '/'
   in the name turned into '\'. An open stream's file is not removed. */

#include <stdio.h>

#ifndef REGTEST

#include <errno.h>
#include <string.h>

#include <windows.h>

#include "../_PDCLIB/_PDCLIB_w9x.h"

extern struct _PDCLIB_file_t * _PDCLIB_filelist;

int remove( const char * pathname )
{
    char path[ _PDCLIB_W9X_PATH_MAX ];
    struct _PDCLIB_file_t * current = _PDCLIB_filelist;

    while ( current != NULL )
    {
        if ( ( current->filename != NULL ) && ( strcmp( current->filename, pathname ) == 0 ) )
        {
            *_PDCLIB_errno_func() = EACCES;
            return EOF;
        }
        current = current->next;
    }

    if ( _PDCLIB_w9x_path( pathname, path ) == NULL )
    {
        return -1;
    }
    if ( DeleteFileA( path ) )
    {
        return 0;
    }
    *_PDCLIB_errno_func() = _PDCLIB_w32errno( GetLastError() );
    return -1;
}

#endif
