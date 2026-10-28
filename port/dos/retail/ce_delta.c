/* Halo CE 1.10 message-delta codec. Every field type mirrors the engine procedure
   named next to it (haloceded.exe 01.00.10.0621 addresses); see ce_delta_notes.md
   for the bit layouts. Floating point: the dedicated server runs with the x87
   precision control at 53 bits (MSVC default), so its intermediate results are
   doubles; the code below does the same arithmetic in double, in the engine's
   order, with the engine's float constants. */
#include "ce_delta.h"

#include <math.h>
#include <string.h>

/* engine constants (float), see notes */
#define K_EPS        ((double)9.99999974737875e-05f)     /* 0x38D1B717: 0.0001f */
#define K_PI_F       ((double)3.14159274101257324f)      /* 0x40490FDB */
#define K_2PI_F      ((double)6.28318548202514648f)      /* 0x40C90FDB */
#define K_HALF_PI_F  ((double)1.57079637050628662f)      /* 0x3FC90FDB */
#define K_3HALF_PI_F ((double)4.71238899230957031f)      /* 0x4096CBE4 */
#define K_INV_PI_F   ((double)0.318309873342514038f)     /* 0x3EA2F983 */
#define K_INV_2PI_F  ((double)0.159154936671257019f)     /* 0x3E22F983 */
#define K_POS_MIN    5000.0                               /* -5000..5000 world units */
#define K_POS_SPAN   10000.0

typedef struct codec {
    const ce_delta_config *cfg;
    ce_bitstream *bs;
} codec;

/* ------------------------------------------------------------------ helpers */

static unsigned long ld_u8(const void *p) { return *(const unsigned char *)p; }
static unsigned long ld_u16(const void *p) { unsigned short v; memcpy(&v, p, 2); return v; }
static unsigned long ld_u32(const void *p) { unsigned long v = 0; unsigned int w; memcpy(&w, p, 4); v = w; return v; }
static float ld_f32(const void *p) { float v; memcpy(&v, p, 4); return v; }
static void st_u8(void *p, unsigned long v) { *(unsigned char *)p = (unsigned char)v; }
static void st_u16(void *p, unsigned long v) { unsigned short w = (unsigned short)v; memcpy(p, &w, 2); }
static void st_u32(void *p, unsigned long v) { unsigned int w = (unsigned int)v; memcpy(p, &w, 4); }
static void st_f32(void *p, float v) { memcpy(p, &v, 4); }
static void *ld_ptr(const void *p) { void *v; memcpy(&v, p, sizeof v); return v; }

static int bits_for(unsigned long n)  /* the engine's byte table at 0x5574A8: max(1, bit length) */
{
    int b = 0;
    while (n) {
        b++;
        n >>= 1;
    }
    return b ? b : 1;
}

/* unsigned 32-bit as the engine loads it on the FPU (fild + 2^32 if negative) */
static double u32d(unsigned long v) { return (double)(v & 0xFFFFFFFFul); }

/* floor(x) then _ftol (64-bit truncation), low 32 bits; then the engine's clamp
   `if ((unsigned)q > range) q = range`: values below the range's bottom wrap to
   huge unsigned numbers and clamp to the TOP (engine behaviour). */
static unsigned long quantize_clamp(double x, unsigned long range)
{
    double f = floor(x);
    unsigned long q;
    if (f >= 0.0) {
        if (f >= 4294967296.0)
            q = 0xFFFFFFFFul;   /* only the low dword is kept; clamps below anyway */
        else
            q = (unsigned long)f;
    } else {
        double m = -f;
        unsigned long lo = (m >= 4294967296.0) ? 0ul : (unsigned long)m;
        q = (0ul - lo) & 0xFFFFFFFFul;
        if (q == 0 && m != 0.0)
            q = 0xFFFFFFFFul;   /* -2^32 multiples: keep it "big" so it clamps high */
    }
    if (q > range)
        q = range;
    return q;
}

/* 0x4D6F20: real_to_fixed(value, min, max) with range */
static unsigned long real_to_fixed(double v, double lo, double hi, unsigned long range)
{
    double t = (v - lo) / (hi - lo);
    return quantize_clamp(t * u32d(range) + 0.5, range);
}

static int changed_real(float base, float src)  /* 0x4D56F0 */
{
    double d = (double)base - (double)src;
    return d < -K_EPS || d > K_EPS;   /* NaN: unchanged (both false), as the engine */
}

#define W(v, n) do { if (ce_bs_write(c->bs, (unsigned long)(v), (n)) < 0) return CE_ERR_OVERFLOW; } while (0)
#define R(dst, n) do { (dst) = ce_bs_read(c->bs, (n)); if (c->bs->overflow) return CE_ERR_OVERFLOW; } while (0)

static long enc_field(codec *c, const ce_field_props *p, const void *base, const void *src);
static long dec_field(codec *c, const ce_field_props *p, const void *base, void *dst);

static int class_lan(const codec *c) { return c->cfg->encoding_class == CE_CLASS_LAN; }

/* ------------------------------------------------------------------ integers etc. */

static unsigned long int_load(const ce_field_props *p, const void *at)
{
    if (p->size == 1) return ld_u8(at);
    if (p->size == 2) return ld_u16(at);
    return ld_u32(at);
}

static void int_store(const ce_field_props *p, void *at, unsigned long v)
{
    if (p->size == 1) st_u8(at, v);
    else if (p->size == 2) st_u16(at, v);
    else st_u32(at, v);
}

/* ------------------------------------------------------------------ normals (0x4D7350/0x4D7510) */

static void normal_angles(const float in[3], float *theta, float *phi)  /* 0x4019A0 + 0x4D71C0 */
{
    float x = in[0], y = in[1], z = in[2];
    double len = sqrt((double)x * x + (double)y * y + (double)z * z);
    if (!(fabs(len) < K_EPS)) {   /* normalize unless shorter than 0.0001 */
        double inv = 1.0 / len;
        x = (float)(inv * x);
        y = (float)(inv * y);
        z = (float)(inv * z);
    }
    if ((double)x <= K_EPS && (double)x >= -K_EPS) {
        *phi = (float)(y > 0.0f ? K_HALF_PI_F : -K_HALF_PI_F);
        *theta = (float)acos((double)z);
    } else {
        double a = atan((double)y / (double)x);
        if (x < 0.0f)
            a += K_PI_F;
        *phi = (float)a;
        *theta = (float)acos((double)z);
    }
}

static void normal_bits(const codec *c, const ce_field_props *p, int *bt, int *bp)
{
    if (class_lan(c)) {
        *bt = p->bits_lan[0];
        *bp = p->bits_lan[1];
    } else {
        *bt = p->bits_internet[0];
        *bp = p->bits_internet[1];
    }
}

