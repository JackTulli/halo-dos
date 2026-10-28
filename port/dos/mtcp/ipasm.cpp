/*
   IPASM.CPP

   halo-dos: mTCP's IP checksums (IPASM.ASM, 16-bit assembly) in C for the
   DOS build's 32-bit protected mode. The same sums as the assembly: each
   pair of bytes loaded as a little-endian word, an odd last byte as the low
   byte of one, the pseudo-header's protocol and length as their network
   order words, and the one's complement of the folded total.

   mTCP: Copyright (C) 2005-2026 Michael B. Brutman (mbbrutman@gmail.com),
   GPLv3 (see LICENSE).
*/

#include "types.h"
#include "ip.h"

static uint32_t add_words( const uint8_t *p, uint16_t len, uint32_t sum ) {
  while ( len > 1 ) {
    sum += (uint32_t)p[0] | ((uint32_t)p[1] << 8);
    p += 2;
    len -= 2;
  }
  if ( len ) sum += p[0];
  return sum;
}

static uint16_t finish( uint32_t sum ) {
  while ( sum >> 16 ) sum = (sum & 0xFFFF) + (sum >> 16);
  return (uint16_t)~sum;
}

static uint32_t pseudo_header( const uint8_t *src, const uint8_t *target, uint8_t protocol, uint16_t len ) {
  uint32_t sum = add_words( src, 4, 0 );
  sum = add_words( target, 4, sum );
  sum += (uint32_t)protocol << 8;
  sum += (uint16_t)((len >> 8) | (len << 8));
  return sum;
}

extern "C" uint16_t ipchksum( uint16_t *data, uint16_t len ) {
  return finish( add_words( (const uint8_t *)data, len, 0 ) );
}

extern "C" uint16_t ip_p_chksum( const IpAddr_t src, const IpAddr_t target, uint16_t *data, uint8_t protocol,
                                 uint16_t len ) {
  return finish( add_words( (const uint8_t *)data, len, pseudo_header( src, target, protocol, len ) ) );
}

extern "C" uint16_t ip_p_chksum2( const IpAddr_t src, const IpAddr_t target, uint16_t *data, uint8_t protocol,
                                  uint16_t len, uint16_t *data2, uint16_t len2 ) {
  uint32_t sum = pseudo_header( src, target, protocol, (uint16_t)(len + len2) );
  sum = add_words( (const uint8_t *)data, len, sum );
  return finish( add_words( (const uint8_t *)data2, len2, sum ) );
}
