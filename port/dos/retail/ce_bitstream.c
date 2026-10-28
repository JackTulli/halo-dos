/* See ce_bitstream.h. LSB-first bit stream, as Halo CE's bitstream.c. */
#include "ce_bitstream.h"

void ce_bs_init(ce_bitstream *bs, void *buffer, unsigned long size_bits)
{
    bs->data = (unsigned char *)buffer;
    bs->limit = size_bits;
    bs->pos = 0;
    bs->overflow = 0;
}

void ce_bs_init_read(ce_bitstream *bs, const void *buffer, unsigned long size_bits)
{
    ce_bs_init(bs, (void *)buffer, size_bits);
}

static void put_bits(unsigned char *data, unsigned long at, unsigned long value, int bits)
{
    while (bits > 0) {
        unsigned long byte = at >> 3;
        int shift = (int)(at & 7);
        int room = 8 - shift;
        int n = bits < room ? bits : room;
        unsigned int mask = ((1u << n) - 1u) << shift;
        data[byte] = (unsigned char)((data[byte] & ~mask) | ((unsigned int)(value << shift) & mask));
        value >>= n;
        at += (unsigned long)n;
        bits -= n;
    }
}

int ce_bs_write(ce_bitstream *bs, unsigned long value, int bits)
{
    if (bits <= 0)
        return 0;
    if (bits > 32 || bs->pos + (unsigned long)bits > bs->limit) {
        bs->overflow = 1;
        return -1;
    }
    if (bits < 32)
        value &= (1ul << bits) - 1ul;
    put_bits(bs->data, bs->pos, value, bits);
    bs->pos += (unsigned long)bits;
    return 0;
}

int ce_bs_write_bit(ce_bitstream *bs, int bit)
{
    return ce_bs_write(bs, bit ? 1ul : 0ul, 1);
}

int ce_bs_poke(ce_bitstream *bs, unsigned long at, unsigned long value, int bits)
{
    if (bits <= 0)
        return 0;
    if (bits > 32 || at + (unsigned long)bits > bs->limit) {
        bs->overflow = 1;
        return -1;
    }
    if (bits < 32)
        value &= (1ul << bits) - 1ul;
    put_bits(bs->data, at, value, bits);
    return 0;
}

unsigned long ce_bs_read(ce_bitstream *bs, int bits)
{
    unsigned long value = 0;
    int got = 0;
    if (bits <= 0)
        return 0;
    if (bits > 32 || bs->pos + (unsigned long)bits > bs->limit) {
        bs->overflow = 1;
        return 0;
    }
    while (got < bits) {
        unsigned long byte = bs->pos >> 3;
        int shift = (int)(bs->pos & 7);
        int room = 8 - shift;
        int n = (bits - got) < room ? (bits - got) : room;
        unsigned long chunk = ((unsigned long)bs->data[byte] >> shift) & ((1ul << n) - 1ul);
        value |= chunk << got;
        got += n;
        bs->pos += (unsigned long)n;
    }
    return value;
}

int ce_bs_read_bit(ce_bitstream *bs)
{
    return (int)ce_bs_read(bs, 1);
}

int ce_bs_seek(ce_bitstream *bs, unsigned long pos)
{
    if (pos > bs->limit) {
        bs->overflow = 1;
        return -1;
    }
    bs->pos = pos;
    return 0;
}