static long enc_normal(codec *c, const ce_field_props *p, const void *base, const void *src)
{
    int bt, bp;
    float v[3], theta, phi;
    unsigned long rt, rp, q0, q1;
    normal_bits(c, p, &bt, &bp);
    rt = (bt >= 32) ? 0xFFFFFFFFul : ((1ul << bt) - 1ul);
    rp = (bp >= 32) ? 0xFFFFFFFFul : ((1ul << bp) - 1ul);
    memcpy(v, src, 12);
    normal_angles(v, &theta, &phi);
    q0 = quantize_clamp((double)theta * K_INV_PI_F * u32d(rt) + 0.5, rt);
    q1 = quantize_clamp(((double)phi + K_HALF_PI_F) * K_INV_2PI_F * u32d(rp) + 0.5, rp);
    if (base) {
        float b[3], bth, bph;
        memcpy(b, base, 12);
        normal_angles(b, &bth, &bph);
        if (real_to_fixed(bth, 0.0, K_PI_F, rt) == q0 &&
            real_to_fixed(bph, -K_HALF_PI_F, K_3HALF_PI_F, rp) == q1)
            return 0;
    }
    W(q0, bt);
    W(q1, bp);
    return bt + bp;
}

static float snap(float v) { return (v < (float)K_EPS && v > -(float)K_EPS) ? 0.0f : v; }

static long dec_normal(codec *c, const ce_field_props *p, void *dst)
{
    int bt, bp;
    unsigned long q0, q1;
    float theta, phi, st, sp, ct, cp, out[3];
    normal_bits(c, p, &bt, &bp);
    R(q0, bt);
    R(q1, bp);
    theta = (float)(u32d(q0) / u32d((1ul << bt) - 1ul) * K_PI_F);
    phi = (float)(u32d(q1) / u32d((1ul << bp) - 1ul) * K_2PI_F - K_HALF_PI_F);
    st = snap((float)sin(theta));   /* 0x4D7270 */
    sp = snap((float)sin(phi));
    ct = snap((float)cos(theta));
    cp = snap((float)cos(phi));
    out[0] = cp * st;
    out[1] = sp * st;
    out[2] = ct;
    memcpy(dst, out, 12);
    return bt + bp;
}

/* ------------------------------------------------------------------ locality (0x4D7680/0x4D7970) */

static long enc_locality(codec *c, const void *base, const void *src)
{
    const ce_delta_config *cfg = c->cfg;
    float s[3];
    long bits = 0;
    int i, b;
    unsigned long range;
    double range_f;
    memcpy(s, src, 12);
    if (base) {
        float bv[3], d[3];
        int sign[3], too_big = 0;
        double dist;
        memcpy(bv, base, 12);
        for (i = 0; i < 3; i++)
            d[i] = (float)((double)s[i] - (double)bv[i]);
        dist = sqrt((double)d[2] * d[2] + (double)d[1] * d[1] + (double)d[0] * d[0]);
        if (!(dist > (double)cfg->locality_min_move))
            return 0;   /* moved no more than the minimum (NaN too): unchanged */
        for (i = 0; i < 3; i++) {
            sign[i] = d[i] < 0.0f;
            if (sign[i])
                d[i] = -d[i];
            if ((double)d[i] > (double)cfg->locality_delta_cutoff)
                too_big = 1;
        }
        if (!too_big && cfg->encoding_class == CE_CLASS_INTERNET) {
            int bd = cfg->locality_bits_delta;
            unsigned long rd = (1ul << bd) - 1ul;
            W(0, 1);
            for (i = 0; i < 3; i++) {
                unsigned long q = quantize_clamp((double)d[i] / (double)cfg->locality_delta_cutoff * u32d(rd) + 0.5, rd);
                W(sign[i], 1);
                W(q, bd);
            }
            return 1 + 3 * (1 + bd);
        }
        W(1, 1);
        bits = 1;
    }
    b = class_lan(c) ? cfg->locality_bits_lan : cfg->locality_bits_full;
    range = (b >= 32) ? 0xFFFFFFFFul : ((1ul << b) - 1ul);
    range_f = (double)(float)u32d(range);   /* the encoder rounds the range to float (decoder does not) */
    for (i = 0; i < 3; i++) {
        double t = ((double)s[i] + K_POS_MIN) * K_EPS;
        unsigned long q = quantize_clamp(range_f * t + 0.5, range);
        W(q, b);
    }
    return bits + 3 * b;
}

static long dec_locality(codec *c, const void *base, void *dst)
{
    const ce_delta_config *cfg = c->cfg;
    float out[3];
    long bits = 0;
    int i, b;
    unsigned long range, q;
    if (base) {
        unsigned long full;
        R(full, 1);
        bits = 1;
        if (!full) {
            int bd = cfg->locality_bits_delta;
            float bv[3];
            memcpy(bv, base, 12);
            for (i = 0; i < 3; i++) {
                unsigned long s;
                float d;
                R(s, 1);
                R(q, bd);
                d = (float)(u32d(q) / u32d((1ul << bd) - 1ul) * (double)cfg->locality_delta_cutoff);
                if (s)
                    d = -d;
                out[i] = (float)((double)d + (double)bv[i]);
            }
            memcpy(dst, out, 12);
            return 1 + 3 * (1 + bd);
        }
    }
    b = class_lan(c) ? cfg->locality_bits_lan : cfg->locality_bits_full;
    range = (b >= 32) ? 0xFFFFFFFFul : ((1ul << b) - 1ul);
    for (i = 0; i < 3; i++) {
        R(q, b);
        out[i] = (float)(u32d(q) / u32d(range) * K_POS_SPAN - K_POS_MIN);
    }
    memcpy(dst, out, 12);
    return bits + 3 * b;
}

/* ------------------------------------------------------------------ smart vector (0x4D8120/0x4D8330) */

static void smart_quantize(const codec *c, const ce_field_props *p, const float v[3], unsigned long q[3])
{
    unsigned long range = class_lan(c) ? p->range_lan : p->range;
    int i;
    for (i = 0; i < 3; i++)
        q[i] = real_to_fixed(v[i], p->minimum, p->maximum, range);
}

static long enc_smart(codec *c, const ce_field_props *p, const void *base, const void *src)
{
    int b = class_lan(c) ? p->bits_lan[0] : p->bits_internet[0];
    float v[3];
    unsigned long q[3];
    long bits = 0;
    int i;
    memcpy(v, src, 12);
    smart_quantize(c, p, v, q);
    if (base) {
        float bv[3];
        unsigned long bq[3];
        memcpy(bv, base, 12);
        smart_quantize(c, p, bv, bq);
        if (bq[0] == q[0] && bq[1] == q[1] && bq[2] == q[2])
            return 0;
    } else {
        int k;
        for (k = 0; k < p->count; k++) {
            unsigned long sq[3];
            smart_quantize(c, p, p->special[k], sq);
            if (sq[0] == q[0] && sq[1] == q[1] && sq[2] == q[2])
                break;
        }
        if (k < p->count) {
            for (i = 0; i <= k; i++)
                W(1, 1);              /* k+1 one bits */
            bits = k + 1;
            if (p->count > 1) {
                W(0, 1);
                bits++;
            }
            return bits;
        }
        W(0, 1);
        bits = 1;
    }
    for (i = 0; i < 3; i++)
        W(q[i], b);
    return bits + 3 * b;
}

