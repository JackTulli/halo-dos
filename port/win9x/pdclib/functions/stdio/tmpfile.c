/* tmpfile( void )

   This file is part of the Public Domain C Library (PDCLib).
   Permission is granted to use, modify, and / or redistribute at will.
*/

/* The 9x build's (nxdk's returns NULL): a new file in the system's
   temporary folder (GetTempPathA, GetTempFileNameA), opened "wb+" and
   removed when the stream is closed (_PDCLIB_DELONCLOSE, which fclose
   honours; 9x has no FILE_FLAG_DELETE_ON_CLOSE to rely on). */

#include <stdio.h>

#ifndef REGTEST

#include "pdclib/_PDCLIB_glue.h"

#include <stdlib.h>
#include <string.h>

#include <windows.h>

#ifndef __STDC_NO_THREADS__
#include <threads.h>
#endif

struct _PDCLIB_file_t * tmpfile( void )
{
    char folder[ MAX_PATH ];
    char filename[ MAX_PATH ];
    struct _PDCLIB_file_t * rc;
    _PDCLIB_fd_t fd;
    DWORD length = GetTempPathA( sizeof( folder ), folder );

    if ( length == 0 || length >= sizeof( folder ) )
    {
        return NULL;
    }
    /* creates the file (an empty one, of a name no other file has) */
    if ( ! GetTempFileNameA( folder, "hal", 0, filename ) )
    {
        return NULL;
    }
    fd = _PDCLIB_open( filename, _PDCLIB_filemode( "wb+" ) );
    if ( fd == _PDCLIB_NOHANDLE )
    {
        DeleteFileA( filename );
        return NULL;
    }
    /* See fopen(). */
    if ( ( rc = calloc( 1, sizeof( struct _PDCLIB_file_t ) + _PDCLIB_UNGETCBUFSIZE + strlen( filename ) + 1 + BUFSIZ ) ) == NULL )
    {
        _PDCLIB_close( fd );
        DeleteFileA( filename );
        return NULL;
    }
    rc->status = _PDCLIB_filemode( "wb+" ) | _IOFBF | _PDCLIB_DELONCLOSE;
    rc->handle = fd;
    rc->ungetbuf = (unsigned char *)rc + sizeof( struct _PDCLIB_file_t );
    rc->filename = (char *)rc->ungetbuf + _PDCLIB_UNGETCBUFSIZE;
    rc->buffer   = rc->filename + strlen( filename ) + 1;
    strcpy( rc->filename, filename );
    rc->bufsize = BUFSIZ;
    rc->bufidx = 0;
    rc->ungetidx = 0;
#ifndef __STDC_NO_THREADS__
    if ( mtx_init( &rc->mtx, mtx_plain | mtx_recursive ) != thrd_success )
    {
        _PDCLIB_close( fd );
        DeleteFileA( filename );
        free( rc );
        return NULL;
    }
#endif
    /* Adding to list of open files */
    _PDCLIB_setstream( rc );
    return rc;
}

#endif
