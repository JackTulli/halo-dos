/* _PDCLIB_rename( const char *, const char * )

   This file is part of the Public Domain C Library (PDCLib).
   Permission is granted to use, modify, and / or redistribute at will.
*/

/* nxdk's implementation (lib/pdclib/platform/xbox): MoveFileA, with '/'
   in the names turned into '\'. As MSVC's rename, it fails when the new
   name exists (MoveFileExA's MOVEFILE_REPLACE_EXISTING is NT's). */

#include <stdio.h>

#ifndef REGTEST

#include "pdclib/_PDCLIB_glue.h"

#include <errno.h>
#include <windows.h>

#include "_PDCLIB_w9x.h"

int _PDCLIB_rename( const char * old, const char * new )
{
    char old_path[ _PDCLIB_W9X_PATH_MAX ];
    char new_path[ _PDCLIB_W9X_PATH_MAX ];

    if ( _PDCLIB_w9x_path( old, old_path ) == NULL || _PDCLIB_w9x_path( new, new_path ) == NULL )
    {
        return -1;
    }
    if ( MoveFileA( old_path, new_path ) )
    {
        return 0;
    }
    *_PDCLIB_errno_func() = _PDCLIB_w32errno( GetLastError() );
    return -1;
}

#endif
