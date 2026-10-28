/* int64_t _PDCLIB_seek( FILE *, int64_t, int )

   This file is part of the Public Domain C Library (PDCLib).
   Permission is granted to use, modify, and / or redistribute at will.
*/

/* nxdk's implementation (lib/pdclib/platform/xbox) with SetFilePointer and
   its high dword in place of SetFilePointerEx, which Windows 9x lacks. */

#include <stdio.h>

#ifndef REGTEST

#include "pdclib/_PDCLIB_glue.h"

#include <errno.h>
#include <windows.h>

#include "_PDCLIB_w9x.h"

_PDCLIB_int64_t _PDCLIB_seek( struct _PDCLIB_file_t * stream, _PDCLIB_int64_t offset, int whence )
{
    DWORD method;
    LONG high = (LONG)( offset >> 32 );
    DWORD low;

    switch ( whence )
    {
        case SEEK_SET: method = FILE_BEGIN; break;
        case SEEK_CUR: method = FILE_CURRENT; break;
        case SEEK_END: method = FILE_END; break;
        default:
            *_PDCLIB_errno_func() = EINVAL;
            return EOF;
    }

    SetLastError( NO_ERROR );
    low = SetFilePointer( stream->handle, (LONG)( offset & 0xFFFFFFFF ), &high, method );
    if ( low == INVALID_SET_FILE_POINTER && GetLastError() != NO_ERROR )
    {
        *_PDCLIB_errno_func() = _PDCLIB_w32errno( GetLastError() );
        return EOF;
    }
    stream->ungetidx = 0;
    stream->bufidx = 0;
    stream->bufend = 0;
    stream->pos.offset = (_PDCLIB_int64_t)( ( (unsigned long long)(DWORD)high << 32 ) | low );
    return stream->pos.offset;
}

#endif