static long dec_smart(codec *c, const ce_field_props *p, const void *base, void *dst)
{
    int b = class_lan(c) ? p->bits_lan[0] : p->bits_internet[0];
    unsigned long range = class_lan(c) ? p->range_lan : p->range;
    long bits = 0;
    unsigned long q[3];
    float out[3];
    int i;
    if (!base) {
        int index = -1;
        for (;;) {
            unsigned long bit;
            R(bit, 1);
            bits++;
            index += (int)bit;
            if (p->count == 1 || !bit)
                break;
        }
        if (index >= 0) {
            if (index >= p->count)
                return CE_ERR_UNSUPPORTED;
            memcpy(dst, p->special[index], 12);
            return bits;
        }
    }
    for (i = 0; i < 3; i++)
        R(q[i], b);
    for (i = 0; i < 3; i++)   /* 0x4D7E10 */
        out[i] = (float)(u32d(q[i]) / u32d(range) * ((double)p->maximum - (double)p->minimum) + (double)p->minimum);
    memcpy(dst, out, 12);
    return bits + 3 * b;
}

/* ------------------------------------------------------------------ throttle (0x4D7C00) */

static unsigned long throttle_code(const void *at)   /* 0x4D7AF0: z is ignored */
{
    float x = ld_f32(at), y = ld_f32((const char *)at + 4);
    unsigned long code = 0;
    if ((double)x > K_EPS) code |= 8;
    if ((double)x < -K_EPS) code |= 4;
    if ((double)y > K_EPS) code |= 2;
    if ((double)y < -K_EPS) code |= 1;
    return code;
}

/* The engine writes all 12 bytes; the structures hold a 2D vector (8 bytes, the
   next field follows), so only `size` bytes are stored here. */
static void throttle_vector(unsigned long code, void *dst, unsigned size)   /* 0x4D7B60 */
{
    float v[3] = { 0.0f, 0.0f, 0.0f };
    int comp;
    if (code) {
        for (comp = 1; comp >= 0; comp--) {
            if (code & 1) v[comp] = -1.0f;
            if (code & 2) v[comp] = 1.0f;
            code >>= 2;
        }
        {
            double len = sqrt((double)v[0] * v[0] + (double)v[1] * v[1] + (double)v[2] * v[2]);
            if (!(fabs(len) < K_EPS)) {
                double inv = 1.0 / len;
                v[0] = (float)(inv * v[0]);
                v[1] = (float)(inv * v[1]);
                v[2] = (float)(inv * v[2]);
            }
        }
    }
    memcpy(dst, v, size < 12 ? size : 12);
}

/* ------------------------------------------------------------------ strings */

/* Strings whose maximum length fills their array carry no NUL (p->size is the
   room the structure gives them); a longer run without NUL is an error. */
static long str_units(const ce_field_props *p, int unit)
{
    long units = (long)(p->size / (unsigned)unit);
    return units < p->count + 1 ? units : p->count + 1;
}

static long ascii_len(const ce_field_props *p, const void *at)
{
    const char *s = (const char *)at;
    long n = 0, limit = str_units(p, 1);
    while (n < limit && s[n])
        n++;
    return n;
}

static long wide_len(const ce_field_props *p, const void *at)
{
    long n = 0, limit = str_units(p, 2);
    while (n < limit && ld_u16((const char *)at + 2 * n))
        n++;
    return n;
}

/* ------------------------------------------------------------------ encode */

