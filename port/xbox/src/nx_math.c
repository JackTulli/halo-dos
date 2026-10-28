/*
NX_MATH.C

The C library's math functions nxdk only stubs: its PDCLib objects for
these assert (and stop the console) when called. The game reaches lrintf
from its HUD; the rest are here so that nothing else can stop it the same
way. Each family defines the same names as the object it replaces, so the
linker never pulls that object in. The x87 does the rounding; the others
are the plain formulas, precise enough for a game.
*/

#include <math.h>
#include <stdint.h>

/* ---------- rounding to integers (the x87's current mode: to nearest) */

static long round_to_long(double value)
{
	long result;

	__asm__ __volatile__("fistpl %0" : "=m"(result) : "t"(value) : "st");
	return result;
}

static long long round_to_long_long(double value)
{
	long long result;

	__asm__ __volatile__("fistpll %0" : "=m"(result) : "t"(value) : "st");
	return result;
}

long lrint(double value) { return round_to_long(value); }
long lrintf(float value) { return round_to_long(value); }
long lrintl(long double value) { return round_to_long((double)value); }
long long llrint(double value) { return round_to_long_long(value); }
long long llrintf(float value) { return round_to_long_long(value); }
long long llrintl(long double value) { return round_to_long_long((double)value); }

static double round_half_away(double value)
{
	return value < 0.0 ? ceil(value - 0.5) : floor(value + 0.5);
}

long lround(double value) { return (long)round_half_away(value); }
long lroundf(float value) { return (long)round_half_away(value); }
long lroundl(long double value) { return (long)round_half_away((double)value); }
long long llround(double value) { return (long long)round_half_away(value); }
long long llroundf(float value) { return (long long)round_half_away(value); }
long long llroundl(long double value) { return (long long)round_half_away((double)value); }

/* ---------- scaling by powers of two */

double scalbn(double value, int exponent) { return ldexp(value, exponent); }
float scalbnf(float value, int exponent) { return (float)ldexp(value, exponent); }
long double scalbnl(long double value, int exponent) { return ldexp((double)value, exponent); }
double scalbln(double value, long exponent) { return ldexp(value, exponent > 2048 ? 2048 : exponent < -2048 ? -2048 : (int)exponent); }
float scalblnf(float value, long exponent) { return (float)scalbln(value, exponent); }
long double scalblnl(long double value, long exponent) { return scalbln((double)value, exponent); }

int ilogb(double value)
{
	int exponent;

	if (value == 0.0)
		return -2147483647 - 1; /* FP_ILOGB0 */
	frexp(value, &exponent);
	return exponent - 1;
}
int ilogbf(float value) { return ilogb(value); }
int ilogbl(long double value) { return ilogb((double)value); }

double logb(double value)
{
	int exponent;

	if (value == 0.0)
		return -HUGE_VAL;
	frexp(value, &exponent);
	return (double)(exponent - 1);
}
float logbf(float value) { return (float)logb(value); }
long double logbl(long double value) { return logb((double)value); }

/* ---------- exponentials and logarithms */

#define LN2 0.69314718055994530942

double log2(double value) { return log(value) / LN2; }
float log2f(float value) { return (float)log2(value); }
long double log2l(long double value) { return log2((double)value); }
double exp2(double value) { return exp(value * LN2); }
float exp2f(float value) { return (float)exp2(value); }
long double exp2l(long double value) { return exp2((double)value); }
double log1p(double value) { return log(1.0 + value); }
float log1pf(float value) { return (float)log1p(value); }
long double log1pl(long double value) { return log1p((double)value); }
double expm1(double value) { return exp(value) - 1.0; }
float expm1f(float value) { return (float)expm1(value); }
long double expm1l(long double value) { return expm1((double)value); }

double cbrt(double value)
{
	double magnitude = pow(fabs(value), 1.0 / 3.0);

	return value < 0.0 ? -magnitude : magnitude;
}
float cbrtf(float value) { return (float)cbrt(value); }
long double cbrtl(long double value) { return cbrt((double)value); }

