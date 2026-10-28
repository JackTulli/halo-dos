/* _PDCLIB_fillbuffer( struct _PDCLIB_file_t * )

   This file is part of the Public Domain C Library (PDCLib).
   Permission is granted to use, modify, and / or redistribute at will.
*/

/* nxdk's implementation (lib/pdclib/platform/xbox): ReadFile. A pipe whose
   writer has gone (ERROR_BROKEN_PIPE) is the end of the stream, as it is
   for a console's standard input. */

#include <stdio.h>

#ifndef REGTEST

#include "pdclib/_PDCLIB_glue.h"

#include <errno.h>
#include <windows.h>

#include "_PDCLIB_w9x.h"

int _PDCLIB_fillbuffer( struct _PDCLIB_file_t * stream )
{
    /* No need to handle buffers > INT_MAX, as PDCLib doesn't allow them */
    DWORD amount_read = 0;

    if ( ! ReadFile( stream->handle, stream->buffer, (DWORD)stream->bufsize, &amount_read, NULL ) )
    {
        DWORD error = GetLastError();

        if ( error != ERROR_BROKEN_PIPE && error != ERROR_HANDLE_EOF )
        {
            *_PDCLIB_errno_func() = _PDCLIB_w32errno( error );
            stream->status |= _PDCLIB_ERRORFLAG;
            return EOF;
        }
        amount_read = 0;
    }
    if ( amount_read == 0 )
    {
        /* End-of-File */
        stream->status |= _PDCLIB_EOFFLAG;
        return EOF;
    }
    /* No text conversion: PDCLib's streams are binary here, as the
       Windows build makes MSVC's (_set_fmode(_O_BINARY), win32_posix.c) */
    stream->pos.offset += amount_read;
    stream->bufend = amount_read;
    stream->bufidx = 0;
    return 0;
}

#endif