static long enc_field(codec *c, const ce_field_props *p, const void *base, const void *src)
{
    switch (p->type) {
    case CE_FT_INTEGER: {   /* 0x4D54D0 */
        unsigned long v = int_load(p, src);
        if (base && int_load(p, base) == v)
            return 0;
        W(v, p->bits);
        return p->bits;
    }
    case CE_FT_REAL:        /* 0x4D56F0 */
    case CE_FT_ANGLE:
        if (base && !changed_real(ld_f32(base), ld_f32(src)))
            return 0;
        W(ld_u32(src), 32);
        return 32;
    case CE_FT_TIME:        /* 0x4D6EF0 */
        if (base && ld_u32(base) == ld_u32(src))
            return 0;
        W(ld_u32(src), 32);
        return 32;
    case CE_FT_BOOLEAN:     /* 0x4D5750: the bit is (byte == 1) */
        if (base && ld_u8(base) == ld_u8(src))
            return 0;
        W(ld_u8(src) == 1, 1);
        return 1;
    case CE_FT_ASCII_STRING: {   /* 0x4D5840 */
        long n = ascii_len(p, src), i;
        if (n > p->count)
            return CE_ERR_STRING;
        if (base && ascii_len(p, base) == n && memcmp(base, src, (size_t)n) == 0)
            return 0;
        W(n, p->bits);
        for (i = 0; i < n; i++)
            W(((const unsigned char *)src)[i], 8);
        return p->bits + 8 * n;
    }
    case CE_FT_WIDE_STRING: {    /* 0x4D59D0 */
        long n = wide_len(p, src), i;
        if (n > p->count)
            return CE_ERR_STRING;
        if (base && wide_len(p, base) == n && memcmp(base, src, (size_t)(2 * n)) == 0)
            return 0;
        W(n, p->bits);
        for (i = 0; i < n; i++)
            W(ld_u16((const char *)src + 2 * i), 16);
        return p->bits + 16 * n;
    }
    case CE_FT_ARRAY:            /* 0x4D5BC0 */
    case CE_FT_STRUCTURE: {      /* 0x4D6070 */
        int n = p->count, i;
        long total = 0;
        if (!base) {
            for (i = 0; i < n; i++) {
                long r;
                if (p->type == CE_FT_ARRAY)
                    r = enc_field(c, p->element, NULL, (const char *)src + p->stride * (unsigned)i);
                else
                    r = enc_field(c, p->members[i].props, NULL, (const char *)src + p->members[i].offset);
                if (r < 0)
                    return r;
                total += r;
            }
            return total;
        } else {
            unsigned long start = c->bs->pos;
            if (ce_bs_seek(c->bs, start + (unsigned long)n) < 0)
                return CE_ERR_OVERFLOW;
            for (i = 0; i < n; i++) {
                long r;
                if (p->type == CE_FT_ARRAY)
                    r = enc_field(c, p->element, (const char *)base + p->stride * (unsigned)i,
                                  (const char *)src + p->stride * (unsigned)i);
                else
                    r = enc_field(c, p->members[i].props, (const char *)base + p->members[i].baseline_offset,
                                  (const char *)src + p->members[i].offset);
                if (r < 0)
                    return r;
                if (ce_bs_poke(c->bs, start + (unsigned long)i, r > 0, 1) < 0)
                    return CE_ERR_OVERFLOW;
                total += r;
            }
            if (total > 0)
                return total + n;
            c->bs->pos = start;     /* nothing changed: no header either */
            return 0;
        }
    }
    case CE_FT_POINTER: {        /* 0x4D64C0 */
        const void *s = ld_ptr(src), *b = base ? ld_ptr(base) : NULL;
        if (!s)
            return CE_ERR_POINTER;
        return enc_field(c, p->element, b, s);
    }
    case CE_FT_TRANSLATED_INDEX: /* 0x4D6670 */
        if (base && ld_u32(base) == ld_u32(src))
            return 0;
        W(ld_u32(src), p->bits);
        return p->bits;
    case CE_FT_POINT:            /* 0x4D6860 */
    case CE_FT_VECTOR: {
        int n = p->count, i;
        if (!base) {
            for (i = 0; i < n; i++)
                W(ld_u32((const char *)src + 4 * i), 32);
            return 32 * n;
        } else {
            unsigned long start = c->bs->pos;
            long total = 0;
            if (ce_bs_seek(c->bs, start + (unsigned long)n) < 0)
                return CE_ERR_OVERFLOW;
            for (i = 0; i < n; i++) {
                int ch = changed_real(ld_f32((const char *)base + 4 * i), ld_f32((const char *)src + 4 * i));
                if (ch) {
                    W(ld_u32((const char *)src + 4 * i), 32);
                    total += 32;
                }
                if (ce_bs_poke(c->bs, start + (unsigned long)i, (unsigned long)ch, 1) < 0)
                    return CE_ERR_OVERFLOW;
            }
            if (total > 0)
                return total + n;
            c->bs->pos = start;
            return 0;
        }
    }
    case CE_FT_FLAGS: {          /* 0x4D6D60 */
        unsigned long v = ld_u32(src), bv = base ? ld_u32(base) : 0;
        unsigned long start = c->bs->pos;
        int i, changed = base == NULL;
        long bits = 0;
        for (i = 0; i < p->count; i++) {
            unsigned long m = 1ul << i;
            if (!(p->valid_mask & m))
                continue;
            if (base && ((bv & m) != 0) != ((v & m) != 0))
                changed = 1;
            W((v & m) != 0, 1);
            bits++;
        }
        if (!changed) {
            c->bs->pos = start;
            return 0;
        }
        return bits;
    }
    case CE_FT_GRENADE_COUNTS: { /* 0x4D70B0: signed bytes, (g0 << 3) | g1 */
        long v = ((long)(signed char)ld_u8(src) * 8) | (long)(signed char)ld_u8((const char *)src + 1);
        if (base) {
            long b = ((long)(signed char)ld_u8(base) * 8) | (long)(signed char)ld_u8((const char *)base + 1);
            if (b == v)
                return 0;
        }
        W((unsigned long)v, 6);
        return 6;
    }
    case CE_FT_FIXED_WIDTH: {    /* 0x4D6FA0 */
        unsigned long q = quantize_clamp((double)ld_f32(src) * u32d(p->range) + 0.5, p->range);
        if (base && real_to_fixed(ld_f32(base), 0.0, 1.0, p->range) == q)
            return 0;
        W(q, p->bits);
        return p->bits;
    }
    case CE_FT_FIXED_WIDTH_NORMAL_VECTOR:
        return enc_normal(c, p, base, src);
    case CE_FT_LOCALITY_REFERENCE_POSITION:
        return enc_locality(c, base, src);
    case CE_FT_DIGITAL_THROTTLE: {   /* 0x4D7C00 */
        unsigned long code = throttle_code(src);
        if (base && throttle_code(base) == code)
            return 0;
        W(code, 4);
        return 4;
    }
    case CE_FT_FIXED_WIDTH_WEAPON_INDEX: {   /* 0x4D7CC0 */
        unsigned long v = ld_u16(src);
        unsigned long code = ((v == 0xFFFFul) ? 4ul : 0ul) | (v & 3ul);
        if (base) {
            unsigned long bv = ld_u16(base);
            if ((((bv == 0xFFFFul) ? 4ul : 0ul) | (bv & 3ul)) == code)
                return 0;
        }
        W(code, 3);
        return 3;
    }
    case CE_FT_FIXED_WIDTH_GRENADE_INDEX: {  /* 0x4D7D70 */
        unsigned long v = ld_u16(src);
        unsigned long code = ((v == 0xFFFFul) ? 2ul : 0ul) | (v & 1ul);
        if (base) {
            unsigned long bv = ld_u16(base);
            if ((((bv == 0xFFFFul) ? 2ul : 0ul) | (bv & 1ul)) == code)
                return 0;
        }
        W(code, 2);
        return 2;
    }
    case CE_FT_SMART_VECTOR:
        return enc_smart(c, p, base, src);
    default:
        return CE_ERR_UNSUPPORTED;
    }
}

/* ------------------------------------------------------------------ decode */

