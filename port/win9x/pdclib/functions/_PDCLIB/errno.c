/* _PDCLIB_errno

   This file is part of the Public Domain C Library (PDCLib).
   Permission is granted to use, modify, and / or redistribute at will.
*/

/* One errno for each thread (PDCLib's generic one is shared by all): the
   game's thread and internet play's both make socket and file calls and
   read errno straight after them, as port/xbox/src/nx_stdlib.c explains
   for the console. The variable is implicit TLS (__thread), which the 9x
   loader gives every thread of the executable (port/win9x/src/w9x_tls.s). */

#ifndef REGTEST

#include "pdclib/_PDCLIB_int.h"

int * _PDCLIB_errno_func( void )
{
    static __thread int error;

    return &error;
}

#endif
