/*
MSVC_STRTOD.C

strtod, strtof and strtold for the builds that use nxdk's C library
(PDCLib), whose own only stub them: the Xbox build and the Windows 98 SE /
Me build. The game's script compiler reads its real literals with atof
(source/hs/hs_compile.c), the settings loader its defaults with strtod
(tomlc17), the wide functions theirs through it (port/linux/src/
msvc_wide.c); the same function on both makes the same values of the same
text.

The conversions scale by exact powers of ten: a value with up to 15
significant digits and a decimal exponent within +-22 is exact (both
factors are), longer inputs may be off by an ulp, which the game's text
(settings, script literals, wide strings) never notices.

Split out of port/xbox/src/nx_stdlib.c (with mbtowc and MSVC's byte swaps),
which keeps the console's rand_s and its errno.
*/

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* ---------- strtod and family */

static int is_space(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static int lower(char c)
{
	return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

static int hex_digit(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	c = (char)lower(c);
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return -1;
}

/* a case-insensitive match of word at text; returns the length matched */
static size_t match_word(const char *text, const char *word)
{
	size_t length = 0;

	while (word[length] && lower(text[length]) == word[length])
		length++;
	return word[length] ? 0 : length;
}

static long double infinity(void)
{
	return __builtin_infl();
}

/* value * 10^exponent, in steps by exact powers (10^22 is the largest
exact one): a single step for the usual exponents, and no intermediate
overflows or underflows on the way to a large or a subnormal result */
static long double scale_by_ten(long double value, long exponent)
{
	static const long double powers[] = {
		1e0L, 1e1L, 1e2L, 1e3L, 1e4L, 1e5L, 1e6L, 1e7L, 1e8L, 1e9L, 1e10L,
		1e11L, 1e12L, 1e13L, 1e14L, 1e15L, 1e16L, 1e17L, 1e18L, 1e19L, 1e20L,
		1e21L, 1e22L
	};
	int negative = exponent < 0;
	unsigned long e = negative ? (unsigned long)-exponent : (unsigned long)exponent;

	if (value == 0.0L)
		return value;
	if (e > 700)
		return negative ? 0.0L : infinity();
	while (e > 0)
	{
		unsigned long step = e > 22 ? 22 : e;

		value = negative ? value / powers[step] : value * powers[step];
		e -= step;
	}
	return value;
}

/* value * 2^exponent */
static long double scale_by_two(long double value, long exponent)
{
	while (exponent > 0 && value != 0.0L && value < infinity())
	{
		value *= 2.0L;
		exponent--;
	}
	while (exponent < 0 && value != 0.0L)
	{
		value *= 0.5L;
		exponent++;
	}
	return value;
}

/* the digits and exponent of a decimal or hexadecimal number at text;
returns the end of what was read, or NULL when text has no number */
static const char *parse_real(const char *text, long double *result)
{
	const char *cursor = text;
	int negative = 0;
	int hexadecimal = 0;
	int base = 10;
	unsigned long long mantissa = 0;
	int any_digits = 0;
	long exponent = 0;      /* decimal (or binary, for hexadecimal) exponent of mantissa */
	long double value;

	while (is_space(*cursor))
		cursor++;
	if (*cursor == '+' || *cursor == '-')
		negative = *cursor++ == '-';

	if (match_word(cursor, "inf"))
	{
		cursor += match_word(cursor, "infinity") ? 8 : 3;
		*result = negative ? -infinity() : infinity();
		return cursor;
	}
	if (match_word(cursor, "nan"))
	{
		cursor += 3;
		if (*cursor == '(')
		{
			const char *close = cursor + 1;

			while ((*close >= '0' && *close <= '9') || (lower(*close) >= 'a' && lower(*close) <= 'z') || *close == '_')
				close++;
			if (*close == ')')
				cursor = close + 1;
		}
		*result = __builtin_nanl("");
		return cursor;
	}

	if (cursor[0] == '0' && lower(cursor[1]) == 'x' && (hex_digit(cursor[2]) >= 0 ||
		(cursor[2] == '.' && hex_digit(cursor[3]) >= 0)))
	{
		hexadecimal = 1;
		base = 16;
		cursor += 2;
	}

	for (;;)
	{
		int digit = hexadecimal ? hex_digit(*cursor) : (*cursor >= '0' && *cursor <= '9' ? *cursor - '0' : -1);

		if (digit < 0)
			break;
		any_digits = 1;
		if (mantissa < (~0ULL - 15) / 16 || (!hexadecimal && mantissa < (~0ULL - 9) / 10))
		{
			mantissa = mantissa * (unsigned long long)base + (unsigned long long)digit;
		}
		else
		{
			/* beyond 19 digits: the digit only scales the value */
			exponent += hexadecimal ? 4 : 1;
		}
		cursor++;
	}
	if (*cursor == '.')
	{
		const char *fraction = cursor + 1;

		while (1)
		{
			int digit = hexadecimal ? hex_digit(*fraction) : (*fraction >= '0' && *fraction <= '9' ? *fraction - '0' : -1);

			if (digit < 0)
				break;
			any_digits = 1;
			if (mantissa < (~0ULL - 15) / 16 || (!hexadecimal && mantissa < (~0ULL - 9) / 10))
			{
				mantissa = mantissa * (unsigned long long)base + (unsigned long long)digit;
				exponent -= hexadecimal ? 4 : 1;
			}
			fraction++;
		}
		if (any_digits)
			cursor = fraction;
	}
	if (!any_digits)
		return NULL;

	/* the exponent part: e for decimal, p for hexadecimal (binary) */
	if (lower(*cursor) == (hexadecimal ? 'p' : 'e'))
	{
		const char *marker = cursor + 1;
		int exponent_negative = 0;
		long exponent_value = 0;

		if (*marker == '+' || *marker == '-')
			exponent_negative = *marker++ == '-';
		if (*marker >= '0' && *marker <= '9')
		{
			while (*marker >= '0' && *marker <= '9')
			{
				if (exponent_value < 100000)
					exponent_value = exponent_value * 10 + (*marker - '0');
				marker++;
			}
			exponent += exponent_negative ? -exponent_value : exponent_value;
			cursor = marker;
		}
	}

	value = (long double)mantissa;
	value = hexadecimal ? scale_by_two(value, exponent) : scale_by_ten(value, exponent);
	*result = negative ? -value : value;
	return cursor;
}

double strtod(const char *restrict text, char **restrict end)
{
	long double value = 0.0L;
	const char *stop = parse_real(text, &value);
	double result;

	if (!stop)
	{
		if (end)
			*end = (char *)text;
		return 0.0;
	}
	if (end)
		*end = (char *)stop;
	result = (double)value;
	if ((result == __builtin_inf() || result == -__builtin_inf()) && value != infinity() && value != -infinity())
		errno = ERANGE;
	else if (result == 0.0 && value != 0.0L)
		errno = ERANGE;
	return result;
}

float strtof(const char *restrict text, char **restrict end)
{
	return (float)strtod(text, end);
}

long double strtold(const char *restrict text, char **restrict end)
{
	long double value = 0.0L;
	const char *stop = parse_real(text, &value);

	if (!stop)
	{
		if (end)
			*end = (char *)text;
		return 0.0L;
	}
	if (end)
		*end = (char *)stop;
	return value;
}

/* ---------- the rest of nxdk's stdlib_ext_.obj */

/* the game's text is single-byte */
int mbtowc(wchar_t *character, const char *text, size_t count)
{
	if (!text)
		return 0;
	if (!count || !*text)
		return 0;
	if (character)
		*character = (wchar_t)(unsigned char)*text;
	return 1;
}

/* the script compiler reads real literals with atof */
double atof(const char *text)
{
	return strtod(text, NULL);
}

unsigned short _byteswap_ushort(unsigned short value)
{
	return __builtin_bswap16(value);
}

unsigned long _byteswap_ulong(unsigned long value)
{
	return __builtin_bswap32(value);
}

unsigned long long _byteswap_uint64(unsigned long long value)
{
	return __builtin_bswap64(value);
}