static long dec_field(codec *c, const ce_field_props *p, const void *base, void *dst)
{
    unsigned long v;
    switch (p->type) {
    case CE_FT_INTEGER:          /* 0x4D5600 */
        R(v, p->bits);
        int_store(p, dst, v);
        return p->bits;
    case CE_FT_REAL:             /* 0x4D5730 */
    case CE_FT_ANGLE:
    case CE_FT_TIME:
        R(v, 32);
        st_u32(dst, v);
        return 32;
    case CE_FT_BOOLEAN:          /* 0x4D5790 */
        R(v, 1);
        st_u8(dst, v);
        return 1;
    case CE_FT_ASCII_STRING: {   /* 0x4D5940 */
        unsigned long n, i;
        R(n, p->bits);
        if ((long)n > p->count)
            return CE_ERR_STRING;
        for (i = 0; i < n; i++) {
            R(v, 8);
            ((unsigned char *)dst)[i] = (unsigned char)v;
        }
        if ((long)n < (long)p->size)   /* the engine always writes the NUL (into the next field when full) */
            ((unsigned char *)dst)[n] = 0;
        return p->bits + 8 * (long)n;
    }
    case CE_FT_WIDE_STRING: {    /* 0x4D5AA0 */
        unsigned long n, i;
        R(n, p->bits);
        if ((long)n > p->count)
            return CE_ERR_STRING;
        for (i = 0; i < n; i++) {
            R(v, 16);
            st_u16((char *)dst + 2 * i, v);
        }
        if ((long)(2 * n) < (long)p->size)
            st_u16((char *)dst + 2 * n, 0);
        return p->bits + 16 * (long)n;
    }
    case CE_FT_ARRAY:            /* 0x4D5DC0 */
    case CE_FT_STRUCTURE: {      /* 0x4D6270 */
        int n = p->count, i;
        long total = 0;
        unsigned char changed[64];
        if (base) {
            if (n > 64)
                return CE_ERR_UNSUPPORTED;
            for (i = 0; i < n; i++) {
                R(v, 1);
                changed[i] = (unsigned char)v;
            }
            total = n;
        }
        for (i = 0; i < n; i++) {
            const ce_field_props *ep;
            char *d;
            const char *b;
            long r;
            if (p->type == CE_FT_ARRAY) {
                ep = p->element;
                d = (char *)dst + p->stride * (unsigned)i;
                b = base ? (const char *)base + p->stride * (unsigned)i : NULL;
            } else {
                ep = p->members[i].props;
                d = (char *)dst + p->members[i].offset;
                b = base ? (const char *)base + p->members[i].baseline_offset : NULL;
            }
            if (base && !changed[i]) {
                /* arrays copy the baseline element; structures leave the member as is
                   (the body was prepared from the baseline, so the result is the same) */
                if (p->type == CE_FT_ARRAY && b != d)
                    memmove(d, b, p->stride);
                continue;
            }
            r = dec_field(c, ep, b, d);
            if (r < 0)
                return r;
            total += r;
        }
        return total;
    }
    case CE_FT_POINTER: {        /* 0x4D64F0 */
        void *d = ld_ptr(dst);
        const void *b = base ? ld_ptr(base) : NULL;
        if (!d)
            return CE_ERR_POINTER;
        return dec_field(c, p->element, b, d);
    }
    case CE_FT_TRANSLATED_INDEX: /* 0x4D66A0 */
        R(v, p->bits);
        st_u32(dst, v);
        return p->bits;
    case CE_FT_POINT:            /* 0x4D6AF0 */
    case CE_FT_VECTOR: {
        int n = p->count, i;
        unsigned char changed[4] = { 1, 1, 1, 1 };
        long total = 0;
        if (base) {
            for (i = 0; i < n; i++) {
                R(v, 1);
                changed[i] = (unsigned char)v;
            }
            total = n;
        }
        for (i = 0; i < n; i++) {
            if (!changed[i]) {
                st_u32((char *)dst + 4 * i, ld_u32((const char *)base + 4 * i));
                continue;
            }
            R(v, 32);
            st_u32((char *)dst + 4 * i, v);
            total += 32;
        }
        return total;
    }
    case CE_FT_FLAGS: {          /* 0x4D6E60: keeps the bits it does not send */
        unsigned long cur = ld_u32(dst);
        int i;
        long bits = 0;
        for (i = 0; i < p->count; i++) {
            unsigned long m = 1ul << i;
            if (!(p->valid_mask & m))
                continue;
            R(v, 1);
            if (v) cur |= m;
            else cur &= ~m;
            bits++;
        }
        st_u32(dst, cur);
        return bits;
    }
    case CE_FT_GRENADE_COUNTS:   /* 0x4D7100 */
        R(v, 6);
        st_u8(dst, v >> 3);
        st_u8((char *)dst + 1, v & 7);
        return 6;
    case CE_FT_FIXED_WIDTH:      /* 0x4D7050 */
        R(v, p->bits);
        st_f32(dst, (float)(u32d(v) / u32d(p->range)));
        return p->bits;
    case CE_FT_FIXED_WIDTH_NORMAL_VECTOR:
        return dec_normal(c, p, dst);
    case CE_FT_LOCALITY_REFERENCE_POSITION:
        return dec_locality(c, base, dst);
    case CE_FT_DIGITAL_THROTTLE: /* 0x4D7C70 */
        R(v, 4);
        throttle_vector(v, dst, p->size);
        return 4;
    case CE_FT_FIXED_WIDTH_WEAPON_INDEX:   /* 0x4D7D20 */
        R(v, 3);
        st_u16(dst, (v & 4) ? 0xFFFFul : (v & 3));
        return 3;
    case CE_FT_FIXED_WIDTH_GRENADE_INDEX:  /* 0x4D7DD0 */
        R(v, 2);
        st_u16(dst, (v & 2) ? 0xFFFFul : (v & 1));
        return 2;
    case CE_FT_SMART_VECTOR:
        return dec_smart(c, p, base, dst);
    default:
        return CE_ERR_UNSUPPORTED;
    }
}

long ce_field_encode(const ce_delta_config *cfg, ce_bitstream *bs, const ce_field_props *p,
                     const void *base, const void *src)
{
    codec c;
    c.cfg = cfg;
    c.bs = bs;
    return enc_field(&c, p, base, src);
}

long ce_field_decode(const ce_delta_config *cfg, ce_bitstream *bs, const ce_field_props *p,
                     const void *base, void *dst)
{
    codec c;
    c.cfg = cfg;
    c.bs = bs;
    return dec_field(&c, p, base, dst);
}

/* ------------------------------------------------------------------ public: config */

void ce_delta_config_default(ce_delta_config *cfg)
{
    cfg->encoding_class = CE_CLASS_INTERNET;
    cfg->changeover = 0;
    cfg->protocol_version = 0;
    cfg->parameters_flag = 0;
    cfg->locality_bits_full = 25;
    cfg->locality_bits_delta = 9;
    cfg->locality_bits_lan = 31;
    cfg->locality_delta_cutoff = 8.0f;
    cfg->locality_min_move = 0.00999999977648258209f;
}

const ce_message_def *ce_delta_message(int type)
{
    if (type < 0 || type >= CE_MESSAGE_COUNT)
        return NULL;
    return &ce_message_defs[type];
}

const char *ce_delta_error_text(int err)
{
    switch (err) {
    case CE_OK: return "ok";
    case CE_ERR_OVERFLOW: return "buffer overflow / read past the end";
    case CE_ERR_TYPE: return "bad message type";
    case CE_ERR_MODE: return "bad mode";
    case CE_ERR_ITERATIONS: return "bad iteration count";
    case CE_ERR_STRING: return "string too long";
    case CE_ERR_POINTER: return "NULL pointer field";
    case CE_ERR_VERSION: return "protocol version mismatch";
    case CE_ERR_UNSUPPORTED: return "unsupported field type";
    case CE_ERR_EMPTY: return "present field decoded to nothing";
    case CE_ERR_STREAM: return "bad stream framing";
    default: return "?";
    }
}

/* ------------------------------------------------------------------ public: streams */

int ce_stream_open(ce_bitstream *bs, const void *data, unsigned long bytes)
{
    unsigned long size;
    ce_bs_init_read(bs, data, bytes * 8ul);
    size = ce_bs_read(bs, 11);
    if (bs->overflow || size * 8ul < 11ul || size > bytes)
        return CE_ERR_STREAM;
    bs->limit = size * 8ul;
    return (int)size;
}

int ce_stream_next(ce_bitstream *bs)
{
    if (bs->limit - bs->pos < 8ul)   /* client dispatcher 0x4C59E0 */
        return CE_ITEM_END;
    return ce_bs_read_bit(bs) ? CE_ITEM_DELTA : CE_ITEM_PACKET;
}

