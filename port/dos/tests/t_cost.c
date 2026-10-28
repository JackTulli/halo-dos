/*
T_COST.C

How many of DOSBox-X's cycles (the time stamp counter, with "rdtsc rate"
0 and fixed cycles) a loop of integer instructions and one of x87
instructions take, the instruction a loop pass the same: where DOSBox
charges the floating point more, the renderer's float work is what to cut.
Prints the cycles per pass of each loop.
*/

#include <stdio.h>

static unsigned long long ticks(void)
{
	unsigned long low, high;

	__asm__ __volatile__("rdtsc" : "=a"(low), "=d"(high));
	return ((unsigned long long)high << 32) | low;
}

#define PASSES 200000

int main(void)
{
	unsigned long long start, integer, x87, memory;
	unsigned long a = 1, b = 3;
	float x = 1.0f, y = 1.0001f;
	static unsigned long cells[64];

	/* 8 integer adds a pass, and the loop's own 2 */
	start = ticks();
	for (long i = 0; i < PASSES; i++)
	{
		__asm__ __volatile__(
			"addl %1, %0\n\taddl %1, %0\n\taddl %1, %0\n\taddl %1, %0\n\t"
			"addl %1, %0\n\taddl %1, %0\n\taddl %1, %0\n\taddl %1, %0"
			: "+r"(a) : "r"(b));
	}
	integer = ticks() - start;
	/* 8 x87 multiplies a pass */
	start = ticks();
	for (long i = 0; i < PASSES; i++)
	{
		__asm__ __volatile__(
			"fmul %%st(1), %%st\n\tfmul %%st(1), %%st\n\tfmul %%st(1), %%st\n\tfmul %%st(1), %%st\n\t"
			"fmul %%st(1), %%st\n\tfmul %%st(1), %%st\n\tfmul %%st(1), %%st\n\tfmul %%st(1), %%st"
			: "+t"(x) : "u"(y));
	}
	x87 = ticks() - start;
	/* 8 memory adds a pass */
	start = ticks();
	for (long i = 0; i < PASSES; i++)
	{
		__asm__ __volatile__(
			"addl %1, (%0)\n\taddl %1, 4(%0)\n\taddl %1, 8(%0)\n\taddl %1, 12(%0)\n\t"
			"addl %1, 16(%0)\n\taddl %1, 20(%0)\n\taddl %1, 24(%0)\n\taddl %1, 28(%0)"
			: : "r"(cells), "r"(b) : "memory");
	}
	memory = ticks() - start;
	printf("cycles a pass (8 instructions and the loop's): integer %.1f, x87 %.1f, memory %.1f (%lu %f)\n",
		(double)integer / PASSES, (double)x87 / PASSES, (double)memory / PASSES, a, (double)x);
	return 0;
}
