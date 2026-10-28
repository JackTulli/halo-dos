/*
MSVC_PRINTF.C

The printf family of the game and the platform layer for the builds that
use nxdk's C library (PDCLib): the Xbox build and the Windows 98 SE / Me
build. PDCLib reads neither the Microsoft C runtime's length modifiers
(%I64d, %I32x) nor floating point (%f, %e, %g), nor wide strings in a
narrow format (%S, %ls); each of these took the following arguments out of
step. Here a call's arguments are prepared into what PDCLib reads right,
and PDCLib's own printf does the rest.

Split out of port/xbox/src/msvc_crt.c, which keeps the console's log and
screen; the 9x build adds its own sinks (port/win9x/src/w9x_crt.c).
*/

/* the printf family here calls PDCLib's own (crt/stdio.h) */
#define HALO_XBOX_PRINTF_IMPLEMENTATION

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

int halo_xbox_vsnprintf(char *buffer, size_t count, const char *format, va_list arguments);
int halo_xbox_vsprintf(char *buffer, const char *format, va_list arguments);
int halo_msvc_vfprintf(FILE *stream, const char *format, va_list arguments);

/* ---------- printf with MSVC length modifiers (as port/linux/src/msvc_crt.c)

Rewrites %I64 as %ll and drops %I32; PDCLib reads the rest. */
static const char *translate_format(const char *format, char *buffer, size_t size)
{
	const char *cursor;
	size_t length = 0;

	if (!strstr(format, "I64") && !strstr(format, "I32"))
		return format;
	for (cursor = format; *cursor && length + 3 < size; cursor++)
	{
		buffer[length++] = *cursor;
		if (*cursor != '%')
			continue;
		if (cursor[1] == '%')
		{
			buffer[length++] = *++cursor;
			continue;
		}
		/* copy flags, width and precision */
		while (cursor[1] && strchr("-+ #0123456789.*", cursor[1]) && length + 3 < size)
			buffer[length++] = *++cursor;
		if (cursor[1] == 'I' && cursor[2] == '6' && cursor[3] == '4')
		{
			buffer[length++] = 'l';
			buffer[length++] = 'l';
			cursor += 3;
		}
		else if (cursor[1] == 'I' && cursor[2] == '3' && cursor[3] == '2')
		{
			cursor += 3;
		}
	}
	buffer[length] = '\0';
	return buffer;
}

/* ---------- floating-point conversions

PDCLib's printf neither formats %f, %e, %g (or %a) nor takes their double
from the argument list, so every later argument was read out of step: a %s
after a %f took half a double as its pointer (the host's crashes of
2026-09-29 and 2026-09-30 in network_player_log_idle_action, players.c).
They are formatted here and handed to PDCLib as strings.

The method is musl's (fmt_fp, src/stdio/vfprintf.c; MIT licence, as
port/third_party/musl-math) for the double: the value's exact decimal
expansion in limbs of nine digits, rounded to the precision half to even,
by integer arithmetic alone, so the x87's precision setting (the game's, or
Direct3D's) does not enter into it. Exponents have two digits at least, as
on the other ports. %a is written as %e. */

#define FORMAT_LEFT 1u
#define FORMAT_PLUS 2u
#define FORMAT_SPACE 4u
#define FORMAT_ALTERNATE 8u
#define FORMAT_ZERO 16u

#define FORMAT_MINIMUM(a, b) ((a) < (b) ? (a) : (b))
#define FORMAT_MAXIMUM(a, b) ((a) > (b) ? (a) : (b))

struct format_output
{
	char *text;
	int size;
	int length;
};

static void format_put(struct format_output *output, const char *text, long count)
{
	for (; count > 0; count--, text++)
	{
		if (output->length < output->size - 1)
			output->text[output->length++] = *text;
	}
	if (output->size > 0)
		output->text[output->length] = 0;
}

/* pads to width with c unless left-adjusted or zero-padded (musl's pad) */
static void format_pad(struct format_output *output, char c, int width, int length, unsigned flags)
{
	if ((flags & (FORMAT_LEFT | FORMAT_ZERO)) || length >= width)
		return;
	for (; length < width; length++)
		format_put(output, &c, 1);
}

/* the decimal digits of value, written backwards ending at end (none for
zero); returns the first */
static char *format_unsigned(unsigned long value, char *end)
{
	for (; value; value /= 10)
		*--end = (char)('0' + value % 10);
	return end;
}

