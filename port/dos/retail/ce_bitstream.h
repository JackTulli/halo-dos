/* Bit stream matching Halo CE 1.10's s_bitstream (haloceded.exe 0x4BBB70 write,
   0x4BBBD0 read, 0x4BBC20 write bool, 0x4BBE00 read bool).

   Bit order: stream bit p lives in byte p >> 3, bit p & 7 (LSB first). A value of
   n bits is stored starting with its least significant bit, so bit k of the value
   is stream bit (start + k). Values wider than 32 bits are written as consecutive
   32-bit words, low word first (the engine does the same; no message uses it).

   Plain C99, no allocation, no OS calls. */
#ifndef CE_BITSTREAM_H
#define CE_BITSTREAM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ce_bitstream {
    unsigned char *data;        /* buffer (read-only use when reading) */
    unsigned long limit;        /* capacity / valid data, in bits */
    unsigned long pos;          /* current bit */
    int overflow;               /* set when a read or write went past limit */
} ce_bitstream;

void ce_bs_init(ce_bitstream *bs, void *buffer, unsigned long size_bits);
void ce_bs_init_read(ce_bitstream *bs, const void *buffer, unsigned long size_bits);

/* Writes the low `bits` (0..32) bits of value. Returns 0 on success, -1 (and sets
   overflow, writing nothing) when it would pass the limit. */
int ce_bs_write(ce_bitstream *bs, unsigned long value, int bits);
int ce_bs_write_bit(ce_bitstream *bs, int bit);

/* Reads `bits` (0..32) bits; on overflow returns 0 and sets overflow. */
unsigned long ce_bs_read(ce_bitstream *bs, int bits);
int ce_bs_read_bit(ce_bitstream *bs);

/* Seeks; positions past limit set overflow and are refused (returns -1). */
int ce_bs_seek(ce_bitstream *bs, unsigned long pos);

/* Writes `bits` bits at an absolute position without moving pos. */
int ce_bs_poke(ce_bitstream *bs, unsigned long at, unsigned long value, int bits);

#define ce_bs_tell(bs) ((bs)->pos)
#define ce_bs_left(bs) ((bs)->limit - (bs)->pos)
#define ce_bs_bytes(bits) (((bits) + 7) >> 3)

#ifdef __cplusplus
}
#endif

#endif
