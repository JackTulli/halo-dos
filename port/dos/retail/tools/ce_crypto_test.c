/*
CE_CRYPTO_TEST.C

ce_crypto.c against Luigi Auriemma's originals (halo_pck_algo.h 0.1.3 and
gssdkcr.h 0.1, GPL-2.0-or-later), on random inputs: challenges, responses,
public and shared keys, a whole key exchange, TEA both ways at every length
from 8 to 64 bytes, and the CRC. Built on the PC with his two files on the
include path (they are not in this repository):

    clang ce_crypto_test.c ../ce_crypto.c -I <folder with his headers> -o ce_crypto_test.exe
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../ce_crypto.h"

/* (his files define these names: kept apart from ours; WIN32 picks his
Windows clock) */
#ifdef _WIN32
#define WIN32
#endif
#include "halo_pck_algo.h"
#include "gssdkcr.h"

static int failures;

static void check(int ok, const char *what, int round)
{
	if (!ok && failures++ < 20)
		printf("FAILED: %s (round %d)\n", what, round);
}

int main(void)
{
	uint32_t seed = 12345;
	int round, size;

	for (round = 0; round < 2000; round++)
	{
		unsigned char challenge[32], mine[32], his[33], copy[33];
		char secret[CE_SECRET_SIZE], secret2[CE_SECRET_SIZE], hex[33], three[2] = "3", fixed[6] = "10001";
		unsigned char key_mine[16], key_his[16], other[16], shared_mine[16], shared_his[16], back[16];
		unsigned char data[64], copy_data[64], tea_key[16];
		int i;

		/* challenge and response */
		ce_gs_challenge(challenge, &seed);
		memcpy(copy, challenge, 32);
		copy[32] = 0;
		ce_gs_response(mine, challenge, &seed);
		gssdkcr(his, copy, 0);
		check(ce_gs_response_matches(mine, his), "response to a valid challenge", round);
		for (i = 0; i < 32; i++)
			check(challenge[i] >= 33 && challenge[i] <= 126, "challenge printable", round);

		/* public keys */
		ce_key_secret(secret, seed + round);
		ce_key_public(secret, key_mine);
		memcpy(hex, secret, 17);
		halo_create_key((uint8_t *)three, (uint8_t *)hex, (uint8_t *)fixed, key_his);
		strcpy(three, "3");
		strcpy(fixed, "10001");
		check(memcmp(key_mine, key_his, 16) == 0, "public key", round);

		/* shared keys, from a random 'public key' of the other side */
		for (i = 0; i < 16; i++)
			other[i] = (unsigned char)(i < 13 ? 0 : rand());
		if (round & 1)
			for (i = 0; i < 16; i++)
				other[i] = (unsigned char)rand();
		ce_key_shared(secret, other, shared_mine);
		halo_byte2hex(other, (uint8_t *)hex);
		memcpy(copy, secret, 17);
		halo_create_key((uint8_t *)hex, copy, (uint8_t *)fixed, shared_his);
		strcpy(fixed, "10001");
		check(memcmp(shared_mine, shared_his, 16) == 0, "shared key", round);

		/* a whole exchange: both sides reach the same key */
		ce_key_secret(secret2, seed * 7 + round);
		ce_key_public(secret2, other);
		ce_key_shared(secret, other, shared_mine);
		ce_key_shared(secret2, key_mine, back);
		check(memcmp(shared_mine, back, 16) == 0, "both sides' keys", round);

		/* TEA and CRC */
		for (i = 0; i < 16; i++)
			tea_key[i] = (unsigned char)rand();
		for (size = 8; size <= 64; size++)
		{
			for (i = 0; i < size; i++)
				data[i] = (unsigned char)rand();
			memcpy(copy_data, data, size);
			ce_tea_encrypt(data, size, tea_key);
			halo_tea_encrypt(copy_data, size, tea_key);
			check(memcmp(data, copy_data, size) == 0, "TEA encryption", round);
			ce_tea_decrypt(data, size, tea_key);
			halo_tea_decrypt(copy_data, size, tea_key);
			check(memcmp(data, copy_data, size) == 0, "TEA decryption", round);
			check(ce_crc32(data, size) == halo_crc32(data, size), "CRC", round);
		}
	}
	printf("%s: %d failures\n", failures ? "FAILED" : "passed", failures);
	return failures != 0;
}
