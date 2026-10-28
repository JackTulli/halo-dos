/* Ghosts of Lockout profiles, adapted from Kevin Boggs' Halo3DS bots.
 * The 3DS fake-client damage fallback is deliberately not used: N64 bots
 * fire the game's real weapons and native projectiles deal their damage. */
#ifndef DOS_BOT_GHOSTS_H
#define DOS_BOT_GHOSTS_H

#include <stdint.h>

enum { DOS_GHOST_NONE, DOS_GHOST_CHAOTIC, DOS_GHOST_KODE };

/* Each named Ghost gets one1-in32 match roll, not a roll per bot/life. */
#define DOS_GHOST_MATCH_ODDS 32u

/* A private stream keeps roster and camouflage rolls out of combat RNG. */
static inline uint32_t dos_ghost_random(uint32_t *seed)
{
	uint32_t value = *seed ? *seed : UINT32_C(0x6d2b79f5);
	value ^= value << 13;
	value ^= value >> 17;
	value ^= value << 5;
	return *seed = value;
}

static inline void dos_ghost_slots(uint32_t seed, int count, int *chaotic, int *kode)
{
	*chaotic = *kode = -1;
	if (count <= 0) return;
	/* Rare match rolls; both may appear, never in the same slot. Lobby
	   preview and live roster replay the same private seeded decisions. */
	if (dos_ghost_random(&seed) % DOS_GHOST_MATCH_ODDS == 0)
		*chaotic = (int)(dos_ghost_random(&seed) % (unsigned)count);
	if (dos_ghost_random(&seed) % DOS_GHOST_MATCH_ODDS == 0)
	{
		*kode = (int)(dos_ghost_random(&seed) % (unsigned)count);
		if (*kode == *chaotic)
		{
			if (count > 1)
				*kode = (*kode + 1 + (int)(dos_ghost_random(&seed) % (unsigned)(count - 1))) % count;
			else if (dos_ghost_random(&seed) & 1)
				*chaotic = -1;
			else
				*kode = -1;
		}
	}
}

static inline int dos_ghost_profile(uint32_t seed, int index, int count)
{
	int chaotic, kode;
	if (index < 0 || index >= count) return DOS_GHOST_NONE;
	dos_ghost_slots(seed, count, &chaotic, &kode);
	return index == chaotic ? DOS_GHOST_CHAOTIC : index == kode ? DOS_GHOST_KODE : DOS_GHOST_NONE;
}

static inline const char *dos_ghost_name(int profile)
{
	return profile == DOS_GHOST_CHAOTIC ? "ChaoticSPARTAN1" :
		profile == DOS_GHOST_KODE ? "KodeBreaker16" : 0;
}

static inline int dos_ghost_camo_roll(int profile, uint32_t *seed)
{
	int chance = profile == DOS_GHOST_CHAOTIC ? 10 : profile == DOS_GHOST_KODE ? 25 : 0;
	return chance && dos_ghost_random(seed) % 100 < (unsigned)chance;
}

/* Team allocation is shared by the live lobby preview and actual players. */
static inline int dos_ghost_lobby_team(int index, const int *human_teams, int human_count)
{
	int counts[2] = { 0, 0 }, team = 0, i;
	if (index < 0 || index >= 15) return -1;
	for (i = 0; human_teams && i < human_count && i < 4; i++)
		counts[human_teams[i] & 1]++;
	for (i = 0; i <= index; i++)
	{
		team = counts[0] <= counts[1] ? 0 : 1;
		counts[team]++;
	}
	return team;
}

#endif
