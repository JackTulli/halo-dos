/*
T_PRINTF.C

The printf family the game and the platform layer use in the 9x build
(port/xbox/include/crt/stdio.h maps their snprintf, sprintf and fprintf to
the MSVC printf of msvc_printf.c over PDCLib), against the UCRT's: the same
program is built a second time against the UCRT (tools/win9x_build.py) and
the two outputs must be equal line for line. Floating point (%f %e %g %a,
flags, widths, precisions, on special and random values), MSVC's length
modifiers (%I64d, %I32x), C99's (%lld), wide strings and characters in a
narrow format (%S %ls %ws %hs %C %lc), NULL strings, %p, the return values,
and fprintf to a stream.
*/

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "w9x_test.h"

#ifdef W9X_TEST
/* the game's printf (port/xbox/include/crt/stdio.h; msvc_printf.c) */
int halo_xbox_snprintf(char *buffer, size_t count, const char *format, ...);
int halo_xbox_sprintf(char *buffer, const char *format, ...);
int halo_msvc_vfprintf(FILE *stream, const char *format, va_list arguments);
#define test_snprintf halo_xbox_snprintf
#define test_sprintf halo_xbox_sprintf
static int test_fprintf(FILE *stream, const char *format, ...)
{
	va_list arguments;
	int result;

	va_start(arguments, format);
	result = halo_msvc_vfprintf(stream, format, arguments);
	va_end(arguments);
	return result;
}
#else
#define test_snprintf snprintf
#define test_sprintf sprintf
#define test_fprintf fprintf
#endif

static char text[2048];
static long line_count;

static void show(const char *format, int index, int result)
{
	static char escaped[sizeof(text) * 4];
	const unsigned char *from;
	char *to = escaped;

	/* (control characters and bytes beyond ASCII as \xHH: the two programs'
	standard output differs in text mode, and the UCRT's turns '\n' into
	"\r\n") */
	for (from = (const unsigned char *)text; *from; from++)
	{
		if (*from < 0x20 || *from >= 0x7F || *from == '\\')
		{
			static const char digits[] = "0123456789abcdef";

			*to++ = '\\';
			*to++ = 'x';
			*to++ = digits[*from >> 4];
			*to++ = digits[*from & 15];
		}
		else
			*to++ = (char)*from;
	}
	*to = 0;
	/* (the format's own '|' as '!': the separator) */
	printf("%s|%d|%d|%s\n", format, index, result, escaped);
	line_count++;
}

static void double_case(const char *format, int index, double value)
{
	int result = test_snprintf(text, sizeof(text), format, value);

	/* (the UCRT writes an infinity with the # flag as "i.nf": a note, not
	compared) */
	if (strchr(format, '#') && value - value != 0.0)
	{
		printf("# known UCRT difference: %s of an infinity: %s\n", format, text);
		return;
	}
	show(format, index, result);
}

