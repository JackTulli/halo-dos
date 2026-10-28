/*
T_START.C

The start-up code (port/win9x/src/w9x_start.c): constructors in priority
order (101 first, as the platform layer's memory window needs), the
floating-point state the program starts in and what _control87 makes of it
(the x87 and MXCSR together: never DAZ), the command line. Compared with
the UCRT's start-up.
*/

#include <float.h>
#include <stdio.h>
#include <string.h>

#include "w9x_test.h"

static int order[8];
static int order_count;

static void record(int value)
{
	if (order_count < 8)
		order[order_count++] = value;
}

/* (declared in the order that would be wrong if run as written) */
__attribute__((constructor)) static void constructor_default(void) { record(65535); }
__attribute__((constructor(200))) static void constructor_200(void) { record(200); }
__attribute__((constructor(101))) static void constructor_101(void) { record(101); }

static __thread int thread_value = 42;

static unsigned int x87_control(void)
{
	unsigned short word;

	__asm__ __volatile__("fnstcw %0" : "=m"(word));
	return word;
}

static unsigned int mxcsr(void)
{
	unsigned int value;

	__asm__ __volatile__("stmxcsr %0" : "=m"(value));
	return value;
}

static void show_state(const char *when)
{
	printf("%s: x87 %04x mxcsr %04x _control87 %08x\n", when, x87_control(), mxcsr(), _control87(0, 0));
}

static volatile double zero_double = 0.0;
static volatile float zero_float = 0.0f;

int main(int argc, char **argv)
{
	const char *name = argv[0];
	const char *cursor;
	int i;

	printf("constructors:");
	for (i = 0; i < order_count; i++)
		printf(" %d", order[i]);
	printf("\n");
	CHECK(order_count == 3 && order[0] == 101 && order[1] == 200 && order[2] == 65535);

	show_state("start");
	CHECK(x87_control() == 0x027F);
	CHECK((mxcsr() & 0xFFC0) == 0x1F80);

	/* the game's real_math_reset_precision (source/math/real_math.c) */
	_control87(CW_DEFAULT, 0xFFFFF);
	show_state("_control87(CW_DEFAULT, 0xFFFFF)");
	CHECK(x87_control() == 0x027F);
	CHECK((mxcsr() & 0xFFC0) == 0x1F80);

	_controlfp(_RC_CHOP, _MCW_RC);
	show_state("_controlfp(_RC_CHOP, _MCW_RC)");
	CHECK((mxcsr() & 0x6000) == 0x6000);
	_controlfp(_PC_24, _MCW_PC);
	show_state("_controlfp(_PC_24, _MCW_PC)");
	_control87(_EM_INEXACT, _MCW_EM);
	show_state("_control87(_EM_INEXACT, _MCW_EM)");
	CHECK((mxcsr() & 0x40) == 0);
	_control87(CW_DEFAULT, 0xFFFFF);
	show_state("_control87(CW_DEFAULT, 0xFFFFF) again");
	CHECK(x87_control() == 0x027F && (mxcsr() & 0xFFC0) == 0x1F80);

	/* the status words, x87 (double) and SSE (float) together */
	_clearfp();
	printf("status after _clearfp: %08x\n", _statusfp());
	zero_double = 1.0 / zero_double;
	printf("status after a double division by zero: %08x\n", _statusfp() & _SW_ZERODIVIDE);
	_clearfp();
	zero_float = 1.0f / zero_float;
	printf("status after a float division by zero: %08x\n", _statusfp() & _SW_ZERODIVIDE);
	CHECK((_statusfp() & _SW_ZERODIVIDE) != 0);
	_clearfp();
	printf("status after _clearfp again: %08x\n", _statusfp());

	/* the command line (the program's name: the folders differ) */
	for (cursor = name; *cursor; cursor++)
	{
		if (*cursor == '\\' || *cursor == '/')
			name = cursor + 1;
	}
	printf("argc %d, program %s, argv[argc] %s\n", argc, name, argv[argc] ? "set" : "NULL");
	CHECK(argc >= 1 && argv[argc] == NULL);

	printf("thread variable %d\n", thread_value);
	CHECK(thread_value == 42);
	return test_finish("t_start");
}