static void format_double(struct format_output *output, double value, int width, int precision, unsigned flags,
	int conversion)
{
	enum { MANTISSA_DIGITS = 53, MAXIMUM_EXPONENT = 1024 };
	unsigned long big[(MANTISSA_DIGITS + 28) / 29 + 1 + (MAXIMUM_EXPONENT + MANTISSA_DIGITS + 28 + 8) / 9];
	unsigned long *a, *d, *r, *z;
	int e2, e, i, j, l, p = precision, t = conversion, pl;
	char buf[9 + MANTISSA_DIGITS / 4], *s;
	const char *prefix = "-0X+0X 0X-0x+0x 0x";
	char ebuf0[3 * sizeof(int)], *ebuf = &ebuf0[3 * sizeof(int)], *estr = ebuf;
	union
	{
		double value;
		unsigned long long bits;
	} parts;
	unsigned long long mantissa;
	int biased;

	parts.value = value;
	biased = (int)((parts.bits >> 52) & 0x7FF);
	mantissa = parts.bits & 0xFFFFFFFFFFFFFULL;
	pl = 1;
	if (parts.bits >> 63)
		;
	else if (flags & FORMAT_PLUS)
		prefix += 3;
	else if (flags & FORMAT_SPACE)
		prefix += 6;
	else
		prefix++, pl = 0;

	if (biased == 0x7FF)
	{
		const char *text = mantissa ? ((t & 32) ? "nan" : "NAN") : ((t & 32) ? "inf" : "INF");

		format_pad(output, ' ', width, 3 + pl, flags & ~FORMAT_ZERO);
		format_put(output, prefix, pl);
		format_put(output, text, 3);
		format_pad(output, ' ', width, 3 + pl, flags ^ FORMAT_LEFT);
		return;
	}
	if ((t | 32) == 'a')
		t = (t & 32) ? 'e' : 'E';
	if (p < 0)
		p = 6;

	/* value = y * 2^e2 with y = m / 2^24 in [2^28, 2^29), m the mantissa
	with its 53rd bit set (musl's frexpl, * 2, * 2^28) */
	e2 = 0;
	if (biased)
	{
		mantissa |= 1ULL << 52;
		e2 = biased - 1075 + 24;
	}
	else if (mantissa)
	{
		e2 = -1074 + 24;
		while (!(mantissa & (1ULL << 52)))
		{
			mantissa <<= 1;
			e2--;
		}
	}

	if (e2 < 0)
		a = r = z = big;
	else
		a = r = z = big + sizeof(big) / sizeof(*big) - MANTISSA_DIGITS - 1;

	/* y in limbs: its integer part, then its fraction (of 24 bits, whose
	expansion in base 10^9 ends within three limbs) */
	*z++ = (unsigned long)(mantissa >> 24);
	{
		unsigned long long fraction = mantissa & 0xFFFFFFULL;

		while (fraction)
		{
			unsigned long long scaled = fraction * 1000000000ULL;

			*z++ = (unsigned long)(scaled >> 24);
			fraction = scaled & 0xFFFFFFULL;
		}
	}

	while (e2 > 0)
	{
		unsigned long carry = 0;
		int sh = FORMAT_MINIMUM(29, e2);

		for (d = z - 1; d >= a; d--)
		{
			unsigned long long x = ((unsigned long long)*d << sh) + carry;

			*d = (unsigned long)(x % 1000000000);
			carry = (unsigned long)(x / 1000000000);
		}
		if (carry)
			*--a = carry;
		while (z > a && !z[-1])
			z--;
		e2 -= sh;
	}
	while (e2 < 0)
	{
		unsigned long carry = 0, *b;
		int sh = FORMAT_MINIMUM(9, -e2), need = 1 + (p + MANTISSA_DIGITS / 3 + 8) / 9;

		for (d = a; d < z; d++)
		{
			unsigned long rm = *d & ((1UL << sh) - 1);

			*d = (*d >> sh) + carry;
			carry = (1000000000UL >> sh) * rm;
		}
		if (!*a)
			a++;
		if (carry)
			*z++ = carry;
		/* no digits past the precision */
		b = (t | 32) == 'f' ? r : a;
		if (z - b > need)
			z = b + need;
		e2 += sh;
	}

	if (a < z)
	{
		for (i = 10, e = 9 * (r - a); *a >= (unsigned long)i; i *= 10, e++)
			;
	}
	else
		e = 0;

	/* rounding, half to even: j is the precision after the point */
	j = p - ((t | 32) != 'f') * e - ((t | 32) == 'g' && p);
	if (j < 9 * (z - r - 1))
	{
		unsigned long x;

		d = r + 1 + ((j + 9 * MAXIMUM_EXPONENT) / 9 - MAXIMUM_EXPONENT);
		j += 9 * MAXIMUM_EXPONENT;
		j %= 9;
		for (i = 10, j++; j < 9; i *= 10, j++)
			;
		x = *d % i;
		if (x || d + 1 != z)
		{
			int odd = ((*d / i) & 1) || (i == 1000000000 && d > a && (d[-1] & 1));
			int up = x > (unsigned long)i / 2 || (x == (unsigned long)i / 2 && (d + 1 != z || odd));

			*d -= x;
			if (up)
			{
				*d = *d + i;
				while (*d > 999999999)
				{
					*d-- = 0;
					if (d < a)
						*--a = 0;
					(*d)++;
				}
				for (i = 10, e = 9 * (r - a); *a >= (unsigned long)i; i *= 10, e++)
					;
			}
		}
		if (z > d + 1)
			z = d + 1;
	}
	for (; z > a && !z[-1]; z--)
		;

	if ((t | 32) == 'g')
	{
		if (!p)
			p++;
		if (p > e && e >= -4)
		{
			t--;
			p -= e + 1;
		}
		else
		{
			t -= 2;
			p--;
		}
		if (!(flags & FORMAT_ALTERNATE))
		{
			/* the trailing zeros of the last limb */
			if (z > a && z[-1])
			{
				for (i = 10, j = 0; z[-1] % i == 0; i *= 10, j++)
					;
			}
			else
				j = 9;
			if ((t | 32) == 'f')
				p = FORMAT_MINIMUM(p, FORMAT_MAXIMUM(0, 9 * (z - r - 1) - j));
			else
				p = FORMAT_MINIMUM(p, FORMAT_MAXIMUM(0, 9 * (z - r - 1) + e - j));
		}
	}
	l = 1 + p + (p || (flags & FORMAT_ALTERNATE));
	if ((t | 32) == 'f')
	{
		if (e > 0)
			l += e;
	}
	else
	{
		estr = format_unsigned(e < 0 ? -e : e, ebuf);
		while (ebuf - estr < 2)
			*--estr = '0';
		*--estr = (char)(e < 0 ? '-' : '+');
		*--estr = (char)t;
		l += (int)(ebuf - estr);
	}

	format_pad(output, ' ', width, pl + l, flags);
	format_put(output, prefix, pl);
	format_pad(output, '0', width, pl + l, flags ^ FORMAT_ZERO);

	if ((t | 32) == 'f')
	{
		if (a > r)
			a = r;
		for (d = a; d <= r; d++)
		{
			s = format_unsigned(*d, buf + 9);
			if (d != a)
			{
				while (s > buf)
					*--s = '0';
			}
			else if (s == buf + 9)
				*--s = '0';
			format_put(output, s, buf + 9 - s);
		}
		if (p || (flags & FORMAT_ALTERNATE))
			format_put(output, ".", 1);
		for (; d < z && p > 0; d++, p -= 9)
		{
			s = format_unsigned(*d, buf + 9);
			while (s > buf)
				*--s = '0';
			format_put(output, s, FORMAT_MINIMUM(9, p));
		}
		format_pad(output, '0', p + 9, 9, 0);
	}
	else
	{
		if (z <= a)
			z = a + 1;
		for (d = a; d < z && p >= 0; d++)
		{
			s = format_unsigned(*d, buf + 9);
			if (s == buf + 9)
				*--s = '0';
			if (d != a)
			{
				while (s > buf)
					*--s = '0';
			}
			else
			{
				format_put(output, s++, 1);
				if (p > 0 || (flags & FORMAT_ALTERNATE))
					format_put(output, ".", 1);
			}
			format_put(output, s, FORMAT_MINIMUM(buf + 9 - s, p));
			p -= (int)(buf + 9 - s);
		}
		format_pad(output, '0', p + 18, 18, 0);
		format_put(output, estr, ebuf - estr);
	}
	format_pad(output, ' ', width, pl + l, flags ^ FORMAT_LEFT);
}

