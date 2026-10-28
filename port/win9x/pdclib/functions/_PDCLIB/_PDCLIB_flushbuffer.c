/* _PDCLIB_flushbuffer( struct _PDCLIB_file_t * )

   This file is part of the Public Domain C Library (PDCLib).
   Permission is granted to use, modify, and / or redistribute at will.
*/

/* nxdk's implementation (lib/pdclib/platform/xbox): WriteFile, again while
   it makes progress (a console or a pipe may take part of a buffer). */

#include <stdio.h>
#include <string.h>

#ifndef REGTEST

#include "pdclib/_PDCLIB_glue.h"

#include <errno.h>
#include <windows.h>

#include "_PDCLIB_w9x.h"

int _PDCLIB_flushbuffer( struct _PDCLIB_file_t * stream )
{
    /* No need to handle buffers > INT_MAX, as PDCLib doesn't allow them */
    _PDCLIB_size_t written = 0;

    if ( ( stream->status & _PDCLIB_FAPPEND ) && stream->bufidx > 0 )
    {
        /* "a": every write at the end, wherever the stream was positioned
           (Win32 has no O_APPEND to do it) */
        LONG high = 0;
        DWORD low;

        SetLastError( NO_ERROR );
        low = SetFilePointer( stream->handle, 0, &high, FILE_END );
        if ( low != INVALID_SET_FILE_POINTER || GetLastError() == NO_ERROR )
        {
            stream->pos.offset = (_PDCLIB_int64_t)( ( (unsigned long long)(DWORD)high << 32 ) | low );
        }
    }

    while ( written < stream->bufidx )
    {
        DWORD amount_written = 0;

        SetLastError( NO_ERROR );
        if ( ! WriteFile( stream->handle, stream->buffer + written, (DWORD)( stream->bufidx - written ), &amount_written, NULL ) || amount_written == 0 )
        {
            /* Write error, or no progress */
            DWORD error = GetLastError();

            *_PDCLIB_errno_func() = error == NO_ERROR ? EIO : _PDCLIB_w32errno( error );
            stream->status |= _PDCLIB_ERRORFLAG;
            /* Move unwritten remains to begin of buffer. */
            stream->bufidx -= written;
            memmove( stream->buffer, stream->buffer + written, stream->bufidx );
            return EOF;
        }
        written += (_PDCLIB_size_t)amount_written;
        stream->pos.offset += amount_written;
    }
    /* Buffer written completely. */
    stream->bufidx = 0;
    return 0;
}

#endif
