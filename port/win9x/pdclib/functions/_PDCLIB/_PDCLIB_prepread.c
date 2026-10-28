/* _PDCLIB_prepread( struct _PDCLIB_file_t * )

   This file is part of the Public Domain C Library (PDCLib).
   Permission is granted to use, modify, and / or redistribute at will.
*/

/* PDCLib's generic one, with update streams that start out writing
   readable: the generic one refused every stream with _PDCLIB_FAPPEND or
   _PDCLIB_FWRITE in its status, and _PDCLIB_filemode puts both in an "a+"
   stream's (and _PDCLIB_FWRITE in a "w+" one's) for good, so such a stream
   could never be read, even after the fseek C asks for between a write and
   a read, and the refusal's error flag then failed its writes too. Here an
   update stream (_PDCLIB_FRW) with no output waiting in its buffer turns
   to reading, as fseek turns it; the waiting output still needs an fseek
   or fflush first, as in C. */

#include <stdio.h>

#ifndef REGTEST

#include "pdclib/_PDCLIB_glue.h"

int _PDCLIB_prepread( struct _PDCLIB_file_t * stream )
{
    if ( ( stream->status & ( _PDCLIB_FWRITE | _PDCLIB_FAPPEND ) ) &&
         ( stream->status & _PDCLIB_FRW ) &&
         ! ( stream->status & _PDCLIB_FREAD ) &&
         stream->bufidx == 0 )
    {
        /* an update stream with nothing to write: reads from here */
        stream->status &= ~ _PDCLIB_FWRITE;
        stream->bufend = 0;
    }

    if ( ( stream->bufidx > stream->bufend ) ||
         ( stream->status & ( _PDCLIB_FWRITE | _PDCLIB_ERRORFLAG | _PDCLIB_WIDESTREAM | _PDCLIB_EOFFLAG ) ) ||
         ( ( stream->status & _PDCLIB_FAPPEND ) && ! ( stream->status & _PDCLIB_FRW ) ) ||
         ! ( stream->status & ( _PDCLIB_FREAD | _PDCLIB_FRW ) ) )
    {
        /* Function called on illegal (e.g. output) stream. */
        *_PDCLIB_errno_func() = _PDCLIB_EBADF;
        stream->status |= _PDCLIB_ERRORFLAG;
        return EOF;
    }

    stream->status |= _PDCLIB_FREAD | _PDCLIB_BYTESTREAM;

    if ( ( stream->bufidx == stream->bufend ) && ( stream->ungetidx == 0 ) )
    {
        return _PDCLIB_fillbuffer( stream );
    }
    else
    {
        return 0;
    }
}

#endif