/* ---------- MSVC's printf and PDCLib's

The arguments of a printf-family call, made into what PDCLib reads right:
MSVC's %I64 as %ll and %I32 dropped, a NULL string as "(null)" (MSVC's;
PDCLib reads through it), each floating-point conversion formatted above
and passed as a string, a wide string or character in a narrow format
(MSVC's %S, %ls, %ws, %C, %lc; %hs and %hS are narrow) narrowed to the C
locale's bytes, and %p written as MSVC writes it (eight upper-case hex
digits). The arguments are copied to an array of stack words (cdecl on the
x86: what a va_list points at). Returns the words as a va_list and the
format that goes with them in *prepared_format; when the format is not
understood, the call's own arguments and its format as translate_format
leaves it. */

#define MAXIMUM_ARGUMENT_WORDS 64
#define MAXIMUM_PREPARED_FORMAT 1024
#define MAXIMUM_NUMBER_TEXT 1024
/* digits beyond these are not asked for; they bound the numbers' text */
#define MAXIMUM_NUMBER_PRECISION 350

struct printf_preparation
{
	unsigned long words[MAXIMUM_ARGUMENT_WORDS];
	char format[MAXIMUM_PREPARED_FORMAT];
	char numbers[MAXIMUM_NUMBER_TEXT];
};

