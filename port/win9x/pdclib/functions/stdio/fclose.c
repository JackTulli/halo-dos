/* fclose( FILE * )

   This file is part of the Public Domain C Library (PDCLib).
   Permission is granted to use, modify, and / or redistribute at will.
*/

/* PDCLib's generic fclose, with the stream's lock destroyed before the
   stream is freed (the generic one leaves it: each lock of the 9x build
   holds a CRITICAL_SECTION, a kernel object on 9x) */

#include <stdio.h>
#include <stdlib.h>

#ifndef REGTEST

#include "pdclib/_PDCLIB_glue.h"

#ifndef __STDC_NO_THREADS__
#include <threads.h>
extern mtx_t _PDCLIB_filelist_mtx;
#endif

extern struct _PDCLIB_file_t * _PDCLIB_filelist;

int fclose( struct _PDCLIB_file_t * stream )
{
    _PDCLIB_LOCK( stream->mtx );

    /* Remove stream from list */
    if ( _PDCLIB_getstream( stream ) )
    {
        _PDCLIB_UNLOCK( stream->mtx );
        return EOF;
    }


    /* Flush buffer */
    if ( stream->status & _PDCLIB_FWRITE )
    {
        if ( _PDCLIB_flushbuffer( stream ) == EOF )
        {
            /* Flush failed, errno already set */
            _PDCLIB_UNLOCK( stream->mtx );
            return EOF;
        }
    }

    /* Close handle */
    _PDCLIB_close( stream->handle );

    /* Delete tmpfile() */
    if ( stream->status & _PDCLIB_DELONCLOSE )
    {
        remove( stream->filename );
    }

    /* Free user buffer (setvbuf allocated) */
    if ( stream->status & _PDCLIB_FREEBUFFER )
    {
        free( stream->buffer );
    }

    /* Free filename (freopen allocated) */
    if ( stream->status & _PDCLIB_FREENAME )
    {
        free( stream->filename );
    }

    _PDCLIB_UNLOCK( stream->mtx );

    /* Free stream (and its lock, which holds a CRITICAL_SECTION here) */
    if ( ! ( stream->status & _PDCLIB_STATIC ) )
    {
#ifndef __STDC_NO_THREADS__
        mtx_destroy( &stream->mtx );
#endif
        free( stream );
    }

    return 0;
}

#endif
