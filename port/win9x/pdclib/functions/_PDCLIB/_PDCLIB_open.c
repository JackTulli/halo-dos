/* _PDCLIB_open( const char * const, int )

   This file is part of the Public Domain C Library (PDCLib).
   Permission is granted to use, modify, and / or redistribute at will.
*/

/* nxdk's implementation (lib/pdclib/platform/xbox) for a Win32 host that
   is Windows 98 SE / Me: no FILE_SHARE_DELETE (9x refuses the flag), no
   SetFilePointerEx (Windows 2000), '/' in names turned into '\', "w+"
   creates or truncates (CREATE_ALWAYS alone), and "a+" opens (nxdk's case
   for it lacks _PDCLIB_FWRITE, which _PDCLIB_filemode sets for every 'a',
   so "a+" fell to the invalid mode).
*/

#include <stdio.h>

#ifndef REGTEST

#include "pdclib/_PDCLIB_glue.h"

#include <errno.h>
#include <windows.h>

#include "_PDCLIB_w9x.h"

_PDCLIB_fd_t _PDCLIB_open( const char * const filename, unsigned int mode )
{
    DWORD access_flags = 0;
    DWORD create_flags = 0;
    char path[ _PDCLIB_W9X_PATH_MAX ];
    HANDLE handle;

    switch ( mode & ( _PDCLIB_FREAD | _PDCLIB_FWRITE | _PDCLIB_FAPPEND | _PDCLIB_FRW ) )
    {
        case _PDCLIB_FREAD: /* "r" */
            access_flags = GENERIC_READ;
            create_flags = OPEN_EXISTING;
            break;
        case _PDCLIB_FWRITE: /* "w" */
            access_flags = GENERIC_WRITE;
            create_flags = CREATE_ALWAYS;
            break;
        case _PDCLIB_FWRITE | _PDCLIB_FAPPEND: /* "a" */
            access_flags = GENERIC_WRITE;
            create_flags = OPEN_ALWAYS;
            break;
        case _PDCLIB_FREAD | _PDCLIB_FRW: /* "r+" */
            access_flags = GENERIC_READ | GENERIC_WRITE;
            create_flags = OPEN_EXISTING;
            break;
        case _PDCLIB_FWRITE | _PDCLIB_FRW: /* "w+" */
            access_flags = GENERIC_READ | GENERIC_WRITE;
            create_flags = CREATE_ALWAYS;
            break;
        case _PDCLIB_FWRITE | _PDCLIB_FAPPEND | _PDCLIB_FRW: /* "a+" (_PDCLIB_filemode's 'a' is FWRITE | FAPPEND) */
            access_flags = GENERIC_READ | GENERIC_WRITE;
            create_flags = OPEN_ALWAYS;
            break;
        default: /* Invalid mode */
            *_PDCLIB_errno_func() = EINVAL;
            return _PDCLIB_NOHANDLE;
    }

    if ( _PDCLIB_w9x_path( filename, path ) == NULL )
    {
        return _PDCLIB_NOHANDLE;
    }

    handle = CreateFileA( path, access_flags, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, create_flags, FILE_ATTRIBUTE_NORMAL, NULL );

    if ( handle == INVALID_HANDLE_VALUE )
    {
        *_PDCLIB_errno_func() = _PDCLIB_w32errno( GetLastError() );
        return _PDCLIB_NOHANDLE;
    }

    if ( mode & _PDCLIB_FAPPEND )
    {
        LONG high = 0;

        SetLastError( NO_ERROR );
        if ( SetFilePointer( handle, 0, &high, FILE_END ) == INVALID_SET_FILE_POINTER && GetLastError() != NO_ERROR )
        {
            *_PDCLIB_errno_func() = _PDCLIB_w32errno( GetLastError() );
            CloseHandle( handle );
            return _PDCLIB_NOHANDLE;
        }
    }
    return handle;
}

#endif
