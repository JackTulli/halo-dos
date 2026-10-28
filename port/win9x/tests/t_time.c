/*
T_TIME.C

Time in the 9x build's C library (port/win9x/src/w9x_crt.c), against the
UCRT's: localtime of fixed moments through the years (the time zone's
daylight saving rules applied to each year, as the UCRT applies them),
gmtime, asctime and strftime's C-locale fields (PDCLib's), and time,
clock and localtime_r agreeing with the system's clock (notes only: the
two programs run at different moments).
*/

#include <windows.h>

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "w9x_test.h"

#ifdef W9X_TEST
struct tm *localtime_r(const time_t *timer, struct tm *result);
#endif

static void show(const char *what, time_t moment, const struct tm *t)
{
	char text[128];

	if (!t)
	{
		printf("%s %ld: NULL\n", what, (long)moment);
		return;
	}
	strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S %a %b %j", t);
	printf("%s %ld: %s wday %d yday %d isdst %d\n", what, (long)moment, text, t->tm_wday, t->tm_yday,
		t->tm_isdst);
}

int main(void)
{
	static const long moments[] = {
		0, 86399, 86400, 951782400, 1000000000, 1009843199, 1104537600, 1234567890, 1300000000, 1400000000,
		1500000000, 1600000000, 1700000000, 1711846800, 1719792000, 1727740800, 1735689599, 1759400000,
		2000000000, 2147483647,
	};
	time_t now = time(NULL);
	size_t i;
	int month;

	for (i = 0; i < sizeof(moments) / sizeof(moments[0]); i++)
	{
		time_t moment = (time_t)moments[i];

		show("gmtime", moment, gmtime(&moment));
		show("localtime", moment, localtime(&moment));
	}
	/* the middle of every month of a few years, both sides of the
	daylight saving changes */
	for (i = 0; i < 4; i++)
	{
		for (month = 0; month < 12; month++)
		{
			time_t moment = (time_t)(946684800L + (long)i * 220898880L + month * 2629800L + 1296000L);

			show("month", moment, localtime(&moment));
		}
	}
	{
		struct tm by_value;
		struct tm *t = localtime(&now);
		SYSTEMTIME local;

		GetLocalTime(&local);
		printf("# now: time() %ld, localtime %02d:%02d, GetLocalTime %02d:%02d\n", (long)now, t->tm_hour, t->tm_min,
			local.wHour, local.wMinute);
		CHECK(t->tm_hour == local.wHour || (t->tm_min == 59 && local.wMinute == 0));
		CHECK(t->tm_year + 1900 == local.wYear || local.wMonth == 1);
#ifdef W9X_TEST
		CHECK(localtime_r(&now, &by_value) == &by_value && by_value.tm_min == t->tm_min);
#else
		(void)by_value;
#endif
	}
	{
		clock_t start = clock();

		Sleep(50);
		printf("# clock advanced %ld in 50 ms (CLOCKS_PER_SEC %ld)\n", (long)(clock() - start), (long)CLOCKS_PER_SEC);
		CHECK(clock() - start >= 40 * CLOCKS_PER_SEC / 1000);
	}
	{
		struct timespec spec;

		CHECK(timespec_get(&spec, TIME_UTC) == TIME_UTC);
		CHECK(spec.tv_sec - now >= 0 && spec.tv_sec - now < 10 && spec.tv_nsec >= 0 && spec.tv_nsec < 1000000000L);
	}
	printf("asctime: %s", asctime(gmtime(&(time_t){ 1234567890 })));
	printf("difftime: %ld\n", (long)difftime((time_t)100, (time_t)40));
	return test_finish("t_time");
}
