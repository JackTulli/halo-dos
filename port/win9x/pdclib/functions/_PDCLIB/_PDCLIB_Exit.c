/* _PDCLIB_Exit( int )

   This file is part of the Public Domain C Library (PDCLib).
   Permission is granted to use, modify, and / or redistribute at will.
*/

/* The 9x build's: the process ends with the status (nxdk's restarts the
   console). */

#include <stdlib.h>

#ifndef REGTEST

#include "pdclib/_PDCLIB_glue.h"

#include <windows.h>

void _PDCLIB_Exit( int status )
{
    ExitProcess( (UINT)status );
}

#endif
