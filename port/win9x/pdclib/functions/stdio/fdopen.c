/* fdopen( int, const char * )

   This file is part of the Public Domain C Library (PDCLib) glue of the
   Windows 98 SE / Me build of the game.

   A stream on an MSVC descriptor (port/win9x/src/w9x_crt_io.c), for the
   wide _wfdopen (port/linux/src/msvc_wide.c). The stream takes the
   descriptor's file handle over: the descriptor is free again at once, and
   fclose closes the handle (POSIX: closing the stream closes the
   descriptor). Built as fopen() builds its streams.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef REGTEST

#include "pdclib/_PDCLIB_glue.h"

#include <errno.h>
#include <windows.h>

#ifndef __STDC_NO_THREADS__
#include <threads.h>
#endif

/* the descriptor's handle, the descriptor freed (w9x_crt_io.c); NULL when
   it is not open */
void * w9x_take_descriptor_handle( int handle );

struct _PDCLIB_file_t * fdopen( int handle, const char * mode )
{
    struct _PDCLIB_file_t * rc;
    unsigned int status;
    void * file;

    if ( mode == NULL || ( status = _PDCLIB_filemode( mode ) ) == 0 )
    {
        *_PDCLIB_errno_func() = EINVAL;
        return NULL;
    }
    if ( ( rc = calloc( 1, sizeof( struct _PDCLIB_file_t ) + _PDCLIB_UNGETCBUFSIZE + 1 + BUFSIZ ) ) == NULL )
    {
        *_PDCLIB_errno_func() = ENOMEM;
        return NULL;
    }
#ifndef __STDC_NO_THREADS__
    if ( mtx_init( &rc->mtx, mtx_plain | mtx_recursive ) != thrd_success )
    {
        free( rc );
        return NULL;
    }
#endif
    if ( ( file = w9x_take_descriptor_handle( handle ) ) == NULL )
    {
#ifndef __STDC_NO_THREADS__
        mtx_destroy( &rc->mtx );
#endif
        free( rc );
        *_PDCLIB_errno_func() = EBADF;
        return NULL;
    }
    rc->status = status | _IOFBF;
    rc->handle = file;
    {
        /* the stream starts where the descriptor was */
        LONG high = 0;
        DWORD low;

        SetLastError( NO_ERROR );
        low = SetFilePointer( file, 0, &high, FILE_CURRENT );

        if ( low != INVALID_SET_FILE_POINTER || GetLastError() == NO_ERROR )
        {
            rc->pos.offset = (_PDCLIB_int64_t)( ( (unsigned long long)(DWORD)high << 32 ) | low );
        }
    }
    rc->ungetbuf = (unsigned char *)rc + sizeof( struct _PDCLIB_file_t );
    /* (no name: the empty one) */
    rc->filename = (char *)rc->ungetbuf + _PDCLIB_UNGETCBUFSIZE;
    rc->buffer   = rc->filename + 1;
    rc->bufsize = BUFSIZ;
    rc->bufidx = 0;
    rc->ungetidx = 0;
    _PDCLIB_setstream( rc );
    return rc;
}

#endif