static const char null_text[] = "(null)";
static const wchar_t null_wide_text[] = L"(null)";

static int prepared_append(struct printf_preparation *preparation, int *length, const char *text, int count)
{
	if (*length + count >= MAXIMUM_PREPARED_FORMAT)
		return 0;
	memcpy(preparation->format + *length, text, count);
	*length += count;
	return 1;
}

/* a conversion written anew: %, the left adjustment, the width and the
precision as numbers (any * already read), then the conversion */
static int prepared_append_conversion(struct printf_preparation *preparation, int *length, unsigned flags,
	int width, int precision, const char *conversion)
{
	char text[40];
	char digits[12];
	int count = 0, digit_count;

	text[count++] = '%';
	if (flags & FORMAT_LEFT)
		text[count++] = '-';
	if (width > 0)
	{
		for (digit_count = 0; width; width /= 10)
			digits[digit_count++] = (char)('0' + width % 10);
		while (digit_count)
			text[count++] = digits[--digit_count];
	}
	if (precision >= 0)
	{
		text[count++] = '.';
		digit_count = 0;
		do
		{
			digits[digit_count++] = (char)('0' + precision % 10);
			precision /= 10;
		} while (precision);
		while (digit_count)
			text[count++] = digits[--digit_count];
	}
	while (*conversion)
		text[count++] = *conversion++;
	return prepared_append(preparation, length, text, count);
}

/* a wide character as the C locale's byte: Latin-1 as itself, the rest as
'?' (where MSVC's runtime fails the call with EILSEQ) */
static char narrowed(unsigned long character)
{
	character &= 0xFFFF;
	return character < 0x100 ? (char)character : '?';
}

/* whether a format has anything PDCLib reads wrong: a floating-point
conversion, a string (which may be NULL) or MSVC's I64/I32; most of the
game's and the platform's formats (integers and text) go to PDCLib as they
are, without the copy */
static int format_needs_preparing(const char *format)
{
	const char *cursor;

	for (cursor = strchr(format, '%'); cursor; cursor = strchr(cursor, '%'))
	{
		cursor++;
		if (*cursor == '%')
		{
			cursor++;
			continue;
		}
		while (*cursor && strchr("-+ #0'*.0123456789hljzqtLw", *cursor))
			cursor++;
		if (!*cursor)
			break;
		if (*cursor == 'I' || strchr("fFeEgGaAsScCp", *cursor))
			return 1;
	}
	return 0;
}

