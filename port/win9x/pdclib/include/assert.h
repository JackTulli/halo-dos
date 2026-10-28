/* Diagnostics <assert.h>

   This file is part of the Public Domain C Library (PDCLib).
   Permission is granted to use, modify, and / or redistribute at will.

   The 9x build's: a failed assertion goes to w9x_assert
   (port/win9x/src/w9x_crt.c), which reports it and ends the program; the
   third-party units (kcp, tomlc17) are the only ones that use it.
*/

#include "pdclib/_PDCLIB_aux.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef _PDCLIB_ASSERT_H
#define _PDCLIB_ASSERT_H
__attribute__(( noreturn )) void w9x_assert( char const * expression, char const * file_name, char const * function_name, unsigned long line );
#endif

/* If NDEBUG is set, assert() is a null operation. */
#undef assert

#ifdef NDEBUG
    #define assert(ignore) ((void)0)
#else
    #define assert(expression) \
        ((void) ((expression) ? 0 :  \
            (w9x_assert(_PDCLIB_symbol2string(expression), \
                        __FILE__, \
                        __func__, \
                        __LINE__), 0)))
#endif

#if !defined(__cplusplus) && !defined(static_assert)
#define static_assert _Static_assert
#endif

#ifdef __cplusplus
}
#endif
