/*
CE_CRYPTO.H

The arithmetic of a Halo PC / Custom Edition connection (ce_crypto.c):
the GameSpy transport's challenge and response, which each side checks the
other with while connecting, and Halo's own key exchange and TEA cipher,
which every data packet after it is encrypted with, with a CRC-32 at its
end.
*/

#ifndef __CE_CRYPTO_H
#define __CE_CRYPTO_H

#include <stdint.h>

#define CE_CHALLENGE_SIZE 32
#define CE_KEY_SIZE 16
/* the private number a side keeps: 16 hexadecimal digits */
#define CE_SECRET_SIZE 17

/* a challenge: 32 printable characters whose response can be worked out
(the transport's own rule, which a server checks before it answers) */
void ce_gs_challenge(unsigned char challenge[CE_CHALLENGE_SIZE], uint32_t *seed);
/* the response to a challenge (two of its characters are random) */
void ce_gs_response(unsigned char response[CE_CHALLENGE_SIZE], const unsigned char challenge[CE_CHALLENGE_SIZE],
	uint32_t *seed);
/* a response matches the one worked out for the same challenge */
int ce_gs_response_matches(const unsigned char a[CE_CHALLENGE_SIZE], const unsigned char b[CE_CHALLENGE_SIZE]);

/* a new private number, its public key (sent to the other side), and the
key both sides then encrypt with (from the other side's public key) */
void ce_key_secret(char secret[CE_SECRET_SIZE], uint32_t seed);
void ce_key_public(const char *secret, unsigned char key[CE_KEY_SIZE]);
void ce_key_shared(const char *secret, const unsigned char other_public[CE_KEY_SIZE], unsigned char key[CE_KEY_SIZE]);

/* TEA over a buffer of 8 bytes or more (a length not a multiple of 8
encrypts its last 8 bytes again, overlapping) */
void ce_tea_encrypt(unsigned char *data, int size, const unsigned char key[CE_KEY_SIZE]);
void ce_tea_decrypt(unsigned char *data, int size, const unsigned char key[CE_KEY_SIZE]);

/* the CRC-32 at the end of a packet (not inverted at the end) */
uint32_t ce_crc32(const unsigned char *data, int size);

#endif