int ce_stream_read_packet(ce_bitstream *bs, unsigned char *out, int out_size)
{
    unsigned long header = ce_bs_read(bs, 16), size, i;
    if (bs->overflow)
        return CE_ERR_OVERFLOW;
    size = header >> 4;
    if (((header >> 2) & 3ul) != 3ul || size < 2)
        return CE_ERR_STREAM;
    if (out && out_size >= 2) {
        out[0] = (unsigned char)header;
        out[1] = (unsigned char)(header >> 8);
    }
    for (i = 2; i < size; i++) {
        unsigned long b = ce_bs_read(bs, 8);
        if (bs->overflow)
            return CE_ERR_OVERFLOW;
        if (out && (long)i < out_size)
            out[i] = (unsigned char)b;
    }
    return (int)size;
}

int ce_stream_begin(ce_bitstream *bs, void *buffer, unsigned long bytes)
{
    if (bytes > CE_STREAM_MAX_BYTES)
        bytes = CE_STREAM_MAX_BYTES;
    ce_bs_init(bs, buffer, bytes * 8ul);
    return ce_bs_write(bs, 0, 11) < 0 ? CE_ERR_OVERFLOW : CE_OK;
}

int ce_stream_put_kind(ce_bitstream *bs, int kind)
{
    return ce_bs_write_bit(bs, kind) < 0 ? CE_ERR_OVERFLOW : CE_OK;
}

long ce_stream_finish(ce_bitstream *bs)
{
    unsigned long bytes = (bs->pos + 7ul) >> 3;
    while (bs->pos & 7ul)
        if (ce_bs_write_bit(bs, 0) < 0)
            return CE_ERR_OVERFLOW;
    if (ce_bs_poke(bs, 0, bytes, 11) < 0)
        return CE_ERR_OVERFLOW;
    return (long)bytes;
}

/* ------------------------------------------------------------------ public: messages */

long ce_delta_encode(const ce_delta_config *cfg, ce_bitstream *bs, int mode, int type,
                     const void *const *headers, const void *const *bodies,
                     const void *const *baselines, int iterations, int keep_empty)
{
    const ce_message_def *def = ce_delta_message(type);
    codec c;
    unsigned long start = bs->pos, count_at, pos;
    int count_bits, kept = 0, i, j;
    if (!def)
        return CE_ERR_TYPE;
    if (mode != 0 && mode != 1)
        return CE_ERR_MODE;
    if (iterations < 1 || iterations > def->max_iterations)
        return CE_ERR_ITERATIONS;
    c.cfg = cfg;
    c.bs = bs;
    /* iteration independent header (0x4D97A0) */
    if (ce_bs_write(bs, (unsigned long)mode, 1) < 0 || ce_bs_write(bs, (unsigned long)type, 6) < 0)
        goto overflow;
    if (cfg->changeover) {
        if (ce_bs_write(bs, (unsigned long)(cfg->parameters_flag != 0), 1) < 0 ||
            ce_bs_write(bs, (unsigned long)cfg->protocol_version, 2) < 0)
            goto overflow;
    }
    count_at = bs->pos;
    count_bits = def->max_iterations > 1 ? bits_for((unsigned long)(def->max_iterations - 1)) : 0;
    if (ce_bs_seek(bs, count_at + (unsigned long)count_bits) < 0)
        goto overflow;
    for (i = 0; i < iterations; i++) {
        const void *header = headers ? headers[i] : NULL;
        const void *body = bodies[i];
        const void *base = (mode == 1 && baselines) ? baselines[i] : NULL;
        unsigned long it_start = bs->pos, presence = bs->pos;
        long body_bits = 0;
        int header_ok = 1;
        if (mode == 1 && ce_bs_seek(bs, presence + (unsigned long)def->body_count) < 0)
            goto overflow;
        for (j = 0; j < def->header_count; j++) {   /* 0x4D96A0 */
            const ce_field_ref *f = &def->header[j];
            long r = enc_field(&c, f->props, NULL, (const char *)header + f->offset);
            if (r < 0) {
                bs->pos = start;
                return r;
            }
            if (r == 0)
                header_ok = 0;
        }
        if (header_ok) {
            for (j = 0; j < def->body_count; j++) {   /* 0x4D9880 */
                const ce_field_ref *f = &def->body[j];
                long r = enc_field(&c, f->props, base ? (const char *)base + f->baseline_offset : NULL,
                                   (const char *)body + f->offset);
                if (r < 0) {
                    bs->pos = start;
                    return r;
                }
                if (mode == 1 && ce_bs_poke(bs, presence + (unsigned long)j, r != 0, 1) < 0)
                    goto overflow;
                body_bits += r;
            }
        }
        if (body_bits > 0 || keep_empty)
            kept++;
        else
            bs->pos = it_start;    /* the next iteration overwrites this one */
    }
    if (!kept) {
        bs->pos = start;
        return 0;
    }
    pos = bs->pos;
    if (count_bits && ce_bs_poke(bs, count_at, (unsigned long)(kept - 1), count_bits) < 0)
        goto overflow;
    return (long)(pos - start);
overflow:
    bs->pos = start;
    return CE_ERR_OVERFLOW;
}

long ce_delta_encode1(const ce_delta_config *cfg, ce_bitstream *bs, int mode, int type,
                      const void *header, const void *body, const void *baseline)
{
    const void *h[1], *b[1], *base[1];
    h[0] = header;
    b[0] = body;
    base[0] = baseline;
    return ce_delta_encode(cfg, bs, mode, type, h, b, base, 1, 0);
}

int ce_delta_read_header(const ce_delta_config *cfg, ce_bitstream *bs, ce_message_header *h)
{
    unsigned long start = bs->pos, v;
    memset(h, 0, sizeof *h);
    h->mode = (int)ce_bs_read(bs, 1);
    h->type = (int)ce_bs_read(bs, 6);
    if (bs->overflow)
        return CE_ERR_OVERFLOW;
    h->def = ce_delta_message(h->type);
    if (!h->def)
        return CE_ERR_TYPE;
    if (cfg->changeover) {
        h->parameters_flag = ce_bs_read_bit(bs);
        h->protocol_version = (int)ce_bs_read(bs, 2);
        if (bs->overflow)
            return CE_ERR_OVERFLOW;
    }
    if (h->def->max_iterations > 1) {
        v = ce_bs_read(bs, bits_for((unsigned long)(h->def->max_iterations - 1)));
        if (bs->overflow)
            return CE_ERR_OVERFLOW;
        h->iterations = (int)v + 1;
        if (h->iterations > h->def->max_iterations)
            return CE_ERR_ITERATIONS;
    } else {
        h->iterations = 1;
    }
    h->bits = bs->pos - start;
    if (cfg->changeover && h->protocol_version != cfg->protocol_version && !h->parameters_flag)
        return CE_ERR_VERSION;
    return CE_OK;
}

