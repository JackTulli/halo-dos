/*
PTHREAD.H

The few POSIX thread calls the platform layer's shared units use (internet
play, port/linux/src/p2p.c), over the console's threads and critical
sections (port/xbox/src/xbox_posix.c).
*/

#ifndef __HALO_XBOX_PTHREAD_H
#define __HALO_XBOX_PTHREAD_H

typedef unsigned long pthread_t;

typedef struct
{
	/* an RTL_CRITICAL_SECTION, made on first use; state 0 = none, 1 =
	being made, 2 = made */
	void *section;
	long state;
} pthread_mutex_t;

typedef struct
{
	int unused;
} pthread_attr_t;

typedef struct
{
	int unused;
} pthread_mutexattr_t;

#define PTHREAD_MUTEX_INITIALIZER { 0, 0 }

int pthread_create(pthread_t *thread, const pthread_attr_t *attributes, void *(*start)(void *), void *argument);
int pthread_detach(pthread_t thread);

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attributes);
int pthread_mutex_destroy(pthread_mutex_t *mutex);
int pthread_mutex_lock(pthread_mutex_t *mutex);
int pthread_mutex_unlock(pthread_mutex_t *mutex);

#endif
