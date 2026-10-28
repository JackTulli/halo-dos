/* vsnprintf( char *, size_t, const char *, va_list )

   This file is part of the Public Domain C Library (PDCLib).
   Permission is granted to use, modify, and / or redistribute at will.
*/

/* PDCLib's generic vsnprintf, with the end of a truncated result fixed:
   the generic one ends the string only when the output fits, or after an
   integer conversion that overflows, so a result cut short in a %s, a %c
   or the format's own text was left without its terminating zero (the
   UCRT's and C99's snprintf always end it within n). */

#include <stdio.h>
#include <stdarg.h>

#ifndef REGTEST

int vsnprintf( char * _PDCLIB_restrict s, size_t n, const char * _PDCLIB_restrict format, _PDCLIB_va_list arg )
{
    /* TODO: This function should interpret format as multibyte characters.  */
    struct _PDCLIB_status_t status;
    status.base = 0;
    status.flags = 0;
    status.n = n;
    status.i = 0;
    status.current = 0;
    status.s = s;
    status.width = 0;
    status.prec = EOF;
    status.stream = NULL;
    va_copy( status.arg, arg );

    while ( *format != '\0' )
    {
        const char * rc;

        if ( ( *format != '%' ) || ( ( rc = _PDCLIB_print( format, &status ) ) == format ) )
        {
            /* No conversion specifier, print verbatim */
            if ( status.i < n )
            {
                s[ status.i ] = *format;
            }

            status.i++;
            format++;
        }
        else
        {
            /* Continue parsing after conversion specifier */
            format = rc;
        }
    }

    if ( status.i < n )
    {
        s[ status.i ] = '\0';
    }
    else if ( n > 0 )
    {
        /* truncated: the last place holds the end */
        s[ n - 1 ] = '\0';
    }

    va_end( status.arg );
    return status.i;
}

#endif
