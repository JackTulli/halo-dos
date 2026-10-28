/*
T_STRTOD.C

strtod and atof of the 9x build (port/xbox/src/msvc_strtod.c, the Xbox
build's), against the UCRT's: the forms of the game's script literals
(source/hs/hs_compile.c reads them with atof) and of its settings
(tomlc17), then random decimal numbers of up to 15 significant digits and
exponents within +-22, which msvc_strtod.c converts exactly; all must give
the UCRT's bits. Longer numbers, which msvc_strtod.c may round an ulp
away, are listed as notes ('#' lines) for the report, not compared.
*/

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "w9x_test.h"

static unsigned long long bits_of(double value)
{
	union
	{
		double value;
		unsigned long long bits;
	} u;

	u.value = value;
	return u.bits;
}

static void convert(const char *prefix, const char *text)
{
	char *end;
	double value;
	float single;
	union
	{
		float value;
		unsigned int bits;
	} f;

	errno = 0;
	value = strtod(text, &end);
	single = strtof(text, NULL);
	f.value = single;
	printf("%s%s|%08lx%08lx|%d|%08x|%s\n", prefix, text, (unsigned long)(bits_of(value) >> 32),
		(unsigned long)bits_of(value), (int)(end - text), f.bits,
		bits_of(atof(text)) == bits_of(value) ? "atof" : "ATOF DIFFERS");
}

/* a random decimal number: sign, digits (some before the point), an
exponent */
static void random_number(char *text, int digits, int exponent_range)
{
	int before = (int)(test_random() % (unsigned)(digits + 1)), i, n = 0;
	int exponent = (int)(test_random() % (unsigned)(2 * exponent_range + 1)) - exponent_range;

	if (test_random() & 1)
		text[n++] = '-';
	if (before == 0)
		text[n++] = '0';
	for (i = 0; i < digits; i++)
	{
		if (i == before)
			text[n++] = '.';
		text[n++] = (char)('0' + test_random() % 10);
	}
	if (exponent || (test_random() & 1))
		n += sprintf(text + n, "e%d", exponent);
	text[n] = 0;
}

int main(void)
{
	static const char *const literals[] = {
		"0", "1", "-1", "0.0", "1.0", "-1.0", "1.", ".5", "-.5", "0.5", "0.25", "0.1", "0.2", "0.3", "0.7",
		"3.14159", "3.1415926535", "2.718281828", "100", "100.0", "65535", "2147483647", "2147483648",
		"4294967296", "1e3", "1E3", "1e-3", "1.5e-3", "2.5E+2", "-7.25e1", "0.0001", "0.001", "0.333333",
		"0.666667", "1000000", "1e10", "1e22", "1e-22", "123456789012345", "0.000000001",
		"3.40282347e+38", "2.2250738585072014e-308", "4.9406564584124654e-324",
		"1e309", "-1e309", "1e-400", "  42.5", "\t-3.5", "+8", "7.5abc", "1e", "1e+", "e5", ".", "-", "",
		"0x1p4", "0x10", "0X1.8p1", "inf", "-inf", "infinity", "INF", "0.1f", "30.", "0.05", "0.95",
		"60.0", "1.0e0", "9.999999", "99999.99", "0.0174532925", "57.2957795",
	};
	/* beyond msvc_strtod.c's exact range (more than 15 digits, or an
	exponent beyond +-22), and the NaN's payload: notes */
	static const char *const beyond[] = {
		"1.17549435e-38", "1.7976931348623157e308", "nan",
	};
	char text[64];
	size_t i;
	int n, long_mismatch_candidates = 0;

	for (i = 0; i < sizeof(literals) / sizeof(literals[0]); i++)
		convert("", literals[i]);
	for (i = 0; i < sizeof(beyond) / sizeof(beyond[0]); i++)
		convert("# beyond ", beyond[i]);
	/* exact for up to 15 significant digits and exponents within +-22 */
	for (n = 0; n < 60000; n++)
	{
		random_number(text, 1 + (int)(test_random() % 15), 22 - 15);
		convert("", text);
	}
	for (n = 0; n < 20000; n++)
	{
		random_number(text, 1 + (int)(test_random() % 6), 16);
		convert("", text);
	}
	/* beyond: notes only */
	for (n = 0; n < 20000; n++)
	{
		random_number(text, 16 + (int)(test_random() % 5), 300);
		convert("# long ", text);
		long_mismatch_candidates++;
	}
	printf("# %d long numbers listed as notes\n", long_mismatch_candidates);
	return test_finish("t_strtod");
}
