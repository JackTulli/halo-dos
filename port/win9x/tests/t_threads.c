/*
T_THREADS.C

Threads over the 9x build's runtime: implicit TLS (__thread, through the
TIB's TLS array at FS:[0x2C], port/win9x/src/w9x_tls.s) on the main thread
and two more; errno for each thread; PDCLib's locks over CRITICAL_SECTION
(port/win9x/pdclib/functions/threads/mtx.c): exclusion under contention,
recursion, trying and timing a lock; two threads writing lines to one
stream; and the floating-point state a new thread starts in.
*/

#include <windows.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <time.h>

#include "w9x_test.h"
#include "w9x_runtime.h"

static __thread int tls_counter = 7;
static __thread char tls_name[16] = "initial";

struct tls_result
{
	int start_value;
	char start_name[16];
	int end_value;
	int error;
	unsigned int x87;
	unsigned int mxcsr;
};

static DWORD WINAPI tls_thread(void *parameter)
{
	struct tls_result *result = parameter;
	unsigned short word;
	int i;

	__asm__ __volatile__("fnstcw %0" : "=m"(word));
	__asm__ __volatile__("stmxcsr %0" : "=m"(result->mxcsr));
	result->x87 = word;
	result->start_value = tls_counter;
	strcpy(result->start_name, tls_name);
	for (i = 0; i < 100000; i++)
		tls_counter++;
	strcpy(tls_name, "thread");
	errno = (int)(INT_PTR)GetCurrentThreadId() & 0x7F;
	Sleep(20);
	result->end_value = tls_counter;
	result->error = errno;
	return 0;
}

static mtx_t lock;
static volatile long shared_counter;
static volatile long inside;
static volatile long overlaps;

static DWORD WINAPI contention_thread(void *parameter)
{
	int i;

	(void)parameter;
	for (i = 0; i < 20000; i++)
	{
		mtx_lock(&lock);
		if (InterlockedIncrement(&inside) != 1)
			overlaps++;
		shared_counter++;
		if ((i & 255) == 0)
			Sleep(0);
		InterlockedDecrement(&inside);
		mtx_unlock(&lock);
	}
	return 0;
}

static volatile long holder_ready;
static volatile long holder_release;

static DWORD WINAPI holder_thread(void *parameter)
{
	(void)parameter;
	mtx_lock(&lock);
	holder_ready = 1;
	while (!holder_release)
		Sleep(1);
	mtx_unlock(&lock);
	return 0;
}

static FILE *shared_stream;

static DWORD WINAPI writer_thread(void *parameter)
{
	int i, who = (int)(INT_PTR)parameter;

	for (i = 0; i < 2000; i++)
		fprintf(shared_stream, "writer %d line %04d ................................\n", who, i);
	return 0;
}

static HANDLE start(LPTHREAD_START_ROUTINE routine, void *parameter)
{
	DWORD id;

	return CreateThread(NULL, 0, routine, parameter, 0, &id);
}

