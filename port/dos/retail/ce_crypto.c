/*
CE_CRYPTO.C

The arithmetic of a Halo PC / Custom Edition connection (ce_crypto.h).

The challenge and response are the GameSpy transport's (GT2): a client and
a server each send 32 printable characters and expect the other to answer
with characters worked out from them and a key string the SDK shares
(positions 0 and 13 of an answer are random and not compared).

Halo's key exchange is a small Diffie-Hellman over 128-bit numbers held
big-endian in 16 bytes: a private number of 16 hexadecimal digits, the
public key 3 to its power, the shared key the other side's public key to
it, all modulo 0x10001 by a single conditional subtraction after each
step, as the game does. Both directions use the shared key. Packets are
then TEA-encrypted (32 rounds) in 8-byte blocks read as little-endian
words, a length that is no multiple of 8 encrypting its last 8 bytes once
more, overlapping.

Worked out from Luigi Auriemma's "Halo packets decryption/encryption
algorithm and keys builder" 0.1.3 and "GS SDK challenge-response
algorithm" 0.1 (Copyright 2004-2006 Luigi Auriemma, GPL-2.0-or-later); the
results are the same, checked by tools/ce_crypto_test.c.
*/

#include "ce_crypto.h"

#include <string.h>

/* ---------- the transport's challenge and response */

/* the key string the GameSpy SDK works its answers out with */
static const char gs_key[] = "3b8dd8995f7c40a9a5c5b7dd5b481341";
#define GS_KEY_SIZE (int)(sizeof(gs_key) - 1)

static unsigned char random_character(uint32_t *seed)
{
	*seed = *seed * 0x343FD + 0x269EC3;
	return (unsigned char)(((*seed >> 16) & 0x7FFF) % 93 + 33);
}

/* whether character i of a challenge must be odd (the transport's rule,
from the characters before it) */
static int odd_wanted(const unsigned char *challenge, int i, int odd)
{
	return (challenge[i - 1] & 1) ^ (i & 1) ^ odd ^ (challenge[0] & 1) ^ (challenge[0] < 79) ^
		(challenge[i - 1] < challenge[0]);
}

void ce_gs_challenge(unsigned char challenge[CE_CHALLENGE_SIZE], uint32_t *seed)
{
	int i, odd = 0;

	challenge[0] = random_character(seed);
	for (i = 1; i < CE_CHALLENGE_SIZE; i++)
	{
		odd = odd_wanted(challenge, i, odd);
		challenge[i] = random_character(seed);
		if ((challenge[i] & 1) != odd)
			challenge[i]++;
	}
}

static int challenge_valid(const unsigned char *challenge)
{
	int i, odd = 0;

	for (i = 1; i < CE_CHALLENGE_SIZE; i++)
	{
		odd = odd_wanted(challenge, i, odd);
		if ((challenge[i] & 1) != odd)
			return 0;
	}
	return 1;
}

void ce_gs_response(unsigned char response[CE_CHALLENGE_SIZE], const unsigned char challenge[CE_CHALLENGE_SIZE],
	uint32_t *seed)
{
	int valid = challenge_valid(challenge), i;

	for (i = 0; i < CE_CHALLENGE_SIZE; i++)
	{
		unsigned look, picked;

		if (!valid || i == 0 || i == 13)
		{
			response[i] = random_character(seed);
			continue;
		}
		look = i == 1 || i == 14 ? challenge[i] : challenge[i - 1];
		picked = challenge[((unsigned char)gs_key[(challenge[i] + i) % GS_KEY_SIZE] + challenge[i] * i) & 31];
		response[i] = (unsigned char)((picked ^ (unsigned char)gs_key[(look * i * 17991u) % GS_KEY_SIZE]) % 93 + 33);
	}
}

int ce_gs_response_matches(const unsigned char a[CE_CHALLENGE_SIZE], const unsigned char b[CE_CHALLENGE_SIZE])
{
	int i;

	for (i = 0; i < CE_CHALLENGE_SIZE; i++)
		if (i != 0 && i != 13 && a[i] != b[i])
			return 0;
	return 1;
}

/* ---------- the key exchange (16-byte big-endian numbers) */

/* number -= modulus, when number is above it (the game's reduction) */
static void reduce(unsigned char *number, const unsigned char *modulus)
{
	int i, carry;

	for (i = 0; i < 16; i++)
		if (number[i] != modulus[i])
			break;
	if (i == 16 || number[i] < modulus[i])
		return;
	for (carry = 0, i = 16; i--; carry >>= 8)
	{
		carry += number[i] - modulus[i];
		number[i] = (unsigned char)carry;
	}
}

/* a = a * b (shift and add, reduced at each step) */
static void multiply(unsigned char *a, const unsigned char *b, const unsigned char *modulus)
{
	unsigned char x[16], y[16];
	int count, i, carry;

	memcpy(x, a, 16);
	memcpy(y, b, 16);
	memset(a, 0, 16);
	for (count = 128; count--;)
	{
		if (x[15] & 1)
		{
			for (carry = 0, i = 16; i--; carry >>= 8)
			{
				carry += a[i] + y[i];
				a[i] = (unsigned char)carry;
			}
			reduce(a, modulus);
		}
		for (carry = 0, i = 0; i < 16; i++, carry <<= 8)
		{
			carry |= x[i];
			x[i] = (unsigned char)(carry >> 1);
			carry &= 1;
		}
		for (carry = 0, i = 16; i--; carry >>= 8)
		{
			carry += y[i] << 1;
			y[i] = (unsigned char)carry;
		}
		reduce(y, modulus);
	}
}

