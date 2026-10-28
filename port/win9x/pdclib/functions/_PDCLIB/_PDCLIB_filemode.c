/* _PDCLIB_filemode( const char * )

   This file is part of the Public Domain C Library (PDCLib).
   Permission is granted to use, modify, and / or redistribute at will.
*/

/* PDCLib's generic parser of fopen's mode, taking the Microsoft runtime's
   spellings as well: 't' (text, which the game asks for in "wt" and "a+t",
   source/main/main.c; the generic parser refused the mode, fopen returned
   NULL and the caller's fclose( NULL ) faulted), and the hints 'c', 'n',
   'N', 'S', 'R' and 'T', which change nothing here. Streams are binary
   whatever is asked: the Windows build makes MSVC's binary by default too
   (_set_fmode( _O_BINARY ), port/windows/src/win32_posix.c), and only an
   explicit 't' would have had line ends translated there. */

#include <stddef.h>

#ifndef REGTEST

unsigned int _PDCLIB_filemode( const char * const mode )
{
    unsigned rc = 0;
    int binary_or_text = 0;
    size_t i;

    switch ( mode[0] )
    {
        case 'r':
            rc |= _PDCLIB_FREAD;
            break;

        case 'w':
            rc |= _PDCLIB_FWRITE;
            break;

        case 'a':
            rc |= _PDCLIB_FAPPEND | _PDCLIB_FWRITE;
            break;

        default:
            /* Other than read, write, or append - invalid */
            return 0;
    }

    for ( i = 1; mode[i] != '\0'; ++i )
    {
        switch ( mode[i] )
        {
            case '+':
                if ( rc & _PDCLIB_FRW ) return 0; /* Duplicates are invalid */
                rc |= _PDCLIB_FRW;
                break;

            case 'b':
            case 't':
                if ( binary_or_text ) return 0; /* Both, or twice: invalid */
                binary_or_text = 1;
                if ( mode[i] == 'b' ) rc |= _PDCLIB_FBIN;
                break;

            case 'c': /* commit on fflush */
            case 'n': /* no commit */
            case 'N': /* not inherited */
            case 'S': /* sequential access */
            case 'R': /* random access */
            case 'T': /* short-lived */
                break;

            default:
                /* Anything else (MSVC's "ccs=", 'D', 'x') - invalid. */
                return 0;
        }
    }

    return rc;
}

#endif