int main(void)
{
	static const char *const double_formats[] = {
		"%f", "%.0f", "%.1f", "%.2f", "%.3f", "%.6f", "%.9f", "%.15f", "%.17f", "%.20f", "%.30f", "%e", "%.0e",
		"%.1e", "%.3e", "%.12e", "%.16e", "%.20e", "%E", "%g", "%.0g", "%.1g", "%.3g", "%.6g", "%.10g", "%.15g",
		"%.17g", "%G", "%10.3f", "%-10.3f!", "%+f", "% f", "%+e", "% g", "%010.2f", "%-010.2f!", "%#.0f", "%#.0e",
		"%#g", "%#.3g", "%#G", "%+.4e", "%12.4e", "%-12.4e!", "%08.3e", "%5.1f", "%*.*f", "%-*.*e", "%Lf", "%Lg",
		"x %.2f y", "a%%b %.1f c", "%.2f%%", "%3.0f", "%.99f", "%.60e",
	};
	static const double specials[] = {
		0.0, 1.0, 0.5, 1.5, 2.5, 3.5, 0.125, 0.05, 0.15, 0.25, 0.35, 9.5, 99.5, 999.5, 0.95, 0.995, 1e-5, 1e-4,
		1e-3, 123456.0, 1234567.0, 1e15, 1e16, 1e17, 1e21, 1e22, 1e23, 1e100, 1e300, 1.7976931348623157e308,
		2.2250738585072014e-308, 4.9406564584124654e-324, 1e-300, 3.14159265358979, 2.718281828459045,
		1.0 / 3.0, 2.0 / 3.0, 999999.9999995, 9.9999995, 0.00009999995, 8.29, 3.0, 16777216.0,
		3.4028234663852886e38, 1.1754943508222875e-38, 4294967296.0, 4294967295.5, 100.0, 1e9, 999999999.5,
		1e-9, 5e-324, 0.1, 0.2, 0.3, 1e308, 9007199254740993.0, 0.000001, 1e6, 1e7,
	};
	static const char *const integer_formats[] = {
		"%d", "%i", "%u", "%x", "%X", "%o", "%5d", "%-5d!", "%05d", "%+d", "% d", "%#x", "%#X", "%#o", "%.3d",
		"%.0d", "%8.3d", "%-8.3x!", "%hd", "%hu", "%hhd", "%ld", "%lu", "%lx", "%c", "%5c", "%-5c!", "%I32d",
		"%I32x", "%I32u",
	};
	static const char *const long_formats[] = {
		"%I64d", "%I64u", "%I64x", "%I64X", "%lld", "%llu", "%llx", "%20I64d", "%-20lld!", "%020I64u", "%+I64d",
		"%#I64x", "%.25I64d", "%I64o",
	};
	static const int integers[] = { 0, 1, -1, 7, 42, -42, 255, 256, 65535, 65536, 100000, -100000, INT_MAX,
		INT_MIN, 0x7F, 0x80, 0xFF };
	static const long long longs[] = { 0, 1, -1, 4294967296LL, -4294967296LL, 1234567890123LL,
		-1234567890123LL, 9223372036854775807LL, -9223372036854775807LL - 1 };
	static const wchar_t wide[] = { 'W', 'i', 'd', 'e', ' ', 0xE9, 0x2014, 'x', 0 };
	static const wchar_t latin1[] = { 'W', 'i', 'd', 'e', ' ', 0xE9, 0xFF, 'x', 0 };
	size_t f, v;
	int i;

	/* ---------- floating point */
	for (f = 0; f < sizeof(double_formats) / sizeof(double_formats[0]); f++)
	{
		const char *format = double_formats[f];
		int index = 0;

		if (strchr(format, '*'))
		{
			for (v = 0; v < sizeof(specials) / sizeof(specials[0]); v++)
			{
				int width = (int)(v % 13), precision = (int)(v % 9);

				show(format, index++, test_snprintf(text, sizeof(text), format, width, precision, specials[v]));
				show(format, index++, test_snprintf(text, sizeof(text), format, -width, precision, -specials[v]));
			}
			continue;
		}
		for (v = 0; v < sizeof(specials) / sizeof(specials[0]); v++)
		{
			double_case(format, index++, specials[v]);
			double_case(format, index++, -specials[v]);
		}
		double_case(format, index++, 1.0 / 0.0 * (1.0 + (double)f * 0.0));
		double_case(format, index++, -1.0 / 0.0 * (1.0 + (double)f * 0.0));
		for (i = 0; i < 3000; i++)
		{
			union
			{
				unsigned long long bits;
				double value;
			} random;
			float single;

			random.bits = test_random();
			if (((random.bits >> 52) & 0x7FF) == 0x7FF)
				random.bits &= ~(1ULL << 62); /* (no NaN: their text differs by design) */
			double_case(format, index++, random.value);
			single = (float)((double)(test_random() % 2000000) / 1000.0 - 1000.0);
			double_case(format, index++, single);
			/* values near the precision's rounding point */
			double_case(format, index++, (double)(long long)(test_random() % 100000) / 1000.0 + 0.0005);
		}
	}

	/* ---------- integers */
	for (f = 0; f < sizeof(integer_formats) / sizeof(integer_formats[0]); f++)
	{
		int index = 0;

		for (v = 0; v < sizeof(integers) / sizeof(integers[0]); v++)
			show(integer_formats[f], index++, test_snprintf(text, sizeof(text), integer_formats[f], integers[v]));
		for (i = 0; i < 200; i++)
		{
			int value = (int)test_random();

			show(integer_formats[f], index++, test_snprintf(text, sizeof(text), integer_formats[f], value));
		}
	}
	for (f = 0; f < sizeof(long_formats) / sizeof(long_formats[0]); f++)
	{
		int index = 0;

		for (v = 0; v < sizeof(longs) / sizeof(longs[0]); v++)
			show(long_formats[f], index++, test_snprintf(text, sizeof(text), long_formats[f], longs[v]));
		for (i = 0; i < 200; i++)
		{
			long long value = (long long)test_random();

			show(long_formats[f], index++, test_snprintf(text, sizeof(text), long_formats[f], value));
		}
	}

	/* ---------- strings and characters, narrow and wide */
	show("%s", 0, test_snprintf(text, sizeof(text), "%s", "narrow"));
	show("%10s", 0, test_snprintf(text, sizeof(text), "%10s", "narrow"));
	show("%-10s!", 0, test_snprintf(text, sizeof(text), "%-10s!", "narrow"));
	show("%.3s", 0, test_snprintf(text, sizeof(text), "%.3s", "narrow"));
	show("%10.3s", 0, test_snprintf(text, sizeof(text), "%10.3s", "narrow"));
	show("%*.*s", 0, test_snprintf(text, sizeof(text), "%*.*s", -8, 2, "narrow"));
	show("%s NULL", 0, test_snprintf(text, sizeof(text), "%s", (char *)NULL));
	show("%hs", 0, test_snprintf(text, sizeof(text), "%hs", "narrow"));
	show("%hS", 0, test_snprintf(text, sizeof(text), "%hS", "narrow"));
	/* (beyond Latin-1, the UCRT fails the call, msvc_printf.c writes '?':
	a note, not compared) */
	i = test_snprintf(text, sizeof(text), "%S", wide);
	printf("# known UCRT difference: %%S of U+2014 gives %d [%s]\n", i, i >= 0 ? text : "");
	show("%S", 0, test_snprintf(text, sizeof(text), "%S", latin1));
	show("%ls", 0, test_snprintf(text, sizeof(text), "%ls", L"wide text"));
	show("%ws", 0, test_snprintf(text, sizeof(text), "%ws", L"wide text"));
	show("%12S!", 0, test_snprintf(text, sizeof(text), "%12S!", L"wide"));
	show("%-12ls!", 0, test_snprintf(text, sizeof(text), "%-12ls!", L"wide"));
	show("%.2S", 0, test_snprintf(text, sizeof(text), "%.2S", L"wide"));
	show("%S NULL", 0, test_snprintf(text, sizeof(text), "%S", (wchar_t *)NULL));
	show("%ls NULL", 0, test_snprintf(text, sizeof(text), "%ls", (wchar_t *)NULL));
	show("%C", 0, test_snprintf(text, sizeof(text), "[%C]", L'w'));
	show("%lc", 0, test_snprintf(text, sizeof(text), "[%lc]", L'w'));
	show("%hc", 0, test_snprintf(text, sizeof(text), "[%hc]", 'n'));
	show("%3C", 0, test_snprintf(text, sizeof(text), "[%3C]", L'w'));
	show("%-3lc", 0, test_snprintf(text, sizeof(text), "[%-3lc]", L'w'));
	show("%c 0xE9", 0, test_snprintf(text, sizeof(text), "[%c]", 0xE9));

	/* ---------- pointers */
	show("%p", 0, test_snprintf(text, sizeof(text), "%p", (void *)0x0012FF7C));
	show("%p", 1, test_snprintf(text, sizeof(text), "%p", (void *)0));
	show("%p", 2, test_snprintf(text, sizeof(text), "%p", (void *)0xDEADBEEF));
	show("%12p!", 0, test_snprintf(text, sizeof(text), "%12p!", (void *)0x1234));
	show("%-12p!", 0, test_snprintf(text, sizeof(text), "%-12p!", (void *)0x1234));

	/* ---------- the game's shapes (players.c, network logs) */
	show("game 1", 0, test_snprintf(text, sizeof(text),
		"player %ld unit %lx at %.2f %.2f %.2f, nearest %lx (tag %lx %s) at %.2f", 3L, 0xe1234L, 1.0, 8.29, -3.5,
		0x1111L, 0xfeedL, "weapons\\pistol", 2.345));
	show("game 2", 0, test_snprintf(text, sizeof(text), "null %s | %I64d | %I32u | [%*.*f] | %% | %-*d|",
		(char *)NULL, 1234567890123LL, 42u, 9, 3, 3.14159, 5, 7));
	show("game 3", 0, test_snprintf(text, sizeof(text), "%s: %d of %d (%.1f%%) %c %S", "loading", 5, 7,
		500.0 / 7.0, '!', L"ok"));

	/* ---------- truncation and sprintf */
	{
		char small[8];
		int result = test_snprintf(small, sizeof(small), "%s %f", "truncated", 1.5);

		snprintf(text, sizeof(text), "%s", small);
		show("truncated", 0, result);
		show("sprintf", 0, test_sprintf(text, "%05.1f|%-6s|%I64x", 2.25, "ab", 0x123456789ABCDEFULL));
	}

	/* ---------- fprintf to a stream */
	fflush(stdout);
	test_fprintf(stdout, "fprintf|%s|%.3f|%e|%I64d|%S|%p\n", "stream", 2.0 / 3.0, 12345.678, -5LL, L"wide",
		(void *)0xABCDEF);
	fflush(stdout);
	printf("# %ld lines\n", line_count);
	return test_finish("t_printf");
}
