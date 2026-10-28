/*
W9X_CRT.C

The 9x build's C runtime beyond nxdk's PDCLib (port/win9x/README.md): the
Microsoft C runtime's extras the game and the platform layer call, the
floating-point control words, time, the environment, abort and signals,
assert, the exact fmod, and the game's printf sinks. The low-level file
functions are in w9x_crt_io.c, the start-up code in w9x_start.c.

Everything here is KERNEL32 of Windows 95 / 98: the UCRT this replaces
needs Windows Vista, and its narrow file functions go through the W
functions, which 9x only stubs.
*/

#include <windows.h>

#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "w9x_runtime.h"

/* dlmalloc's (w9x_malloc.c) */
size_t dlmalloc_usable_size(const void *pointer);

/* ---------- start-up */

static DWORD start_tick;

void w9x_crt_start(void)
{
	start_tick = GetTickCount();
}

/* ---------- strings */

char *strdup(const char *string)
{
	size_t size = strlen(string) + 1;
	char *copy = malloc(size);

	if (copy)
		memcpy(copy, string, size);
	return copy;
}

/* (the C locale's case: ASCII, as MSVC's in the "C" locale) */
static int lower(int c)
{
	return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

static int upper(int c)
{
	return c >= 'a' && c <= 'z' ? c - ('a' - 'A') : c;
}

int _stricmp(const char *a, const char *b)
{
	int ca, cb;

	do
	{
		ca = lower((unsigned char)*a++);
		cb = lower((unsigned char)*b++);
	} while (ca && ca == cb);
	return ca - cb;
}

int _strnicmp(const char *a, const char *b, size_t count)
{
	int ca = 0, cb = 0;

	while (count--)
	{
		ca = lower((unsigned char)*a++);
		cb = lower((unsigned char)*b++);
		if (!ca || ca != cb)
			break;
	}
	return ca - cb;
}

char *_strlwr(char *string)
{
	char *cursor;

	for (cursor = string; *cursor; cursor++)
		*cursor = (char)lower((unsigned char)*cursor);
	return string;
}

char *_strupr(char *string)
{
	char *cursor;

	for (cursor = string; *cursor; cursor++)
		*cursor = (char)upper((unsigned char)*cursor);
	return string;
}

size_t wcslen(const wchar_t *string)
{
	const wchar_t *end = string;

	while (*end)
		end++;
	return (size_t)(end - string);
}

static char *unsigned_to_string(unsigned long value, char *string, int radix, int negative)
{
	char digits[40];
	int count = 0;
	char *cursor = string;

	if (radix < 2 || radix > 36)
		radix = 10;
	do
	{
		int digit = (int)(value % (unsigned long)radix);

		digits[count++] = (char)(digit < 10 ? '0' + digit : 'a' + digit - 10);
		value /= (unsigned long)radix;
	} while (value);
	if (negative)
		*cursor++ = '-';
	while (count)
		*cursor++ = digits[--count];
	*cursor = 0;
	return string;
}

char *_ltoa(long value, char *string, int radix)
{
	if (radix == 10 && value < 0)
		return unsigned_to_string((unsigned long)(-(value + 1)) + 1, string, radix, 1);
	return unsigned_to_string((unsigned long)value, string, radix, 0);
}

char *_itoa(int value, char *string, int radix)
{
	return _ltoa(value, string, radix);
}

char *_ultoa(unsigned long value, char *string, int radix)
{
	return unsigned_to_string(value, string, radix, 0);
}

/* ---------- paths (port/xbox/src/msvc_crt.c's) */

void _splitpath(const char *path, char *drive, char *directory, char *name, char *extension)
{
	const char *cursor = path;
	const char *directory_end;
	const char *extension_start;
	const char *scan;

	if (drive)
		drive[0] = 0;
	if (cursor[0] && cursor[1] == ':')
	{
		if (drive)
		{
			drive[0] = cursor[0];
			drive[1] = ':';
			drive[2] = 0;
		}
		cursor += 2;
	}
	directory_end = cursor;
	for (scan = cursor; *scan; scan++)
	{
		if (*scan == '\\' || *scan == '/')
			directory_end = scan + 1;
	}
	if (directory)
	{
		memcpy(directory, cursor, (size_t)(directory_end - cursor));
		directory[directory_end - cursor] = 0;
	}
	extension_start = strrchr(directory_end, '.');
	if (!extension_start)
		extension_start = directory_end + strlen(directory_end);
	if (name)
	{
		memcpy(name, directory_end, (size_t)(extension_start - directory_end));
		name[extension_start - directory_end] = 0;
	}
	if (extension)
		strcpy(extension, extension_start);
}

void _makepath(char *path, const char *drive, const char *directory, const char *name, const char *extension)
{
	path[0] = 0;
	if (drive && drive[0])
	{
		strcat(path, drive);
		if (drive[strlen(drive) - 1] != ':')
			strcat(path, ":");
	}
	if (directory && directory[0])
	{
		strcat(path, directory);
		if (directory[strlen(directory) - 1] != '\\' && directory[strlen(directory) - 1] != '/')
			strcat(path, "\\");
	}
	if (name)
		strcat(path, name);
	if (extension && extension[0])
	{
		if (extension[0] != '.')
			strcat(path, ".");
		strcat(path, extension);
	}
}

/* ---------- memory */

size_t _msize(void *pointer)
{
	return pointer ? dlmalloc_usable_size(pointer) : (size_t)-1;
}

/* ---------- floating-point control

MSVC's control word (<float.h>) is translated to the x87's and mirrored
into MXCSR, the SSE unit's, which does the game's single precision: the
exception masks and the rounding. The UCRT does the same on processors with
SSE2; here it is the Pentium III's SSE. MXCSR's DAZ (bit 6) and FTZ (bit
15) stay clear whatever is asked: a Pentium III without DAZ faults when it
is set (#GP on ldmxcsr), and with denormals kept the results are the
other ports'. */

/* the state every thread computes in (w9x_runtime.h) */
#define DEFAULT_X87_CONTROL 0x027F /* 53 bits, to nearest, all masked */
#define DEFAULT_MXCSR 0x1F80 /* to nearest, all masked, no DAZ or FTZ */

/* MXCSR's bits other than the flags (0-5), DAZ (6) and FTZ (15): the
masks (7-12) and the rounding (13-14) */
#define MXCSR_CONTROL_BITS 0x7F80u

void w9x_fpu_reset(void)
{
	unsigned short control = DEFAULT_X87_CONTROL;
	unsigned int mxcsr = DEFAULT_MXCSR;

	__asm__ __volatile__("fninit\n\tfldcw %0" : : "m"(control));
#ifdef __SSE__
	__asm__ __volatile__("ldmxcsr %0" : : "m"(mxcsr));
#else
	/* (the DOS and Windows 95 builds, for any Pentium: no SSE unit) */
	(void)mxcsr;
#endif
}

static unsigned int control_word_to_msvc(unsigned short word)
{
	unsigned int value = 0;

	if (word & 0x01) value |= _EM_INVALID;
	if (word & 0x02) value |= _EM_DENORMAL;
	if (word & 0x04) value |= _EM_ZERODIVIDE;
	if (word & 0x08) value |= _EM_OVERFLOW;
	if (word & 0x10) value |= _EM_UNDERFLOW;
	if (word & 0x20) value |= _EM_INEXACT;
	switch ((word >> 10) & 3)
	{
	case 1: value |= _RC_DOWN; break;
	case 2: value |= _RC_UP; break;
	case 3: value |= _RC_CHOP; break;
	default: break;
	}
	switch ((word >> 8) & 3)
	{
	case 0: value |= _PC_24; break;
	case 2: value |= _PC_53; break;
	default: value |= _PC_64; break;
	}
	/* (the infinity control bit, which the x87 ignores since the 387;
	MSVC reports it set) */
	if (word & 0x1000) value |= _IC_AFFINE;
	return value;
}

static unsigned short msvc_to_control_word(unsigned int value, unsigned short word)
{
	word &= (unsigned short)~0x1f3f;
	if (value & _EM_INVALID) word |= 0x01;
	if (value & _EM_DENORMAL) word |= 0x02;
	if (value & _EM_ZERODIVIDE) word |= 0x04;
	if (value & _EM_OVERFLOW) word |= 0x08;
	if (value & _EM_UNDERFLOW) word |= 0x10;
	if (value & _EM_INEXACT) word |= 0x20;
	switch (value & _MCW_RC)
	{
	case _RC_DOWN: word |= 1 << 10; break;
	case _RC_UP: word |= 2 << 10; break;
	case _RC_CHOP: word |= 3 << 10; break;
	default: break;
	}
	switch (value & _MCW_PC)
	{
	case _PC_24: break;
	case _PC_53: word |= 2 << 8; break;
	default: word |= 3 << 8; break;
	}
	if (value & _IC_AFFINE) word |= 0x1000;
	return word;
}

static unsigned int msvc_to_mxcsr(unsigned int value, unsigned int mxcsr)
{
	/* (the flags kept; DAZ and FTZ cleared with the rest) */
	mxcsr &= 0x3Fu;
	if (value & _EM_INVALID) mxcsr |= 1u << 7;
	if (value & _EM_DENORMAL) mxcsr |= 1u << 8;
	if (value & _EM_ZERODIVIDE) mxcsr |= 1u << 9;
	if (value & _EM_OVERFLOW) mxcsr |= 1u << 10;
	if (value & _EM_UNDERFLOW) mxcsr |= 1u << 11;
	if (value & _EM_INEXACT) mxcsr |= 1u << 12;
	switch (value & _MCW_RC)
	{
	case _RC_DOWN: mxcsr |= 1u << 13; break;
	case _RC_UP: mxcsr |= 2u << 13; break;
	case _RC_CHOP: mxcsr |= 3u << 13; break;
	default: break;
	}
	return mxcsr & (MXCSR_CONTROL_BITS | 0x3Fu);
}

static unsigned short read_control_word(void)
{
	unsigned short word;

	__asm__ __volatile__("fnstcw %0" : "=m"(word));
	return word;
}

static void write_control_word(unsigned short word)
{
	__asm__ __volatile__("fldcw %0" : : "m"(word));
}

static unsigned int read_mxcsr(void)
{
#ifdef __SSE__
	unsigned int mxcsr;

	__asm__ __volatile__("stmxcsr %0" : "=m"(mxcsr));
	return mxcsr;
#else
	return DEFAULT_MXCSR;
#endif
}

static void write_mxcsr(unsigned int mxcsr)
{
#ifdef __SSE__
	/* never DAZ or FTZ (see above) */
	mxcsr &= ~0x8040u;
	__asm__ __volatile__("ldmxcsr %0" : : "m"(mxcsr));
#else
	(void)mxcsr;
#endif
}

unsigned int _control87(unsigned int new_value, unsigned int mask)
{
	unsigned short word = read_control_word();
	unsigned int current = control_word_to_msvc(word);

	/* (the denormal control, _MCW_DN, is SSE2's and is left alone) */
	mask &= _MCW_EM | _MCW_RC | _MCW_PC | _MCW_IC;
	if (mask)
	{
		unsigned int wanted = (current & ~mask) | (new_value & mask);

		write_control_word(msvc_to_control_word(wanted, word));
		write_mxcsr(msvc_to_mxcsr(wanted, read_mxcsr()));
		current = control_word_to_msvc(read_control_word());
	}
	return current;
}

unsigned int _controlfp(unsigned int new_value, unsigned int mask)
{
	/* (MSVC's _controlfp leaves the denormal exception mask alone) */
	return _control87(new_value, mask & ~_EM_DENORMAL);
}

static unsigned int status_to_msvc(unsigned int status)
{
	unsigned int value = 0;

	if (status & 0x01) value |= _SW_INVALID;
	if (status & 0x02) value |= _SW_DENORMAL;
	if (status & 0x04) value |= _SW_ZERODIVIDE;
	if (status & 0x08) value |= _SW_OVERFLOW;
	if (status & 0x10) value |= _SW_UNDERFLOW;
	if (status & 0x20) value |= _SW_INEXACT;
	return value;
}

unsigned int _statusfp(void)
{
	unsigned short word;

	__asm__ __volatile__("fnstsw %0" : "=m"(word));
	return status_to_msvc(word | (read_mxcsr() & 0x3F));
}

unsigned int _clearfp(void)
{
	unsigned int value = _statusfp();

	__asm__ __volatile__("fnclex");
	write_mxcsr(read_mxcsr() & ~0x3Fu);
	return value;
}

int _isnan(double value)
{
	return value != value;
}

int _finite(double value)
{
	return value == value && value - value == 0.0;
}

double _hypot(double x, double y)
{
	return sqrt(x * x + y * y);
}

double _copysign(double x, double y)
{
	return __builtin_copysign(x, y);
}

/* ---------- fmod, exactly

The remainder of x / y is exact in IEEE arithmetic, and the game takes it
in its simulation (object and animation timing, vehicles, periodic
functions), which runs in lockstep on every machine of a game: it must be
the same bits as the other ports' C runtimes give. The x87's fprem (nxdk's
fmod) is exact only while the exponents differ by less than 64. This is
musl's (src/math/fmod.c and fmodf.c; MIT licence, as port/third_party/
musl-math): long division on the significands, in integers. */

double fmod(double x, double y)
{
	union
	{
		double f;
		uint64_t i;
	} ux, uy;
	int ex, ey, sx;
	uint64_t i, uxi;

	ux.f = x;
	uy.f = y;
	ex = (int)(ux.i >> 52 & 0x7ff);
	ey = (int)(uy.i >> 52 & 0x7ff);
	sx = (int)(ux.i >> 63);
	uxi = ux.i;
	if (uy.i << 1 == 0 || y != y || ex == 0x7ff)
		return (x * y) / (x * y);
	if (uxi << 1 <= uy.i << 1)
	{
		if (uxi << 1 == uy.i << 1)
			return 0 * x;
		return x;
	}
	/* normalize x and y */
	if (!ex)
	{
		for (i = uxi << 12; i >> 63 == 0; ex--, i <<= 1)
			;
		uxi <<= -ex + 1;
	}
	else
	{
		uxi &= -1ULL >> 12;
		uxi |= 1ULL << 52;
	}
	if (!ey)
	{
		for (i = uy.i << 12; i >> 63 == 0; ey--, i <<= 1)
			;
		uy.i <<= -ey + 1;
	}
	else
	{
		uy.i &= -1ULL >> 12;
		uy.i |= 1ULL << 52;
	}
	/* x mod y */
	for (; ex > ey; ex--)
	{
		i = uxi - uy.i;
		if (i >> 63 == 0)
		{
			if (i == 0)
				return 0 * x;
			uxi = i;
		}
		uxi <<= 1;
	}
	i = uxi - uy.i;
	if (i >> 63 == 0)
	{
		if (i == 0)
			return 0 * x;
		uxi = i;
	}
	for (; uxi >> 52 == 0; uxi <<= 1, ex--)
		;
	/* scale result */
	if (ex > 0)
	{
		uxi -= 1ULL << 52;
		uxi |= (uint64_t)ex << 52;
	}
	else
		uxi >>= -ex + 1;
	uxi |= (uint64_t)sx << 63;
	ux.i = uxi;
	return ux.f;
}

float fmodf(float x, float y)
{
	union
	{
		float f;
		uint32_t i;
	} ux, uy;
	int ex, ey;
	uint32_t sx, i, uxi;

	ux.f = x;
	uy.f = y;
	ex = (int)(ux.i >> 23 & 0xff);
	ey = (int)(uy.i >> 23 & 0xff);
	sx = ux.i & 0x80000000;
	uxi = ux.i;
	if (uy.i << 1 == 0 || y != y || ex == 0xff)
		return (x * y) / (x * y);
	if (uxi << 1 <= uy.i << 1)
	{
		if (uxi << 1 == uy.i << 1)
			return 0 * x;
		return x;
	}
	/* normalize x and y */
	if (!ex)
	{
		for (i = uxi << 9; i >> 31 == 0; ex--, i <<= 1)
			;
		uxi <<= -ex + 1;
	}
	else
	{
		uxi &= -1U >> 9;
		uxi |= 1U << 23;
	}
	if (!ey)
	{
		for (i = uy.i << 9; i >> 31 == 0; ey--, i <<= 1)
			;
		uy.i <<= -ey + 1;
	}
	else
	{
		uy.i &= -1U >> 9;
		uy.i |= 1U << 23;
	}
	/* x mod y */
	for (; ex > ey; ex--)
	{
		i = uxi - uy.i;
		if (i >> 31 == 0)
		{
			if (i == 0)
				return 0 * x;
			uxi = i;
		}
		uxi <<= 1;
	}
	i = uxi - uy.i;
	if (i >> 31 == 0)
	{
		if (i == 0)
			return 0 * x;
		uxi = i;
	}
	for (; uxi >> 23 == 0; uxi <<= 1, ex--)
		;
	/* scale result up */
	if (ex > 0)
	{
		uxi -= 1U << 23;
		uxi |= (uint32_t)ex << 23;
	}
	else
		uxi >>= -ex + 1;
	uxi |= sx;
	ux.i = uxi;
	return ux.f;
}

long double fmodl(long double x, long double y)
{
	/* (a long double is a double on this target) */
	return fmod((double)x, (double)y);
}

/* ---------- time

time_t is 32 bits, as in the Windows build (_USE_32BIT_TIME_T) and the
Xbox's C runtime. Local time follows the system's time zone the way the
UCRT's does: the zone's bias, and its daylight saving rules
(GetTimeZoneInformation, read once) applied to the year of the time asked
about, not only today's (FileTimeToLocalFileTime applies today's bias to
any date). */

#define EPOCH_TICKS 116444736000000000ULL /* 1601 to 1970, in 100 ns */

static unsigned long long system_ticks(void)
{
	FILETIME now;

	GetSystemTimeAsFileTime(&now);
	return ((unsigned long long)now.dwHighDateTime << 32) | now.dwLowDateTime;
}

time_t time(time_t *timer)
{
	time_t result = (time_t)((system_ticks() - EPOCH_TICKS) / 10000000ULL);

	if (timer)
		*timer = result;
	return result;
}

int timespec_get(struct timespec *ts, int base)
{
	unsigned long long ticks;

	if (base != TIME_UTC)
		return 0;
	ticks = system_ticks() - EPOCH_TICKS;
	ts->tv_sec = (time_t)(ticks / 10000000ULL);
	ts->tv_nsec = (long)(ticks % 10000000ULL) * 100;
	return base;
}

clock_t clock(void)
{
	/* milliseconds since the start (CLOCKS_PER_SEC is 1000), as MSVC's
	clock is the time since the process started */
	return (clock_t)(GetTickCount() - start_tick);
}

static int is_leap_year(long year)
{
	return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static const short days_before_month[2][13] = {
	{ 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365 },
	{ 0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366 },
};

/* days from 1970-01-01 to the date (month 1-12) */
static long long days_from_date(long year, int month, int day)
{
	long long days = 365LL * (year - 1970) + day - 1 + days_before_month[is_leap_year(year)][month - 1];
	long y;

	/* the leap days of the years between */
	for (y = 1970; y < year; y++)
		days += is_leap_year(y);
	for (y = year; y < 1970; y++)
		days -= is_leap_year(y);
	return days;
}

/* the date of a day counted from 1970-01-01 */
static void date_from_days(long long days, long *year, int *month, int *day, int *day_of_year)
{
	long y = 1970;
	int leap, m;

	while (days < 0)
	{
		y--;
		days += 365 + is_leap_year(y);
	}
	while (days >= 365 + is_leap_year(y))
	{
		days -= 365 + is_leap_year(y);
		y++;
	}
	leap = is_leap_year(y);
	for (m = 1; m < 12 && days >= days_before_month[leap][m]; m++)
		;
	*year = y;
	*month = m;
	*day = (int)(days - days_before_month[leap][m - 1]) + 1;
	*day_of_year = (int)days;
}

/* the moment (seconds from 1970, in the rule's own local time) a daylight
saving rule of the zone takes effect in year: a date of that year, or
"the nth (5: the last) weekday of the month" */
static long long rule_moment(const SYSTEMTIME *rule, long year)
{
	int day = rule->wDay;

	if (rule->wYear == 0)
	{
		int days_in_month = days_before_month[is_leap_year(year)][rule->wMonth] -
			days_before_month[is_leap_year(year)][rule->wMonth - 1];
		/* 1970-01-01 was a Thursday (4) */
		int first_weekday = (int)((days_from_date(year, rule->wMonth, 1) % 7 + 7 + 4) % 7);

		day = 1 + (rule->wDayOfWeek - first_weekday + 7) % 7 + (rule->wDay - 1) * 7;
		while (day > days_in_month)
			day -= 7;
	}
	return days_from_date(year, rule->wMonth, day) * 86400 + rule->wHour * 3600 + rule->wMinute * 60 +
		rule->wSecond;
}

static TIME_ZONE_INFORMATION time_zone;
static volatile LONG time_zone_state; /* 0 unread, 1 being read, 2 read, 3 none */

static const TIME_ZONE_INFORMATION *read_time_zone(void)
{
#ifdef _KERNEL32_
	/* (DOS's, asked each time: its zone is learnt once the network is up,
	dos_kernel32.c) */
	GetTimeZoneInformation(&time_zone);
	return &time_zone;
#endif
	if (time_zone_state < 2)
	{
		if (InterlockedCompareExchange(&time_zone_state, 1, 0) == 0)
		{
			LONG state = GetTimeZoneInformation(&time_zone) == TIME_ZONE_ID_INVALID ? 3 : 2;

			InterlockedExchange(&time_zone_state, state);
		}
		while (time_zone_state < 2)
			Sleep(1);
	}
	return time_zone_state == 2 ? &time_zone : NULL;
}

struct tm *localtime_r(const time_t *timer, struct tm *result)
{
	const TIME_ZONE_INFORMATION *zone;
	long long utc, standard, daylight, local;
	long year;
	int month, day, day_of_year, daylight_saving = 0;

	/* (MSVC's 32-bit local time ends a day before time_t does, at
	_MAX__TIME32_T, so that every time zone's local time fits) */
	if (!timer || *timer < 0 || *timer > 0x7FFFD27FL)
	{
		errno = EINVAL;
		return NULL;
	}
	utc = (long long)*timer;
	zone = read_time_zone();
	local = utc;
	if (zone)
	{
		standard = utc - (long long)(zone->Bias + zone->StandardBias) * 60;
		local = standard;
		if (zone->DaylightDate.wMonth && zone->StandardDate.wMonth)
		{
			long long start, end;

			daylight = utc - (long long)(zone->Bias + zone->DaylightBias) * 60;
			date_from_days(standard >= 0 ? standard / 86400 : (standard - 86399) / 86400, &year, &month, &day,
				&day_of_year);
			/* the rules are local times: the start in standard time, the
			end in daylight saving time; south of the equator the period
			spans the new year */
			start = rule_moment(&zone->DaylightDate, year);
			end = rule_moment(&zone->StandardDate, year);
			if (start < end)
				daylight_saving = standard >= start && daylight < end;
			else
				daylight_saving = standard >= start || daylight < end;
			if (daylight_saving)
				local = daylight;
		}
	}
	date_from_days(local >= 0 ? local / 86400 : (local - 86399) / 86400, &year, &month, &day, &day_of_year);
	{
		long long seconds = local - days_from_date(year, month, day) * 86400;

		result->tm_hour = (int)(seconds / 3600);
		result->tm_min = (int)(seconds / 60 % 60);
		result->tm_sec = (int)(seconds % 60);
	}
	result->tm_mday = day;
	result->tm_mon = month - 1;
	result->tm_year = (int)(year - 1900);
	result->tm_yday = day_of_year;
	result->tm_wday = (int)((days_from_date(year, month, day) % 7 + 7 + 4) % 7);
	result->tm_isdst = daylight_saving;
	return result;
}

struct tm *localtime(const time_t *timer)
{
	static __thread struct tm result;

	return localtime_r(timer, &result);
}

/* PDCLib's asctime and ctime return NULL; these are the C standard's */
char *asctime(const struct tm *t)
{
	static const char days[7][4] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
	static const char months[12][4] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct",
		"Nov", "Dec" };
	static __thread char text[32];

	if (!t || t->tm_wday < 0 || t->tm_wday > 6 || t->tm_mon < 0 || t->tm_mon > 11)
	{
		errno = EINVAL;
		return NULL;
	}
	snprintf(text, sizeof(text), "%.3s %.3s%3d %.2d:%.2d:%.2d %d\n", days[t->tm_wday], months[t->tm_mon], t->tm_mday,
		t->tm_hour, t->tm_min, t->tm_sec, 1900 + t->tm_year);
	return text;
}

char *ctime(const time_t *timer)
{
	struct tm *local = localtime(timer);

	return local ? asctime(local) : NULL;
}

/* ---------- the environment */

/* getenv's answers live as long as the program, as the environment
block's would: one copy for each name and value asked for */
#define MAXIMUM_ENVIRONMENT_NAMES 64

struct environment_entry
{
	char *name;
	char *value;
};

static struct environment_entry environment[MAXIMUM_ENVIRONMENT_NAMES];
static volatile LONG environment_lock;

char *getenv(const char *name)
{
	char stack_value[512];
	char *value = stack_value;
	DWORD length;
	char *answer = NULL;
	int index;

	if (!name || !*name)
		return NULL;
	length = GetEnvironmentVariableA(name, stack_value, sizeof(stack_value));
	if (length >= sizeof(stack_value))
	{
		value = malloc(length + 1);
		if (!value)
			return NULL;
		length = GetEnvironmentVariableA(name, value, length + 1);
	}
	if (length == 0)
	{
		if (value != stack_value)
			free(value);
		return NULL;
	}
	while (InterlockedExchange(&environment_lock, 1))
		Sleep(1);
	for (index = 0; index < MAXIMUM_ENVIRONMENT_NAMES && environment[index].name; index++)
	{
		if (!_stricmp(environment[index].name, name) && !strcmp(environment[index].value, value))
		{
			answer = environment[index].value;
			break;
		}
	}
	if (!answer && index < MAXIMUM_ENVIRONMENT_NAMES)
	{
		/* (a name whose value changed gets a slot of its own: the old
		answer stays valid for whoever holds it) */
		char *copied_name = strdup(name);
		char *copied_value = strdup(value);

		if (copied_name && copied_value)
		{
			environment[index].name = copied_name;
			environment[index].value = copied_value;
			answer = copied_value;
		}
		else
		{
			free(copied_name);
			free(copied_value);
		}
	}
	InterlockedExchange(&environment_lock, 0);
	if (value != stack_value)
		free(value);
	return answer;
}

int system(const char *command)
{
	/* no command interpreter is run: none is there (as MSVC's answer
	for a NULL command when COMSPEC is unset) */
	(void)command;
	errno = ENOENT;
	return command ? -1 : 0;
}

/* ---------- abort, signals and assert */

static void report(const char *text)
{
	DWORD written;
	HANDLE error = GetStdHandle(STD_ERROR_HANDLE);

	OutputDebugStringA(text);
	if (error && error != INVALID_HANDLE_VALUE)
		WriteFile(error, text, (DWORD)strlen(text), &written, NULL);
}

void (*_PDCLIB_sigabrt)(int) = SIG_DFL;
void (*_PDCLIB_sigfpe)(int) = SIG_DFL;
void (*_PDCLIB_sigill)(int) = SIG_DFL;
void (*_PDCLIB_sigint)(int) = SIG_DFL;
void (*_PDCLIB_sigsegv)(int) = SIG_DFL;
void (*_PDCLIB_sigterm)(int) = SIG_DFL;

static void (**signal_slot(int number))(int)
{
	switch (number)
	{
	case SIGABRT: return &_PDCLIB_sigabrt;
	case SIGFPE: return &_PDCLIB_sigfpe;
	case SIGILL: return &_PDCLIB_sigill;
	case SIGINT: return &_PDCLIB_sigint;
	case SIGSEGV: return &_PDCLIB_sigsegv;
	case SIGTERM: return &_PDCLIB_sigterm;
	default: return NULL;
	}
}

void (*signal(int number, void (*handler)(int)))(int)
{
	void (**slot)(int) = signal_slot(number);
	void (*previous)(int);

	if (!slot || handler == SIG_ERR)
	{
		errno = EINVAL;
		return SIG_ERR;
	}
	previous = *slot;
	*slot = handler;
	return previous;
}

int raise(int number)
{
	void (**slot)(int) = signal_slot(number);
	void (*handler)(int);

	if (!slot)
		return -1;
	handler = *slot;
	if (handler == SIG_IGN)
		return 0;
	if (handler == SIG_DFL)
	{
		report(number == SIGABRT ? "abnormal program termination\n" : "terminated by a signal\n");
		ExitProcess(3);
	}
	*slot = SIG_DFL;
	handler(number);
	return 0;
}

void abort(void)
{
	raise(SIGABRT);
	/* (a handler that returned) */
	ExitProcess(3);
}

void w9x_assert(const char *expression, const char *file_name, const char *function_name, unsigned long line)
{
	char text[512];

	snprintf(text, sizeof(text), "Assertion failed: %s, file %s, line %lu (%s)\n", expression, file_name, line,
		function_name);
	report(text);
	abort();
}

/* ---------- MSVC's errno */

int *_errno(void)
{
	return &errno;
}

/* ---------- the game's printf sinks (crt/stdio.h of the Xbox build maps
the game's printf family here; port/xbox/src/msvc_printf.c formats) */

FILE *halo_xbox_fopen(const char *path, const char *mode)
{
	/* (the 9x build's crt/stdio.h sends the game's fopen to the Windows
	build's path translation instead; this is for anything else) */
	return fopen(path, mode);
}

int halo_xbox_vfprintf(FILE *stream, const char *format, va_list arguments)
{
	if (!stream)
	{
		errno = EINVAL;
		return -1;
	}
	return halo_msvc_vfprintf(stream, format, arguments);
}

int halo_xbox_fprintf(FILE *stream, const char *format, ...)
{
	va_list arguments;
	int result;

	va_start(arguments, format);
	result = halo_xbox_vfprintf(stream, format, arguments);
	va_end(arguments);
	return result;
}

int halo_xbox_vprintf(const char *format, va_list arguments)
{
	return halo_msvc_vfprintf(stdout, format, arguments);
}

int halo_xbox_printf(const char *format, ...)
{
	va_list arguments;
	int result;

	va_start(arguments, format);
	result = halo_msvc_vfprintf(stdout, format, arguments);
	va_end(arguments);
	return result;
}

int halo_xbox_fputs(const char *string, FILE *stream)
{
	if (!stream)
	{
		errno = EINVAL;
		return EOF;
	}
	return fputs(string, stream);
}
