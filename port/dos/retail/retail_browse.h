/*
RETAIL_BROWSE.H

The retail Custom Edition servers for ONLINE PLAY's list (joined by
port/dos/game/dos_ce_client.c), and Halo PC's when asked for (only to look
at): the lists from the master server that replaced GameSpy's, then each
server asked what it is playing; again every minute while the page is
open. Driven by polling from
the network's update (port/dos/game/dos_network.c).
*/

#ifndef __RETAIL_BROWSE_H
#define __RETAIL_BROWSE_H

#include <stdint.h>

/* which list a server came from */
enum
{
	_retail_custom_edition,
	_retail_pc
};

struct retail_browse_server
{
	int kind;
	char name[64], map[32], game_type[32], version[24];
	int players, maximum_players, password, ping_ms;
	/* where it is (network order; its port) */
	uint32_t address;
	uint16_t port;
};

/* starts (or restarts) looking; seed: random bits */
void retail_browse_start(unsigned long now_ms, uint32_t seed);
void retail_browse_stop(void);
/* whether Halo PC's servers are listed too (the default: Custom Edition's
alone) */
void retail_browse_include_pc(int include);
void retail_browse_update(unsigned long now_ms);
/* the servers that answered, those with players first, then by ping */
int retail_browse_servers(struct retail_browse_server *servers, int maximum);
/* what it is doing, for the page's help line ("" when nothing to say) */
const char *retail_browse_status(void);

#endif
