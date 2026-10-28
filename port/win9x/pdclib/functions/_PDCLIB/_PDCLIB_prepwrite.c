/* _PDCLIB_prepwrite( struct _PDCLIB_file_t * )

   This file is part of the Public Domain C Library (PDCLib).
   Permission is granted to use, modify, and / or redistribute at will.
*/

/* PDCLib's generic one, with update streams that start out reading
   writable: _PDCLIB_filemode puts _PDCLIB_FREAD in an "r+" stream's status,
   which the generic one refused, so a write straight after fopen( "r+" )
   failed (the UCRT's succeeds). Here an update stream (_PDCLIB_FRW) with no
   input left in its buffer turns to writing, at the position the system's
   file is at (which is the stream's then); input still in the buffer needs
   an fseek first, as in C. The counterpart of _PDCLIB_prepread.c. */

#include <stdio.h>

#ifndef REGTEST

#include "pdclib/_PDCLIB_glue.h"

int _PDCLIB_prepwrite( struct _PDCLIB_file_t * stream )
{
    if ( ( stream->status & _PDCLIB_FREAD ) &&
         ( stream->status & _PDCLIB_FRW ) &&
         ! ( stream->status & _PDCLIB_EOFFLAG ) &&
         stream->bufidx == stream->bufend &&
         stream->ungetidx == 0 )
    {
        /* an update stream with nothing left to read: writes from here */
        stream->status &= ~ _PDCLIB_FREAD;
        stream->bufidx = 0;
        stream->bufend = 0;
    }

    if ( ( stream->bufidx < stream->bufend ) || ( stream->ungetidx > 0 ) ||
         ( stream->status & ( _PDCLIB_FREAD | _PDCLIB_ERRORFLAG | _PDCLIB_WIDESTREAM | _PDCLIB_EOFFLAG ) ) ||
         ! ( stream->status & ( _PDCLIB_FWRITE | _PDCLIB_FAPPEND | _PDCLIB_FRW ) ) )
    {
        /* Function called on illegal (e.g. input) stream. */
        *_PDCLIB_errno_func() = _PDCLIB_EBADF;
        stream->status |= _PDCLIB_ERRORFLAG;
        return EOF;
    }

    stream->status |= _PDCLIB_FWRITE | _PDCLIB_BYTESTREAM;
    return 0;
}

#endif
