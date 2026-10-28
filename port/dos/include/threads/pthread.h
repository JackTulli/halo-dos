/*
PTHREAD.H

POSIX threads as the DOS and Windows 95 builds have them: one thread. The
game runs in one, and the native ports' code these builds compile again
(xnet.c, internet play) locks against threads of its own that these builds
do not start: their work is polled from the game's loop instead. So a lock
is free and always taken, and a thread cannot be made (pthread_create fails
with EAGAIN, which callers already handle as no thread).
*/

#ifndef __HALO_DOS_PTHREAD_H
#define __HALO_DOS_PTHREAD_H

#include <stddef.h>

typedef unsigned long pthread_t;
typedef struct { int unused; } pthread_attr_t;
typedef struct { int unused; } pthread_mutex_t;
typedef struct { int unused; } pthread_mutexattr_t;
typedef struct { int unused; } pthread_cond_t;
typedef struct { int unused; } pthread_condattr_t;
typedef struct { int done; } pthread_once_t;

#define PTHREAD_MUTEX_INITIALIZER { 0 }
#define PTHREAD_COND_INITIALIZER { 0 }
#define PTHREAD_ONCE_INIT { 0 }
#define PTHREAD_CREATE_JOINABLE 0
#define PTHREAD_CREATE_DETACHED 1
#define PTHREAD_MUTEX_NORMAL 0
#define PTHREAD_MUTEX_RECURSIVE 1
#define PTHREAD_MUTEX_DEFAULT PTHREAD_MUTEX_NORMAL

struct timespec;

static __inline__ int pthread_create(pthread_t *thread, const pthread_attr_t *attributes, void *(*start)(void *),
	void *argument)
{
	(void)thread;
	(void)attributes;
	(void)start;
	(void)argument;
	return 11; /* EAGAIN */
}
static __inline__ int pthread_detach(pthread_t thread) { (void)thread; return 0; }
static __inline__ pthread_t pthread_self(void) { return 1; }
static __inline__ int pthread_equal(pthread_t a, pthread_t b) { return a == b; }
static __inline__ int pthread_once(pthread_once_t *once, void (*routine)(void))
{
	if (!once->done)
	{
		once->done = 1;
		routine();
	}
	return 0;
}
static __inline__ int pthread_attr_init(pthread_attr_t *attributes) { (void)attributes; return 0; }
static __inline__ int pthread_attr_destroy(pthread_attr_t *attributes) { (void)attributes; return 0; }
static __inline__ int pthread_attr_setdetachstate(pthread_attr_t *attributes, int state)
{
	(void)attributes;
	(void)state;
	return 0;
}
static __inline__ int pthread_attr_setstacksize(pthread_attr_t *attributes, size_t size)
{
	(void)attributes;
	(void)size;
	return 0;
}
static __inline__ int pthread_mutexattr_init(pthread_mutexattr_t *attributes) { (void)attributes; return 0; }
static __inline__ int pthread_mutexattr_destroy(pthread_mutexattr_t *attributes) { (void)attributes; return 0; }
static __inline__ int pthread_mutexattr_settype(pthread_mutexattr_t *attributes, int type)
{
	(void)attributes;
	(void)type;
	return 0;
}
static __inline__ int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attributes)
{
	(void)mutex;
	(void)attributes;
	return 0;
}
static __inline__ int pthread_mutex_destroy(pthread_mutex_t *mutex) { (void)mutex; return 0; }
static __inline__ int pthread_mutex_lock(pthread_mutex_t *mutex) { (void)mutex; return 0; }
static __inline__ int pthread_mutex_trylock(pthread_mutex_t *mutex) { (void)mutex; return 0; }
static __inline__ int pthread_mutex_unlock(pthread_mutex_t *mutex) { (void)mutex; return 0; }
static __inline__ int pthread_cond_init(pthread_cond_t *condition, const pthread_condattr_t *attributes)
{
	(void)condition;
	(void)attributes;
	return 0;
}
static __inline__ int pthread_cond_destroy(pthread_cond_t *condition) { (void)condition; return 0; }
static __inline__ int pthread_cond_wait(pthread_cond_t *condition, pthread_mutex_t *mutex)
{
	(void)condition;
	(void)mutex;
	return 0;
}
static __inline__ int pthread_cond_timedwait(pthread_cond_t *condition, pthread_mutex_t *mutex,
	const struct timespec *deadline)
{
	(void)condition;
	(void)mutex;
	(void)deadline;
	return 110; /* ETIMEDOUT */
}
static __inline__ int pthread_cond_signal(pthread_cond_t *condition) { (void)condition; return 0; }
static __inline__ int pthread_cond_broadcast(pthread_cond_t *condition) { (void)condition; return 0; }

#endif /* __HALO_DOS_PTHREAD_H */