/* ---------- hyperbolic inverses */

double asinh(double value) { return log(value + sqrt(value * value + 1.0)); }
float asinhf(float value) { return (float)asinh(value); }
long double asinhl(long double value) { return asinh((double)value); }
double acosh(double value) { return log(value + sqrt(value * value - 1.0)); }
float acoshf(float value) { return (float)acosh(value); }
long double acoshl(long double value) { return acosh((double)value); }
double atanh(double value) { return 0.5 * log((1.0 + value) / (1.0 - value)); }
float atanhf(float value) { return (float)atanh(value); }
long double atanhl(long double value) { return atanh((double)value); }

/* ---------- the rest */

double fdim(double a, double b) { return a > b ? a - b : 0.0; }
float fdimf(float a, float b) { return a > b ? a - b : 0.0f; }
long double fdiml(long double a, long double b) { return a > b ? a - b : 0.0L; }

double fma(double a, double b, double c) { return a * b + c; }
float fmaf(float a, float b, float c) { return a * b + c; }
long double fmal(long double a, long double b, long double c) { return a * b + c; }

double nan(const char *tag) { (void)tag; return __builtin_nan(""); }
float nanf(const char *tag) { (void)tag; return __builtin_nanf(""); }
long double nanl(const char *tag) { (void)tag; return __builtin_nanl(""); }

double remquo(double x, double y, int *quotient)
{
	double q = x / y;
	double n = round_half_away(q);

	*quotient = (int)n;
	return x - n * y;
}
float remquof(float x, float y, int *quotient) { return (float)remquo(x, y, quotient); }
long double remquol(long double x, long double y, int *quotient) { return remquo((double)x, (double)y, quotient); }

double nextafter(double from, double to)
{
	union { double value; uint64_t bits; } u;

	if (from == to)
		return to;
	if (from != from || to != to)
		return from + to;
	u.value = from;
	if (from == 0.0)
	{
		u.bits = 1;
		return to > 0.0 ? u.value : -u.value;
	}
	if ((from < to) == (from > 0.0))
		u.bits++;
	else
		u.bits--;
	return u.value;
}

float nextafterf(float from, float to)
{
	union { float value; uint32_t bits; } u;

	if (from == to)
		return to;
	if (from != from || to != to)
		return from + to;
	u.value = from;
	if (from == 0.0f)
	{
		u.bits = 1;
		return to > 0.0f ? u.value : -u.value;
	}
	if ((from < to) == (from > 0.0f))
		u.bits++;
	else
		u.bits--;
	return u.value;
}

long double nextafterl(long double from, long double to) { return nextafter((double)from, (double)to); }

/* the error function (Abramowitz and Stegun 7.1.26) and the gamma
function (Stirling's series), for completeness */
double erf(double value)
{
	double sign = value < 0.0 ? -1.0 : 1.0;
	double x = fabs(value);
	double t = 1.0 / (1.0 + 0.3275911 * x);
	double y = 1.0 - (((((1.061405429 * t - 1.453152027) * t) + 1.421413741) * t - 0.284496736) * t + 0.254829592) *
		t * exp(-x * x);

	return sign * y;
}
float erff(float value) { return (float)erf(value); }
long double erfl(long double value) { return erf((double)value); }
double erfc(double value) { return 1.0 - erf(value); }
float erfcf(float value) { return (float)erfc(value); }
long double erfcl(long double value) { return erfc((double)value); }

double lgamma(double value)
{
	double x = value, sum = 0.0;

	while (x < 8.0)
	{
		sum -= log(x);
		x += 1.0;
	}
	return sum + (x - 0.5) * log(x) - x + 0.91893853320467274178 + 1.0 / (12.0 * x) - 1.0 / (360.0 * x * x * x);
}
float lgammaf(float value) { return (float)lgamma(value); }
long double lgammal(long double value) { return lgamma((double)value); }
double tgamma(double value) { return exp(lgamma(value)); }
float tgammaf(float value) { return (float)tgamma(value); }
long double tgammal(long double value) { return tgamma((double)value); }
