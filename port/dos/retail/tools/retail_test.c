/*
RETAIL_TEST.C

A host test (Linux) of ONLINE PLAY's retail Halo PC / Custom Edition list
against the live servers: retail_browse.c driven as the game drives it,
then the servers printed as the page lists them.
*/

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../retail_browse.h"

static unsigned long now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (unsigned long)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

int main(int argc, char **argv)
{
	static struct retail_browse_server servers[400];
	unsigned long start = now_ms();
	char last[80] = "";
	int count, i, players = 0;

	(void)argc;
	(void)argv;
	setvbuf(stdout, NULL, _IOLBF, 0);
	retail_browse_start(start, (uint32_t)time(NULL));
	/* (a frame's update every 33 ms, until the round has answers or 30 s) */
	while (now_ms() - start < 30000)
	{
		struct timespec frame = { 0, 33 * 1000000L };

		retail_browse_update(now_ms());
		if (strcmp(last, retail_browse_status()))
		{
			snprintf(last, sizeof(last), "%s", retail_browse_status());
			printf("%5lu ms  %s\n", now_ms() - start, last);
		}
		if (strstr(last, "ANSWERED") || strstr(last, "NOT") || strstr(last, "NO NETWORK"))
			break;
		nanosleep(&frame, NULL);
	}
	count = retail_browse_servers(servers, 400);
	for (i = 0; i < count; i++)
	{
		players += servers[i].players;
		if (i < 25)
			printf("%-2s %4d ms  %-30.30s %-16.16s %-12.12s %2d/%-2d%s\n", servers[i].kind ? "PC" : "CE",
				servers[i].ping_ms, servers[i].name, servers[i].map, servers[i].game_type, servers[i].players,
				servers[i].maximum_players, servers[i].password ? " (password)" : "");
	}
	printf("%d servers, %d players\n", count, players);
	retail_browse_stop();
	return 0;
}
