/*
T_FMOD.C

fmod and fmodf of the 9x build (musl's exact remainder, port/win9x/src/
w9x_crt.c), against the UCRT's, which is exact too: the game takes fmod in
its simulation, which must give every machine of a game the same bits.
Special values, random values of every exponent (gaps beyond the 64 bits
an x87 fprem step covers among them), subnormals, and the game's own
shapes (angles and periods). NaN results are compared as NaN only (their
sign and payload are the processor's).
*/

#include <math.h>
#include <stdio.h>

#include "w9x_test.h"

static unsigned long long double_bits(double value)
{
	union
	{
		double value;
		unsigned long long bits;
	} u;

	u.value = value;
	return u.bits;
}

static double from_bits(unsigned long long bits)
{
	union
	{
		double value;
		unsigned long long bits;
	} u;

	u.bits = bits;
	return u.value;
}

static unsigned int float_bits(float value)
{
	union
	{
		float value;
		unsigned int bits;
	} u;

	u.value = value;
	return u.bits;
}

static void show_double(double x, double y)
{
	double r = fmod(x, y);

	if (r != r)
		printf("d %08lx%08lx %08lx%08lx nan\n", (unsigned long)(double_bits(x) >> 32), (unsigned long)double_bits(x),
			(unsigned long)(double_bits(y) >> 32), (unsigned long)double_bits(y));
	else
		printf("d %08lx%08lx %08lx%08lx %08lx%08lx\n", (unsigned long)(double_bits(x) >> 32),
			(unsigned long)double_bits(x), (unsigned long)(double_bits(y) >> 32), (unsigned long)double_bits(y),
			(unsigned long)(double_bits(r) >> 32), (unsigned long)double_bits(r));
}

static void show_float(float x, float y)
{
	float r = fmodf(x, y);

	if (r != r)
		printf("f %08x %08x nan\n", float_bits(x), float_bits(y));
	else
		printf("f %08x %08x %08x\n", float_bits(x), float_bits(y), float_bits(r));
}

int main(void)
{
	static const double specials[] = {
		0.0, -0.0, 1.0, -1.0, 0.5, 2.0, 3.0, 10.0, 360.0, 6.283185307179586, 3.141592653589793, 1e-300, 1e300,
		4.9406564584124654e-324, 2.2250738585072014e-308, 1.7976931348623157e308, 1.0 / 3.0, 0.1, 30.0, 1e15,
		9007199254740993.0, 1e-10,
	};
	double infinity = 1.0 / from_bits(0);
	size_t i, j;
	int n;

	for (i = 0; i < sizeof(specials) / sizeof(specials[0]); i++)
	{
		for (j = 0; j < sizeof(specials) / sizeof(specials[0]); j++)
		{
			show_double(specials[i], specials[j]);
			show_double(-specials[i], specials[j]);
			show_float((float)specials[i], (float)specials[j]);
		}
		show_double(specials[i], infinity);
		show_double(infinity, specials[i]);
		show_double(specials[i], from_bits(0x7FF8000000000000ULL));
	}
	/* random bits: any exponents, so gaps of up to 2046 */
	for (n = 0; n < 50000; n++)
	{
		unsigned long long x = test_random(), y = test_random();

		if (((x >> 52) & 0x7FF) == 0x7FF || ((y >> 52) & 0x7FF) == 0x7FF)
			continue;
		show_double(from_bits(x), from_bits(y));
		show_float(from_bits(x & 0xC7FFFFFFFFFFFFFFULL) != 0 ? (float)from_bits(x & 0xC7FFFFFFFFFFFFFFULL) : 1.0f,
			(float)from_bits((y & 0xBFFFFFFFFFFFFFFFULL) | 0x3800000000000000ULL));
	}
	/* close exponents (the usual case), and huge gaps */
	for (n = 0; n < 50000; n++)
	{
		double y = from_bits((test_random() & 0x800FFFFFFFFFFFFFULL) | ((unsigned long long)(1023 - 30 + n % 60) << 52));
		double x = from_bits((test_random() & 0x800FFFFFFFFFFFFFULL) | ((unsigned long long)(1023 - 30 + (n * 7) % 90) << 52));
		double big = from_bits((test_random() & 0x800FFFFFFFFFFFFFULL) | ((unsigned long long)(1500 + n % 500) << 52));
		double tiny = from_bits((test_random() & 0x800FFFFFFFFFFFFFULL) | ((unsigned long long)(1 + n % 400) << 52));
		double subnormal = from_bits(test_random() & 0x800FFFFFFFFFFFFFULL);

		show_double(x, y);
		show_double(big, tiny);
		show_double(big, y);
		show_double(x, subnormal);
		show_float((float)x, (float)y);
		show_float((float)(x * 1e30), (float)(y * 1e-30));
	}
	/* the game's: angles into a turn, times into a period */
	for (n = 0; n < 20000; n++)
	{
		double angle = (double)(long long)(test_random() % 2000000001) / 1000.0 - 1000000.0;

		show_double(angle, 6.283185307179586);
		show_float((float)angle, 6.2831855f);
		show_double(angle * 0.0001, 1.0);
	}
	return test_finish("t_fmod");
}