int ce_delta_read_iteration_header(const ce_delta_config *cfg, ce_bitstream *bs,
                                   const ce_message_header *h, unsigned char present[64],
                                   void *header)
{
    const ce_message_def *def = h->def;
    codec c;
    unsigned long start = bs->pos;
    int j;
    c.cfg = cfg;
    c.bs = bs;
    for (j = 0; j < 64; j++)
        present[j] = (unsigned char)(h->mode == 0 && j < def->body_count);
    if (h->mode == 1)
        for (j = 0; j < def->body_count; j++) {
            present[j] = (unsigned char)ce_bs_read_bit(bs);
            if (bs->overflow)
                return CE_ERR_OVERFLOW;
        }
    if (def->header_count && header)
        memset(header, 0, def->header_size);
    for (j = 0; j < def->header_count; j++) {
        const ce_field_ref *f = &def->header[j];
        long r = dec_field(&c, f->props, NULL, (char *)header + f->offset);
        if (r < 0)
            return (int)r;
    }
    return (int)(bs->pos - start);
}

static void prepare_body(const ce_message_def *def, const void *baseline, void *body)
{
    void *saved[8];
    int j, n = 0;
    for (j = 0; j < def->body_count && n < 8; j++)
        if (def->body[j].props->type == CE_FT_POINTER)
            saved[n++] = ld_ptr((const char *)body + def->body[j].offset);
    if (baseline) {
        if (baseline != body)
            memmove(body, baseline, def->body_size);
    } else {
        memset(body, 0, def->body_size);
    }
    for (j = 0, n = 0; j < def->body_count && n < 8; j++)
        if (def->body[j].props->type == CE_FT_POINTER)
            memcpy((char *)body + def->body[j].offset, &saved[n++], sizeof(void *));
}

int ce_delta_read_iteration_body(const ce_delta_config *cfg, ce_bitstream *bs,
                                 const ce_message_header *h, const unsigned char present[64],
                                 const void *baseline, void *body)
{
    const ce_message_def *def = h->def;
    codec c;
    unsigned long start = bs->pos;
    int j;
    if (h->mode == 0)
        baseline = NULL;    /* 0x4D9030: stateless bodies decode without baseline */
    c.cfg = cfg;
    c.bs = bs;
    prepare_body(def, baseline, body);
    for (j = 0; j < def->body_count; j++) {
        const ce_field_ref *f = &def->body[j];
        long r;
        if (!present[j])
            continue;
        r = dec_field(&c, f->props, baseline ? (const char *)body + f->baseline_offset : NULL,
                      (char *)body + f->offset);
        if (r < 0)
            return (int)r;
        if (r == 0)
            return CE_ERR_EMPTY;
    }
    return (int)(bs->pos - start);
}

int ce_delta_decode(const ce_delta_config *cfg, ce_bitstream *bs, ce_message_header *h,
                    void *header_buf, void *body_buf,
                    ce_baseline_fn baseline_fn, ce_iteration_fn each, void *user)
{
    unsigned long start = bs->pos;
    unsigned char present[64];
    int err, i;
    err = ce_delta_read_header(cfg, bs, h);
    if (err < 0)
        return err;
    for (i = 0; i < h->iterations; i++) {
        const void *base = NULL;
        err = ce_delta_read_iteration_header(cfg, bs, h, present, header_buf);
        if (err < 0)
            return err;
        if (h->mode == 1 && baseline_fn)
            base = baseline_fn(user, h, i, header_buf);
        err = ce_delta_read_iteration_body(cfg, bs, h, present, base, body_buf);
        if (err < 0)
            return err;
        if (each) {
            err = each(user, h, i, present, header_buf, body_buf);
            if (err < 0)
                return err;
        }
    }
    h->bits = bs->pos - start;
    return CE_OK;
}

/* ------------------------------------------------------------------ public: access */

static int vec_count(const ce_field_props *p)
{
    if (p->type == CE_FT_POINT || p->type == CE_FT_VECTOR)
        return p->count;
    return p->size >= 12 ? 3 : (int)(p->size / 4);
}


const ce_field_ref *ce_delta_field(const ce_message_def *def, int part, const char *name)
{
    const ce_field_ref *f = part ? def->body : def->header;
    int n = part ? def->body_count : def->header_count, i;
    for (i = 0; i < n; i++)
        if (strcmp(f[i].name, name) == 0)
            return &f[i];
    return NULL;
}

unsigned long ce_get_uint(const ce_field_props *p, const void *at)
{
    switch (p->type) {
    case CE_FT_INTEGER:
        return int_load(p, at);
    case CE_FT_BOOLEAN:
        return ld_u8(at);
    case CE_FT_FIXED_WIDTH_WEAPON_INDEX:
    case CE_FT_FIXED_WIDTH_GRENADE_INDEX:
        return ld_u16(at);
    case CE_FT_GRENADE_COUNTS:
        return ld_u8(at) | (ld_u8((const char *)at + 1) << 8);
    default:
        return ld_u32(at);
    }
}

long ce_get_int(const ce_field_props *p, const void *at)
{
    unsigned long v = ce_get_uint(p, at);
    unsigned size = p->type == CE_FT_INTEGER ? p->size :
                    (p->type == CE_FT_BOOLEAN ? 1u :
                    (p->type == CE_FT_FIXED_WIDTH_WEAPON_INDEX || p->type == CE_FT_FIXED_WIDTH_GRENADE_INDEX) ? 2u : 4u);
    if (size == 1) return (long)(signed char)v;
    if (size == 2) return (long)(short)v;
    return (long)(int)v;
}

float ce_get_real(const ce_field_props *p, const void *at)
{
    (void)p;
    return ld_f32(at);
}

void ce_get_vector(const ce_field_props *p, const void *at, float out[3])
{
    int n = vec_count(p), i;
    out[2] = 0.0f;
    for (i = 0; i < n; i++)
        out[i] = ld_f32((const char *)at + 4 * i);
}

void ce_set_uint(const ce_field_props *p, void *at, unsigned long v)
{
    switch (p->type) {
    case CE_FT_INTEGER:
        int_store(p, at, v);
        break;
    case CE_FT_BOOLEAN:
        st_u8(at, v);
        break;
    case CE_FT_FIXED_WIDTH_WEAPON_INDEX:
    case CE_FT_FIXED_WIDTH_GRENADE_INDEX:
        st_u16(at, v);
        break;
    case CE_FT_GRENADE_COUNTS:
        st_u8(at, v & 0xFF);
        st_u8((char *)at + 1, (v >> 8) & 0xFF);
        break;
    default:
        st_u32(at, v);
        break;
    }
}

void ce_set_real(const ce_field_props *p, void *at, float v)
{
    (void)p;
    st_f32(at, v);
}

void ce_set_vector(const ce_field_props *p, void *at, const float v[3])
{
    int n = vec_count(p), i;
    for (i = 0; i < n; i++)
        st_f32((char *)at + 4 * i, v[i]);
}

