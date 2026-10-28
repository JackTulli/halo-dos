/* mtx_init, mtx_lock, mtx_trylock, mtx_timedlock, mtx_unlock, mtx_destroy

   This file is part of the Public Domain C Library (PDCLib) glue of the
   Windows 98 SE / Me build of the game.

   PDCLib's own locks (its list of streams and each stream's) over a
   CRITICAL_SECTION, which every Win32 since Windows 95 has. A critical
   section is always recursive, which mtx_recursive needs and mtx_plain
   does not mind.

   Trying a lock without waiting is TryEnterCriticalSection on NT, which
   9x exports but does not implement; here a count of the threads that hold
   or wait for the lock tells whether it is free, and the owner whether the
   caller already holds it. PDCLib itself never tries or times a lock.
*/

#include <threads.h>
#include <time.h>

#ifndef REGTEST

#include <windows.h>

struct w9x_mutex
{
    CRITICAL_SECTION section;
    /* the threads that hold the lock or are on their way to it */
    volatile LONG users;
    /* the thread that holds it, and how many times over */
    volatile DWORD owner;
    unsigned long depth;
};

int mtx_init( mtx_t * mtx, int type )
{
    struct w9x_mutex * mutex = LocalAlloc( LMEM_FIXED | LMEM_ZEROINIT, sizeof( struct w9x_mutex ) );

    (void)type;
    if ( mutex == NULL )
    {
        mtx->handle = NULL;
        return thrd_nomem;
    }
    InitializeCriticalSection( &mutex->section );
    mtx->handle = mutex;
    return thrd_success;
}

void mtx_destroy( mtx_t * mtx )
{
    struct w9x_mutex * mutex = mtx->handle;

    if ( mutex != NULL )
    {
        DeleteCriticalSection( &mutex->section );
        LocalFree( mutex );
        mtx->handle = NULL;
    }
}

static void took( struct w9x_mutex * mutex )
{
    mutex->owner = GetCurrentThreadId();
    mutex->depth++;
}

int mtx_lock( mtx_t * mtx )
{
    struct w9x_mutex * mutex = mtx->handle;

    if ( mutex == NULL )
    {
        return thrd_error;
    }
    InterlockedIncrement( &mutex->users );
    EnterCriticalSection( &mutex->section );
    took( mutex );
    return thrd_success;
}

int mtx_trylock( mtx_t * mtx )
{
    struct w9x_mutex * mutex = mtx->handle;

    if ( mutex == NULL )
    {
        return thrd_error;
    }
    if ( mutex->owner == GetCurrentThreadId() )
    {
        /* held by this thread: taken again at once */
        return mtx_lock( mtx );
    }
    if ( InterlockedCompareExchange( &mutex->users, 1, 0 ) != 0 )
    {
        return thrd_busy;
    }
    /* nobody holds it or waits for it: this does not wait either */
    EnterCriticalSection( &mutex->section );
    took( mutex );
    return thrd_success;
}

int mtx_timedlock( mtx_t * _PDCLIB_restrict mtx, const struct timespec * _PDCLIB_restrict ts )
{
    for ( ;; )
    {
        struct timespec now;
        int result = mtx_trylock( mtx );

        if ( result != thrd_busy )
        {
            return result;
        }
        timespec_get( &now, TIME_UTC );
        if ( now.tv_sec > ts->tv_sec || ( now.tv_sec == ts->tv_sec && now.tv_nsec >= ts->tv_nsec ) )
        {
            return thrd_timedout;
        }
        /* (any thread may run, the holder too, whatever its priority: 9x's
           Sleep( 0 ) only yields to threads of the caller's own) */
        Sleep( 1 );
    }
}

int mtx_unlock( mtx_t * mtx )
{
    struct w9x_mutex * mutex = mtx->handle;

    if ( mutex == NULL || mutex->owner != GetCurrentThreadId() )
    {
        return thrd_error;
    }
    if ( --mutex->depth == 0 )
    {
        mutex->owner = 0;
    }
    LeaveCriticalSection( &mutex->section );
    InterlockedDecrement( &mutex->users );
    return thrd_success;
}

#endif
