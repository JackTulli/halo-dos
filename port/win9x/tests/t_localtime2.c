/*
T_LOCALTIME2.C

localtime (port/win9x/src/w9x_crt.c) against the UCRT's at every hour of
1971, 1999, 2007 (the year the United States' rules changed), 2024, 2026
and 2037, and at every minute of the six hours around each of this time
zone's changes in 2026: the moments where a rule applied to the wrong side
of a change would show. Also the ends of the 32-bit range.
*/

#include <stdio.h>
#include <time.h>

static void show(time_t moment)
{
	struct tm *t = localtime(&moment);

	if (!t)
	{
		printf("%ld NULL\n", (long)moment);
		return;
	}
	printf("%ld %04d-%02d-%02d %02d:%02d:%02d w%d y%d d%d\n", (long)moment, t->tm_year + 1900, t->tm_mon + 1,
		t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec, t->tm_wday, t->tm_yday, t->tm_isdst);
}

int main(void)
{
	static const long years[][2] = {
		{ 31536000L, 63072000L }, /* 1971 */
		{ 915148800L, 946684800L }, /* 1999 */
		{ 1167609600L, 1199145600L }, /* 2007 */
		{ 1704067200L, 1735689600L }, /* 2024 */
		{ 1767225600L, 1798761600L }, /* 2026 */
		{ 2114380800L, 2145916800L }, /* 2037 */
	};
	static const long edges[] = { -1, 0, 1, 3600, 86399, 2147397247L, 2147397248L, 2147483646L, 2147483647L };
	size_t i;
	long moment;
	int previous = -1;

	for (i = 0; i < sizeof(years) / sizeof(years[0]); i++)
	{
		for (moment = years[i][0]; moment < years[i][1]; moment += 3600)
			show((time_t)moment);
	}
	/* every minute around the changes of 2026 */
	for (moment = 1767225600L; moment < 1798761600L; moment += 3600)
	{
		time_t t = (time_t)moment;
		struct tm *local = localtime(&t);
		int isdst = local ? local->tm_isdst : -1;

		if (previous != -1 && isdst != previous)
		{
			long minute;

			for (minute = moment - 3 * 3600; minute < moment + 3 * 3600; minute += 60)
				show((time_t)minute);
		}
		previous = isdst;
	}
	for (i = 0; i < sizeof(edges) / sizeof(edges[0]); i++)
		show((time_t)edges[i]);
	fflush(stdout);
	return 0;
}