/* tiny formatter (no stdio dependency) */
typedef struct fmt_buf {
    char *out;
    int size, len;
} fmt_buf;

static void put_c(fmt_buf *f, char ch)
{
    if (f->len + 1 < f->size)
        f->out[f->len] = ch;
    f->len++;
}

static void put_s(fmt_buf *f, const char *s)
{
    while (*s)
        put_c(f, *s++);
}

static void put_u(fmt_buf *f, unsigned long v)
{
    char tmp[12];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10ul);
        v /= 10ul;
    } while (v);
    while (n)
        put_c(f, tmp[--n]);
}

static void put_i(fmt_buf *f, long v)
{
    if (v < 0) {
        put_c(f, '-');
        put_u(f, (unsigned long)(-(v + 1)) + 1ul);
    } else {
        put_u(f, (unsigned long)v);
    }
}

static void put_f(fmt_buf *f, float x)
{
    double v = x;
    unsigned long ip, fp;
    if (v != v) {
        put_s(f, "nan");
        return;
    }
    if (v < 0) {
        put_c(f, '-');
        v = -v;
    }
    if (v >= 4e9) {
        put_s(f, "huge");
        return;
    }
    ip = (unsigned long)v;
    fp = (unsigned long)((v - (double)ip) * 1000000.0 + 0.5);
    if (fp >= 1000000ul) {
        ip++;
        fp -= 1000000ul;
    }
    put_u(f, ip);
    put_c(f, '.');
    {
        char tmp[6];
        int k;
        for (k = 5; k >= 0; k--) {
            tmp[k] = (char)('0' + fp % 10ul);
            fp /= 10ul;
        }
        for (k = 0; k < 6; k++)
            put_c(f, tmp[k]);
    }
}

static void fmt_value(fmt_buf *f, const ce_field_props *p, const void *at)
{
    int i;
    switch (p->type) {
    case CE_FT_REAL:
    case CE_FT_ANGLE:
    case CE_FT_FIXED_WIDTH:
        put_f(f, ld_f32(at));
        break;
    case CE_FT_POINT:
    case CE_FT_VECTOR:
    case CE_FT_FIXED_WIDTH_NORMAL_VECTOR:
    case CE_FT_LOCALITY_REFERENCE_POSITION:
    case CE_FT_DIGITAL_THROTTLE:
    case CE_FT_SMART_VECTOR:
    case CE_FT_ITEM_PLACEMENT_POSITION: {
        int n = vec_count(p);
        put_c(f, '(');
        for (i = 0; i < n; i++) {
            if (i)
                put_s(f, ", ");
            put_f(f, ld_f32((const char *)at + 4 * i));
        }
        put_c(f, ')');
        break;
    }
    case CE_FT_ASCII_STRING:
        put_c(f, '"');
        for (i = 0; i <= p->count && ((const char *)at)[i]; i++)
            put_c(f, ((const char *)at)[i]);
        put_c(f, '"');
        break;
    case CE_FT_WIDE_STRING:
        put_s(f, "L\"");
        for (i = 0; i <= p->count; i++) {
            unsigned long ch = ld_u16((const char *)at + 2 * i);
            if (!ch)
                break;
            put_c(f, (char)(ch < 128 ? ch : '?'));
        }
        put_c(f, '"');
        break;
    case CE_FT_ARRAY:
        put_c(f, '[');
        for (i = 0; i < p->count; i++) {
            if (i)
                put_s(f, ", ");
            fmt_value(f, p->element, (const char *)at + p->stride * (unsigned)i);
        }
        put_c(f, ']');
        break;
    case CE_FT_STRUCTURE:
        put_c(f, '{');
        for (i = 0; i < p->count; i++) {
            if (i)
                put_s(f, ", ");
            put_s(f, p->members[i].name);
            put_s(f, "=");
            fmt_value(f, p->members[i].props, (const char *)at + p->members[i].offset);
        }
        put_c(f, '}');
        break;
    case CE_FT_POINTER: {
        const void *d = ld_ptr(at);
        if (d)
            fmt_value(f, p->element, d);
        else
            put_s(f, "NULL");
        break;
    }
    case CE_FT_GRENADE_COUNTS:
        put_u(f, ld_u8(at));
        put_c(f, '/');
        put_u(f, ld_u8((const char *)at + 1));
        break;
    case CE_FT_FIXED_WIDTH_WEAPON_INDEX:
    case CE_FT_FIXED_WIDTH_GRENADE_INDEX:
        put_i(f, ce_get_int(p, at));
        break;
    case CE_FT_FLAGS:
        put_s(f, "0x");
        {
            unsigned long v = ld_u32(at);
            int k;
            for (k = 28; k >= 0; k -= 4)
                put_c(f, "0123456789ABCDEF"[(v >> k) & 15]);
        }
        break;
    default:
        put_u(f, ce_get_uint(p, at));
        break;
    }
}

int ce_format_value(const ce_field_props *p, const void *at, char *out, int size)
{
    fmt_buf f;
    f.out = out;
    f.size = size;
    f.len = 0;
    fmt_value(&f, p, at);
    if (size > 0)
        out[f.len < size ? f.len : size - 1] = 0;
    return f.len;
}

static void visit(const ce_field_props *p, const void *at, char *path, int len, int cap,
                  ce_visit_fn fn, void *user)
{
    int i;
    fn(user, path, p, at);
    if (p->type == CE_FT_ARRAY || p->type == CE_FT_STRUCTURE) {
        for (i = 0; i < p->count; i++) {
            fmt_buf f;
            const ce_field_props *ep;
            const char *ea;
            f.out = path;
            f.size = cap;
            f.len = len;
            if (p->type == CE_FT_ARRAY) {
                put_c(&f, '[');
                put_u(&f, (unsigned long)i);
                put_c(&f, ']');
                ep = p->element;
                ea = (const char *)at + p->stride * (unsigned)i;
            } else {
                put_c(&f, '.');
                put_s(&f, p->members[i].name);
                ep = p->members[i].props;
                ea = (const char *)at + p->members[i].offset;
            }
            path[f.len < cap ? f.len : cap - 1] = 0;
            visit(ep, ea, path, f.len < cap ? f.len : cap - 1, cap, fn, user);
            path[len] = 0;
        }
    }
}

void ce_delta_visit(const ce_message_def *def, int part, const void *data, ce_visit_fn fn, void *user)
{
    const ce_field_ref *f = part ? def->body : def->header;
    int n = part ? def->body_count : def->header_count, i;
    char path[128];
    for (i = 0; i < n; i++) {
        int len = 0;
        while (f[i].name[len] && len < 100) {
            path[len] = f[i].name[len];
            len++;
        }
        path[len] = 0;
        visit(f[i].props, (const char *)data + f[i].offset, path, len, (int)sizeof path, fn, user);
    }
}
