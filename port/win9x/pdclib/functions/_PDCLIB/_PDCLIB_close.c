/* _PDCLIB_close( _PDCLIB_fd_t )

   This file is part of the Public Domain C Library (PDCLib).
   Permission is granted to use, modify, and / or redistribute at will.
*/

/* nxdk's implementation (lib/pdclib/platform/xbox), Win32 as it was */

#include <stdio.h>

#ifndef REGTEST

#include "pdclib/_PDCLIB_glue.h"

#include <errno.h>
#include <windows.h>

#include "_PDCLIB_w9x.h"

int _PDCLIB_close( _PDCLIB_fd_t fd )
{
    if ( CloseHandle( fd ) )
    {
        return 0;
    }
    *_PDCLIB_errno_func() = _PDCLIB_w32errno( GetLastError() );
    return -1;
}

#endif