static va_list prepare_arguments(const char *format, va_list arguments, struct printf_preparation *preparation,
	const char **prepared_format)
{
	unsigned long *words = preparation->words;
	const char *cursor;
	int count = 0, length = 0, numbers_used = 0;
	va_list in;

	if (!format_needs_preparing(format))
	{
		*prepared_format = format;
		return arguments;
	}
	va_copy(in, arguments);
	for (cursor = format; *cursor; cursor++)
	{
		const char *specification = cursor;
		int first_word = count, wide = 0, longs = 0, halves = 0, width = 0, precision = -1;
		unsigned flags = 0;

		if (*cursor != '%')
		{
			/* the text up to the next conversion, whole */
			const char *next = strchr(cursor, '%');
			int run = next ? (int)(next - cursor) : (int)strlen(cursor);

			if (!prepared_append(preparation, &length, cursor, run))
				goto give_up;
			cursor += run - 1;
			continue;
		}
		cursor++;
		if (*cursor == '%')
		{
			if (!prepared_append(preparation, &length, "%%", 2))
				goto give_up;
			continue;
		}
		for (; *cursor && strchr("-+ #0'", *cursor); cursor++)
		{
			flags |= *cursor == '-' ? FORMAT_LEFT : *cursor == '+' ? FORMAT_PLUS : *cursor == ' ' ? FORMAT_SPACE :
				*cursor == '#' ? FORMAT_ALTERNATE : *cursor == '0' ? FORMAT_ZERO : 0;
		}
		if (*cursor == '*')
		{
			if (count >= MAXIMUM_ARGUMENT_WORDS)
				goto give_up;
			width = (int)(words[count++] = va_arg(in, unsigned long));
			if (width < 0)
			{
				flags |= FORMAT_LEFT;
				width = -width;
			}
			cursor++;
		}
		else
		{
			for (; *cursor >= '0' && *cursor <= '9'; cursor++)
				width = width * 10 + (*cursor - '0');
		}
		if (*cursor == '.')
		{
			cursor++;
			precision = 0;
			if (*cursor == '*')
			{
				if (count >= MAXIMUM_ARGUMENT_WORDS)
					goto give_up;
				precision = (int)(words[count++] = va_arg(in, unsigned long));
				if (precision < 0)
					precision = -1;
				cursor++;
			}
			else
			{
				for (; *cursor >= '0' && *cursor <= '9'; cursor++)
					precision = precision * 10 + (*cursor - '0');
			}
		}
		for (;; cursor++)
		{
			if (*cursor == 'l')
				longs++;
			else if (*cursor == 'j' || *cursor == 'L' || *cursor == 'q')
				longs = 2;
			else if (*cursor == 'w')
				wide = 1;
			else if (*cursor == 'h')
				halves++;
			else if (cursor[0] == 'I' && cursor[1] == '6' && cursor[2] == '4')
			{
				longs = 2;
				cursor += 2;
			}
			else if (cursor[0] == 'I' && cursor[1] == '3' && cursor[2] == '2')
				cursor += 2;
			else if (*cursor != 'h' && *cursor != 'z' && *cursor != 't')
				break;
		}
		if (count + 2 > MAXIMUM_ARGUMENT_WORDS)
			goto give_up;
		switch (*cursor)
		{
		case 'd': case 'i': case 'o': case 'u': case 'x': case 'X':
			words[count++] = va_arg(in, unsigned long);
			if (longs >= 2)
				words[count++] = va_arg(in, unsigned long);
			break;
		case 'n':
			words[count++] = va_arg(in, unsigned long);
			break;
		case 'p':
			/* MSVC's: eight upper-case hex digits, no prefix */
			count = first_word;
			words[count++] = va_arg(in, unsigned long);
			if (!prepared_append_conversion(preparation, &length, flags, width, 8, "lX"))
				goto give_up;
			continue;
		case 'c': case 'C':
		{
			unsigned long character = va_arg(in, unsigned long);

			if (!halves && (longs || wide || *cursor == 'C'))
				character = (unsigned char)narrowed(character);
			count = first_word;
			words[count++] = character & 0xFF;
			if (!prepared_append_conversion(preparation, &length, flags, width, -1, "c"))
				goto give_up;
			continue;
		}
		case 's': case 'S':
		{
			unsigned long pointer = va_arg(in, unsigned long);

			if (!halves && (longs || wide || *cursor == 'S'))
			{
				/* a wide string: narrowed here, up to its precision (which
				counts the bytes written) */
				const wchar_t *text = pointer ? (const wchar_t *)pointer : null_wide_text;
				char *narrow = preparation->numbers + numbers_used;
				int room = MAXIMUM_NUMBER_TEXT - numbers_used - 1;
				int used = 0;

				if (room < 0)
					goto give_up;
				for (; text[used] && (precision < 0 || used < precision); used++)
				{
					if (used >= room)
						goto give_up;
					narrow[used] = narrowed(text[used]);
				}
				narrow[used] = 0;
				numbers_used += used + 1;
				count = first_word;
				words[count++] = (unsigned long)narrow;
				if (!prepared_append_conversion(preparation, &length, flags, width, -1, "s"))
					goto give_up;
				continue;
			}
			count = first_word;
			words[count++] = pointer ? pointer : (unsigned long)null_text;
			if (!prepared_append_conversion(preparation, &length, flags, width, precision, "s"))
				goto give_up;
			continue;
		}
		case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
		{
			/* (a long double is a double on this target) */
			double value = va_arg(in, double);
			struct format_output output;

			if (numbers_used >= MAXIMUM_NUMBER_TEXT)
				goto give_up;
			output.text = preparation->numbers + numbers_used;
			output.size = MAXIMUM_NUMBER_TEXT - numbers_used;
			output.length = 0;
			output.text[0] = 0;
			if (flags & FORMAT_LEFT)
				flags &= ~FORMAT_ZERO;
			format_double(&output, value, width, FORMAT_MINIMUM(precision, MAXIMUM_NUMBER_PRECISION), flags,
				*cursor);
			/* the text stands for the conversion, its * width and precision too */
			count = first_word;
			words[count++] = (unsigned long)output.text;
			numbers_used += output.length + 1;
			if (!prepared_append(preparation, &length, "%s", 2))
				goto give_up;
			continue;
		}
		default:
			goto give_up;
		}
		/* the conversion as written, with MSVC's length modifiers translated */
		for (; specification <= cursor; specification++)
		{
			if (specification[0] == 'I' && specification[1] == '6' && specification[2] == '4')
			{
				if (!prepared_append(preparation, &length, "ll", 2))
					goto give_up;
				specification += 2;
			}
			else if (specification[0] == 'I' && specification[1] == '3' && specification[2] == '2')
				specification += 2;
			else if (!prepared_append(preparation, &length, specification, 1))
				goto give_up;
		}
	}
	va_end(in);
	preparation->format[length] = 0;
	*prepared_format = preparation->format;
	return (va_list)words;
give_up:
	va_end(in);
	*prepared_format = translate_format(format, preparation->format, sizeof(preparation->format));
	return arguments;
}