int main(void)
{
	struct tls_result results[2];
	HANDLE threads[4];
	int i;

	/* ---------- TLS and errno on three threads */
	memset(results, 0, sizeof(results));
	tls_counter = 1000;
	strcpy(tls_name, "main");
	errno = 99;
	for (i = 0; i < 2; i++)
		threads[i] = start(tls_thread, &results[i]);
	WaitForMultipleObjects(2, threads, TRUE, INFINITE);
	for (i = 0; i < 2; i++)
	{
		CloseHandle(threads[i]);
		printf("# thread %d: TLS started %d '%s', ended %d, errno %d; x87 %04x mxcsr %04x\n", i,
			results[i].start_value, results[i].start_name, results[i].end_value, results[i].error, results[i].x87,
			results[i].mxcsr);
		CHECK(results[i].start_value == 7);
		CHECK(!strcmp(results[i].start_name, "initial"));
		CHECK(results[i].end_value == 100007);
	}
	CHECK(results[0].error != 99 && results[1].error != 99);
	printf("# main: TLS %d '%s', errno %d\n", tls_counter, tls_name, errno);
	CHECK(tls_counter == 1000 && !strcmp(tls_name, "main"));
	CHECK(errno == 99);

	/* ---------- PDCLib's locks */
	CHECK(mtx_init(&lock, mtx_plain | mtx_recursive) == thrd_success);
	for (i = 0; i < 4; i++)
		threads[i] = start(contention_thread, NULL);
	WaitForMultipleObjects(4, threads, TRUE, INFINITE);
	for (i = 0; i < 4; i++)
		CloseHandle(threads[i]);
	printf("# contention: counter %ld, overlaps %ld\n", shared_counter, overlaps);
	CHECK(shared_counter == 80000);
	CHECK(overlaps == 0);

	/* recursion, and trying */
	CHECK(mtx_lock(&lock) == thrd_success);
	CHECK(mtx_lock(&lock) == thrd_success);
	CHECK(mtx_trylock(&lock) == thrd_success);
	CHECK(mtx_unlock(&lock) == thrd_success);
	CHECK(mtx_unlock(&lock) == thrd_success);
	CHECK(mtx_unlock(&lock) == thrd_success);
	CHECK(mtx_unlock(&lock) == thrd_error); /* (not held) */
	CHECK(mtx_trylock(&lock) == thrd_success);
	CHECK(mtx_unlock(&lock) == thrd_success);

	threads[0] = start(holder_thread, NULL);
	while (!holder_ready)
		Sleep(1);
	CHECK(mtx_trylock(&lock) == thrd_busy);
	{
		struct timespec until;
		DWORD before = GetTickCount(), waited;

		timespec_get(&until, TIME_UTC);
		until.tv_nsec += 50 * 1000000L;
		if (until.tv_nsec >= 1000000000L)
		{
			until.tv_sec++;
			until.tv_nsec -= 1000000000L;
		}
		CHECK(mtx_timedlock(&lock, &until) == thrd_timedout);
		waited = GetTickCount() - before;
		printf("# timed lock gave up after %lu ms (asked 50)\n", (unsigned long)waited);
		CHECK(waited >= 40 && waited < 1000);
	}
	holder_release = 1;
	WaitForSingleObject(threads[0], INFINITE);
	CloseHandle(threads[0]);
	CHECK(mtx_trylock(&lock) == thrd_success);
	CHECK(mtx_unlock(&lock) == thrd_success);
	mtx_destroy(&lock);

	/* ---------- one stream, two writers: whole lines (the stream's lock) */
	shared_stream = fopen("build/win9x/tests/t_threads.txt", "wb");
	CHECK(shared_stream != NULL);
	if (shared_stream)
	{
		char line[128];
		int lines = 0, broken = 0;

		threads[0] = start(writer_thread, (void *)1);
		threads[1] = start(writer_thread, (void *)2);
		WaitForMultipleObjects(2, threads, TRUE, INFINITE);
		CloseHandle(threads[0]);
		CloseHandle(threads[1]);
		fclose(shared_stream);
		shared_stream = fopen("build/win9x/tests/t_threads.txt", "rb");
		CHECK(shared_stream != NULL);
		while (shared_stream && fgets(line, sizeof(line), shared_stream))
		{
			int who, number;
			char expected[128];

			lines++;
			if (sscanf(line, "writer %d line %d", &who, &number) != 2)
			{
				broken++;
				continue;
			}
			snprintf(expected, sizeof(expected), "writer %d line %04d ................................\n", who, number);
			if (strcmp(line, expected))
				broken++;
		}
		if (shared_stream)
			fclose(shared_stream);
		printf("# shared stream: %d lines, %d broken\n", lines, broken);
		CHECK(lines == 4000 && broken == 0);
		remove("build/win9x/tests/t_threads.txt");
	}
	return test_finish("t_threads");
}