/* result = base to the power of exponent */
static void power(unsigned char *result, const unsigned char *base, const unsigned char *exponent,
	const unsigned char *modulus)
{
	unsigned char b[16], e[16];
	int count, i, carry;

	memcpy(b, base, 16);
	memcpy(e, exponent, 16);
	memset(result, 0, 16);
	result[15] = 1;
	for (count = 128; count--;)
	{
		if (e[15] & 1)
			multiply(result, b, modulus);
		multiply(b, b, modulus);
		for (carry = 0, i = 0; i < 16; i++, carry <<= 8)
		{
			carry |= e[i];
			e[i] = (unsigned char)(carry >> 1);
			carry &= 1;
		}
	}
}

/* hexadecimal digits as a number */
static void number_from_hex(const char *hex, unsigned char *number)
{
	int i, j, carry;

	memset(number, 0, 16);
	for (; *hex; hex++)
	{
		int t = *hex | 0x20;

		for (j = 0; j < 4; j++)
			for (carry = 0, i = 16; i--; carry >>= 8)
			{
				carry += number[i] << 1;
				number[i] = (unsigned char)carry;
			}
		number[15] |= (unsigned char)(t - 0x27 * (t > 0x60) - 0x30);
	}
}

static const unsigned char key_modulus[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1};
static const unsigned char key_base[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3};

void ce_key_secret(char secret[CE_SECRET_SIZE], uint32_t seed)
{
	static const char hex[] = "0123456789ABCDEF";
	int i;

	for (i = 0; i < 16; i++)
	{
		seed = seed * 0x343FD + 0x269EC3;
		secret[i] = hex[(seed >> 16) & 15];
	}
	secret[16] = 0;
}

void ce_key_public(const char *secret, unsigned char key[CE_KEY_SIZE])
{
	unsigned char exponent[16];

	number_from_hex(secret, exponent);
	power(key, key_base, exponent, key_modulus);
}

void ce_key_shared(const char *secret, const unsigned char other_public[CE_KEY_SIZE], unsigned char key[CE_KEY_SIZE])
{
	unsigned char exponent[16];

	number_from_hex(secret, exponent);
	power(key, other_public, exponent, key_modulus);
}

/* ---------- TEA */

static uint32_t load(const unsigned char *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void store(unsigned char *p, uint32_t value)
{
	p[0] = (unsigned char)value;
	p[1] = (unsigned char)(value >> 8);
	p[2] = (unsigned char)(value >> 16);
	p[3] = (unsigned char)(value >> 24);
}

static void encrypt_block(unsigned char *block, const unsigned char *key)
{
	uint32_t y = load(block), z = load(block + 4), sum = 0;
	uint32_t a = load(key), b = load(key + 4), c = load(key + 8), d = load(key + 12);
	int i;

	for (i = 0; i < 32; i++)
	{
		sum += 0x9E3779B9;
		y += ((z << 4) + a) ^ (z + sum) ^ ((z >> 5) + b);
		z += ((y << 4) + c) ^ (y + sum) ^ ((y >> 5) + d);
	}
	store(block, y);
	store(block + 4, z);
}

static void decrypt_block(unsigned char *block, const unsigned char *key)
{
	uint32_t y = load(block), z = load(block + 4), sum = 0xC6EF3720;
	uint32_t a = load(key), b = load(key + 4), c = load(key + 8), d = load(key + 12);
	int i;

	for (i = 0; i < 32; i++)
	{
		z -= ((y << 4) + c) ^ (y + sum) ^ ((y >> 5) + d);
		y -= ((z << 4) + a) ^ (z + sum) ^ ((z >> 5) + b);
		sum -= 0x9E3779B9;
	}
	store(block, y);
	store(block + 4, z);
}

void ce_tea_encrypt(unsigned char *data, int size, const unsigned char key[CE_KEY_SIZE])
{
	int i;

	if (size < 8)
		return;
	for (i = 0; i + 8 <= size; i += 8)
		encrypt_block(data + i, key);
	if (size & 7)
		encrypt_block(data + size - 8, key);
}

void ce_tea_decrypt(unsigned char *data, int size, const unsigned char key[CE_KEY_SIZE])
{
	int i;

	if (size < 8)
		return;
	if (size & 7)
		decrypt_block(data + size - 8, key);
	for (i = 0; i + 8 <= size; i += 8)
		decrypt_block(data + i, key);
}

/* ---------- CRC-32 */

uint32_t ce_crc32(const unsigned char *data, int size)
{
	static uint32_t table[256];
	static int made;
	uint32_t crc = 0xFFFFFFFF;

	if (!made)
	{
		uint32_t n, value;
		int bit;

		for (n = 0; n < 256; n++)
		{
			for (value = n, bit = 0; bit < 8; bit++)
				value = value & 1 ? 0xEDB88320 ^ (value >> 1) : value >> 1;
			table[n] = value;
		}
		made = 1;
	}
	while (size-- > 0)
		crc = table[(*data++ ^ crc) & 0xFF] ^ (crc >> 8);
	return crc;
}