int halo_xbox_vsnprintf(char *buffer, size_t count, const char *format, va_list arguments)
{
	struct printf_preparation preparation;
	va_list prepared = prepare_arguments(format, arguments, &preparation, &format);
	int result = vsnprintf(buffer, count, format, prepared);

	/* PDCLib's vsnprintf leaves a result cut short in a %s, a %c or the
	format's text without its end (the 9x build's own vsnprintf ends it,
	port/win9x/pdclib/functions/stdio/vsnprintf.c; nxdk's does not) */
	if (count > 0 && result >= 0 && (size_t)result >= count)
		buffer[count - 1] = 0;
	return result;
}

int halo_xbox_snprintf(char *buffer, size_t count, const char *format, ...)
{
	va_list arguments;
	int result;

	va_start(arguments, format);
	result = halo_xbox_vsnprintf(buffer, count, format, arguments);
	va_end(arguments);
	return result;
}

int halo_xbox_vsprintf(char *buffer, const char *format, va_list arguments)
{
	struct printf_preparation preparation;
	va_list prepared = prepare_arguments(format, arguments, &preparation, &format);

	return vsprintf(buffer, format, prepared);
}

int halo_xbox_sprintf(char *buffer, const char *format, ...)
{
	va_list arguments;
	int result;

	va_start(arguments, format);
	result = halo_xbox_vsprintf(buffer, format, arguments);
	va_end(arguments);
	return result;
}

/* fprintf to a stream through the same preparation (the Xbox build's
halo_xbox_vfprintf, msvc_crt.c, and the 9x build's sinks, w9x_crt.c) */
int halo_msvc_vfprintf(FILE *stream, const char *format, va_list arguments)
{
	struct printf_preparation preparation;
	va_list prepared = prepare_arguments(format, arguments, &preparation, &format);

	return vfprintf(stream, format, prepared);
}
