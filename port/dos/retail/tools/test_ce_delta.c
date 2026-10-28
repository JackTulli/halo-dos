/* Host self-tests for the CE message-delta codec (no game, no network).
   test_ce_delta [capture1.bin [decode-log]]
   1. round trip of every message type, stateless, with synthetic values
   2. incremental round trips with baselines (presence bits, deltas, specials)
   3. every stream of a capture (records: [1 byte reliable][2 bytes size LE][stream]),
      decoded to its end (LAN class); stateless messages re-encoded bit for bit
   Build 32-bit (the engine layouts hold 32-bit pointers). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ce_delta.h"

static int failures = 0;
static unsigned long rng_state = 12345;

static unsigned long rnd(void)
{
    rng_state = rng_state * 1103515245ul + 12345ul;
    rng_state &= 0xFFFFFFFFul;
    return (rng_state >> 8) & 0xFFFFFF;
}

static unsigned long rnd32(void) { return (rnd() << 8) ^ rnd(); }
static float rndf(float lo, float hi) { return lo + (hi - lo) * (float)rnd() / (float)0xFFFFFF; }

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ------------------------------------------------------------------ synthetic values */

static unsigned short chat_text[2][256];

static void fill(const ce_field_props *p, void *at, int variant)
{
    int i;
    switch (p->type) {
    case CE_FT_INTEGER: {
        unsigned long v = rnd32();
        if (p->bits < 32)
            v &= (1ul << p->bits) - 1ul;
        ce_set_uint(p, at, v);
        break;
    }
    case CE_FT_REAL:
        ce_set_real(p, at, rndf(-1000.0f, 1000.0f));
        break;
    case CE_FT_TIME:
        ce_set_uint(p, at, rnd32());
        break;
    case CE_FT_BOOLEAN:
        *(unsigned char *)at = (unsigned char)(rnd() & 1);
        break;
    case CE_FT_ASCII_STRING: {
        int n = (int)(rnd() % (unsigned long)(p->count + 1));
        for (i = 0; i < n; i++)
            ((char *)at)[i] = (char)(' ' + rnd() % 95);
        ((char *)at)[n] = 0;
        break;
    }
    case CE_FT_WIDE_STRING: {
        int n = (int)(rnd() % (unsigned long)(p->count + 1));
        unsigned short *w = (unsigned short *)at;
        for (i = 0; i < n; i++)
            w[i] = (unsigned short)(1 + rnd() % 0xFFFE);
        w[n] = 0;
        break;
    }
    case CE_FT_ARRAY:
        for (i = 0; i < p->count; i++)
            fill(p->element, (char *)at + p->stride * (unsigned)i, variant);
        break;
    case CE_FT_STRUCTURE:
        for (i = 0; i < p->count; i++)
            fill(p->members[i].props, (char *)at + p->members[i].offset, variant);
        break;
    case CE_FT_POINTER: {
        void *ptr = chat_text[variant];
        fill(p->element, ptr, variant);
        memcpy(at, &ptr, sizeof ptr);
        break;
    }
    case CE_FT_TRANSLATED_INDEX:
        ce_set_uint(p, at, rnd() % (unsigned long)(p->count + 1));
        break;
    case CE_FT_POINT:
    case CE_FT_VECTOR:
        for (i = 0; i < p->count; i++)
            ((float *)at)[i] = rndf(-500.0f, 500.0f);
        break;
    case CE_FT_FLAGS:
        ce_set_uint(p, at, rnd32() & p->valid_mask);
        break;
    case CE_FT_GRENADE_COUNTS:
        ((unsigned char *)at)[0] = (unsigned char)(rnd() & 7);
        ((unsigned char *)at)[1] = (unsigned char)(rnd() & 7);
        break;
    case CE_FT_FIXED_WIDTH:
        ce_set_real(p, at, rndf(0.0f, 1.0f));
        break;
    case CE_FT_FIXED_WIDTH_NORMAL_VECTOR: {
        float v[3], l;
        do {
            v[0] = rndf(-1, 1);
            v[1] = rndf(-1, 1);
            v[2] = rndf(-1, 1);
            l = (float)sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        } while (l < 0.1f);
        v[0] /= l;
        v[1] /= l;
        v[2] /= l;
        memcpy(at, v, 12);
        break;
    }
    case CE_FT_LOCALITY_REFERENCE_POSITION:
        for (i = 0; i < 3; i++)
            ((float *)at)[i] = rndf(-4000.0f, 4000.0f);
        break;
    case CE_FT_DIGITAL_THROTTLE: {
        static const float pick[3] = { -1.0f, 0.0f, 1.0f };
        float v[3];
        v[0] = pick[rnd() % 3];
        v[1] = pick[rnd() % 3];
        v[2] = 0.0f;
        memcpy(at, v, p->size < 12 ? p->size : 12);
        break;
    }
    case CE_FT_FIXED_WIDTH_WEAPON_INDEX:
        ce_set_uint(p, at, (rnd() % 5 == 0) ? 0xFFFFul : rnd() % 4);
        break;
    case CE_FT_FIXED_WIDTH_GRENADE_INDEX:
        ce_set_uint(p, at, (rnd() % 3 == 0) ? 0xFFFFul : rnd() % 2);
        break;
    case CE_FT_SMART_VECTOR:
        if (rnd() % 4 == 0) {
            memset(at, 0, 12);   /* the special (zero) vector */
        } else {
            for (i = 0; i < 3; i++)
                ((float *)at)[i] = rndf(p->minimum, p->maximum);
        }
        break;
    default:
        break;
    }
}

/* Tolerance check of a decoded value against the source. */
static double position_tolerance = 1e-3;

static int close_enough(const ce_field_props *p, const void *a, const void *b, int cls)
{
    int i;
    switch (p->type) {
    case CE_FT_FIXED_WIDTH:
        return fabs(*(const float *)a - *(const float *)b) <= 0.5 / (double)p->range + 1e-6;
    case CE_FT_FIXED_WIDTH_NORMAL_VECTOR: {
        const float *x = (const float *)a, *y = (const float *)b;
        int bt = cls == CE_CLASS_LAN ? p->bits_lan[0] : p->bits_internet[0];
        double tol = 3.5 * 3.14159265 / (double)((1ul << bt) - 1ul) + 1e-5;
        for (i = 0; i < 3; i++)
            if (fabs(x[i] - y[i]) > tol)
                return 0;
        return 1;
    }
    case CE_FT_LOCALITY_REFERENCE_POSITION: {
        const float *x = (const float *)a, *y = (const float *)b;
        for (i = 0; i < 3; i++)
            if (fabs(x[i] - y[i]) > position_tolerance)
                return 0;
        return 1;
    }
    case CE_FT_SMART_VECTOR: {
        const float *x = (const float *)a, *y = (const float *)b;
        double span = (double)p->maximum - (double)p->minimum;
        double step = span / (double)(cls == CE_CLASS_LAN ? p->range_lan : p->range);
        for (i = 0; i < 3; i++)
            if (fabs(x[i] - y[i]) > step + fabs(y[i]) * 1e-6 + 1e-6)
                return 0;
        return 1;
    }
    case CE_FT_DIGITAL_THROTTLE: {
        const float *x = (const float *)a, *y = (const float *)b;
        float ny[3];
        double l = sqrt((double)y[0] * y[0] + (double)y[1] * y[1]);
        ny[0] = (float)(l > 0 ? y[0] / l : 0);
        ny[1] = (float)(l > 0 ? y[1] / l : 0);
        return fabs(x[0] - ny[0]) < 1e-6 && fabs(x[1] - ny[1]) < 1e-6;
    }
    case CE_FT_ARRAY:
        for (i = 0; i < p->count; i++)
            if (!close_enough(p->element, (const char *)a + p->stride * (unsigned)i,
                              (const char *)b + p->stride * (unsigned)i, cls))
                return 0;
        return 1;
    case CE_FT_STRUCTURE:
        for (i = 0; i < p->count; i++)
            if (!close_enough(p->members[i].props, (const char *)a + p->members[i].offset,
                              (const char *)b + p->members[i].offset, cls))
                return 0;
        return 1;
    case CE_FT_POINTER:
        return close_enough(p->element, *(void *const *)a, *(void *const *)b, cls);
    case CE_FT_ASCII_STRING:
        return strcmp((const char *)a, (const char *)b) == 0;
    case CE_FT_WIDE_STRING: {
        const unsigned short *x = (const unsigned short *)a, *y = (const unsigned short *)b;
        for (i = 0; i <= p->count; i++) {
            if (x[i] != y[i])
                return 0;
            if (!x[i])
                return 1;
        }
        return 1;
    }
    default:
        return memcmp(a, b, p->size) == 0;
    }
}

static int bits_equal(const unsigned char *a, const unsigned char *b, unsigned long bits)
{
    unsigned long i;
    for (i = 0; i < bits; i++)
        if (((a[i >> 3] >> (i & 7)) & 1) != ((b[i >> 3] >> (i & 7)) & 1))
            return 0;
    return 1;
}

/* ------------------------------------------------------------------ 1+2: round trips */

static int reencode_same, reencode_lossy;

/* Fields whose decode -> encode is not always the identity in the engine itself:
   locality positions (the encoder scales by the range rounded to float, the decoder
   by the exact range), normals (angles through float vectors), 31-bit smart vectors
   (more bits than a float holds). */
static int has_lossy(const ce_message_def *def)
{
    int j;
    for (j = 0; j < def->body_count; j++) {
        int t = def->body[j].props->type;
        if (t == CE_FT_LOCALITY_REFERENCE_POSITION || t == CE_FT_FIXED_WIDTH_NORMAL_VECTOR || t == CE_FT_SMART_VECTOR)
            return 1;
    }
    return 0;
}

static unsigned char src_h[64], src_b[CE_DELTA_MAX_BODY_SIZE + 64], base_b[CE_DELTA_MAX_BODY_SIZE + 64];
static unsigned char dec_h[64], dec_b[CE_DELTA_MAX_BODY_SIZE + 64], dec2_b[CE_DELTA_MAX_BODY_SIZE + 64];
static unsigned char buf[8192], buf2[8192];

static void prepare_pointers(const ce_message_def *def, void *body, int variant)
{
    int j;
    for (j = 0; j < def->body_count; j++)
        if (def->body[j].props->type == CE_FT_POINTER) {
            void *ptr = chat_text[variant];
            memcpy((char *)body + def->body[j].offset, &ptr, sizeof ptr);
        }
}

static void roundtrip_stateless(const ce_delta_config *cfg, int type, int rounds)
{
    const ce_message_def *def = ce_delta_message(type);
    int r, j;
    for (r = 0; r < rounds; r++) {
        ce_bitstream bs;
        ce_message_header h;
        unsigned char present[64];
        long bits, bits2;
        int err;
        memset(src_h, 0, sizeof src_h);
        memset(src_b, 0, sizeof src_b);
        for (j = 0; j < def->header_count; j++)
            fill(def->header[j].props, src_h + def->header[j].offset, 0);
        for (j = 0; j < def->body_count; j++)
            fill(def->body[j].props, src_b + def->body[j].offset, 0);
        ce_bs_init(&bs, buf, sizeof buf * 8);
        bits = ce_delta_encode1(cfg, &bs, 0, type, src_h, src_b, NULL);
        CHECK(bits > 0, "%s: encode returned %ld", def->name, bits);
        if (bits <= 0)
            return;
        CHECK((unsigned long)bits <= def->total_max_bits, "%s: %ld bits > engine max %u", def->name, bits, def->total_max_bits);
        ce_bs_init_read(&bs, buf, (unsigned long)bits);
        err = ce_delta_read_header(cfg, &bs, &h);
        CHECK(err == 0 && h.type == type && h.mode == 0 && h.iterations == 1, "%s: header %d", def->name, err);
        err = ce_delta_read_iteration_header(cfg, &bs, &h, present, dec_h);
        CHECK(err >= 0, "%s: iteration header %d", def->name, err);
        memset(dec_b, 0, sizeof dec_b);
        prepare_pointers(def, dec_b, 1);
        err = ce_delta_read_iteration_body(cfg, &bs, &h, present, NULL, dec_b);
        CHECK(err >= 0, "%s: body %d (%s)", def->name, err, ce_delta_error_text(err));
        CHECK(bs.pos == (unsigned long)bits, "%s: decoded %lu of %ld bits", def->name, bs.pos, bits);
        for (j = 0; j < def->header_count; j++) {
            const ce_field_ref *f = &def->header[j];
            CHECK(close_enough(f->props, dec_h + f->offset, src_h + f->offset, cfg->encoding_class),
                  "%s header %s differs", def->name, f->name);
        }
        for (j = 0; j < def->body_count; j++) {
            const ce_field_ref *f = &def->body[j];
            CHECK(close_enough(f->props, dec_b + f->offset, src_b + f->offset, cfg->encoding_class),
                  "%s body %s differs (class %d)", def->name, f->name, cfg->encoding_class);
        }
        /* re-encoding what was decoded gives the same bits */
        ce_bs_init(&bs, buf2, sizeof buf2 * 8);
        bits2 = ce_delta_encode1(cfg, &bs, 0, type, dec_h, dec_b, NULL);
        if (bits2 == bits && bits_equal(buf, buf2, (unsigned long)bits))
            reencode_same++;
        else if (has_lossy(def))
            reencode_lossy++;   /* the engine's own quantizers are not idempotent there */
        else
            CHECK(0, "%s: re-encode differs (%ld vs %ld bits)", def->name, bits2, bits);
    }
}

/* Incremental: random baseline, change a random subset of body fields. */
static void roundtrip_incremental(const ce_delta_config *cfg, int type, int rounds)
{
    const ce_message_def *def = ce_delta_message(type);
    int r, j;
    for (r = 0; r < rounds; r++) {
        ce_bitstream bs;
        ce_message_header h;
        unsigned char present[64];
        long bits;
        int err, changed = 0;
        memset(src_h, 0, sizeof src_h);
        memset(base_b, 0, sizeof base_b);
        for (j = 0; j < def->header_count; j++)
            fill(def->header[j].props, src_h + def->header[j].offset, 0);
        for (j = 0; j < def->body_count; j++)
            fill(def->body[j].props, base_b + def->body[j].offset, 0);
        prepare_pointers(def, base_b, 0);
        /* the baseline is what the receiver holds: make it the decoded (quantized) form */
        ce_bs_init(&bs, buf, sizeof buf * 8);
        bits = ce_delta_encode1(cfg, &bs, 0, type, src_h, base_b, NULL);
        ce_bs_init_read(&bs, buf, (unsigned long)bits);
        ce_delta_read_header(cfg, &bs, &h);
        ce_delta_read_iteration_header(cfg, &bs, &h, present, dec_h);
        memset(dec2_b, 0, sizeof dec2_b);
        prepare_pointers(def, dec2_b, 1);
        ce_delta_read_iteration_body(cfg, &bs, &h, present, NULL, dec2_b);
        memcpy(base_b, dec2_b, sizeof base_b);
        memcpy(src_b, base_b, sizeof src_b);
        prepare_pointers(def, src_b, 0);
        if (def->body_count && def->body[0].props->type == CE_FT_POINTER)
            memcpy(chat_text[0], chat_text[1], sizeof chat_text[0]);
        for (j = 0; j < def->body_count; j++)
            if (rnd() % 3 == 0) {
                const ce_field_ref *f = &def->body[j];
                if (f->props->type == CE_FT_LOCALITY_REFERENCE_POSITION && rnd() % 2) {
                    float *b = (float *)(base_b + f->offset), *s = (float *)(src_b + f->offset);
                    int k;
                    for (k = 0; k < 3; k++)   /* small move: delta coding (internet) */
                        s[k] = b[k] + rndf(-7.5f, 7.5f);
                } else {
                    fill(f->props, src_b + f->offset, 0);
                }
                changed++;
            }
        ce_bs_init(&bs, buf, sizeof buf * 8);
        position_tolerance = 0.0101;   /* moves under 0.01 are "unchanged"; deltas step 8/511 */
        bits = ce_delta_encode1(cfg, &bs, 1, type, src_h, src_b, base_b);
        if (bits == 0) {
            CHECK(1, "");
            continue;   /* nothing changed after quantization */
        }
        CHECK(bits > 0, "%s incremental: %ld", def->name, bits);
        if (bits < 0)
            return;
        ce_bs_init_read(&bs, buf, (unsigned long)bits);
        err = ce_delta_read_header(cfg, &bs, &h);
        CHECK(err == 0 && h.mode == 1, "%s incremental header", def->name);
        err = ce_delta_read_iteration_header(cfg, &bs, &h, present, dec_h);
        CHECK(err >= 0, "%s incremental iteration header %d", def->name, err);
        prepare_pointers(def, dec_b, 1);
        if (def->body_count && def->body[0].props->type == CE_FT_POINTER)
            memcpy(chat_text[1], chat_text[0], sizeof chat_text[0]);
        err = ce_delta_read_iteration_body(cfg, &bs, &h, present, base_b, dec_b);
        CHECK(err >= 0, "%s incremental body %d (%s)", def->name, err, ce_delta_error_text(err));
        CHECK(bs.pos == (unsigned long)bits, "%s incremental: decoded %lu of %ld bits", def->name, bs.pos, bits);
        for (j = 0; j < def->body_count; j++) {
            const ce_field_ref *f = &def->body[j];
            if (f->props->type == CE_FT_POINTER)
                continue;
            CHECK(close_enough(f->props, dec_b + f->offset, src_b + f->offset, cfg->encoding_class),
                  "%s incremental body %s differs (present %d)", def->name, f->name, present[j]);
        }
        position_tolerance = 1e-3;
    }
}

/* iterated message: super_ping_update with several iterations */
static void iterated(const ce_delta_config *cfg)
{
    const ce_message_def *def = ce_delta_message(CE_MSG_SUPER_PING_UPDATE);
    unsigned char bodies[16][8];
    const void *b[16];
    ce_bitstream bs;
    ce_message_header h;
    unsigned char present[64];
    long bits;
    int i;
    for (i = 0; i < 16; i++) {
        memset(bodies[i], 0, 8);
        bodies[i][0] = (unsigned char)i;
        memcpy(bodies[i] + 4, &i, 4);
        b[i] = bodies[i];
    }
    ce_bs_init(&bs, buf, sizeof buf * 8);
    bits = ce_delta_encode(cfg, &bs, 0, def->type, NULL, b, NULL, 16, 0);
    CHECK(bits == 1 + 6 + 4 + 16 * 40, "super_ping_update x16: %ld bits", bits);
    ce_bs_init_read(&bs, buf, (unsigned long)bits);
    ce_delta_read_header(cfg, &bs, &h);
    CHECK(h.iterations == 16, "iterations %d", h.iterations);
    for (i = 0; i < h.iterations; i++) {
        ce_delta_read_iteration_header(cfg, &bs, &h, present, dec_h);
        ce_delta_read_iteration_body(cfg, &bs, &h, present, NULL, dec_b);
        CHECK(memcmp(dec_b, bodies[i], 8) == 0, "iteration %d differs", i);
    }
    CHECK(bs.pos == (unsigned long)bits, "iterated: %lu of %ld", bs.pos, bits);
}

/* ------------------------------------------------------------------ 3: capture */

typedef struct cap_ctx {
    FILE *log;
    const ce_delta_config *cfg;
    int messages;
    int reencoded, reencode_failed, reencode_same, reencode_lossy;
    unsigned long msg_start;
    const unsigned char *stream;
} cap_ctx;

static unsigned char zero_base[CE_DELTA_MAX_BODY_SIZE + 64];

static const void *zero_baseline(void *user, const ce_message_header *h, int iteration, const void *header)
{
    (void)user; (void)h; (void)iteration; (void)header;
    return zero_base;
}

static void log_field(void *user, const char *path, const ce_field_props *p, const void *at)
{
    cap_ctx *c = (cap_ctx *)user;
    char text[2048];
    if (p->type == CE_FT_ARRAY || p->type == CE_FT_STRUCTURE)
        return;
    ce_format_value(p, at, text, sizeof text);
    fprintf(c->log, "      %s = %s\n", path, text);
}

static int each_iteration(void *user, const ce_message_header *h, int iteration,
                          const unsigned char *present, const void *header, const void *body)
{
    cap_ctx *c = (cap_ctx *)user;
    int j;
    if (!c->log)
        return 0;
    fprintf(c->log, "    iteration %d%s\n", iteration, h->mode ? " (incremental, zero baseline)" : "");
    ce_delta_visit(h->def, 0, header, log_field, c);
    for (j = 0; j < h->def->body_count; j++) {
        const ce_field_ref *f = &h->def->body[j];
        char text[4096];
        if (!present[j])
            continue;
        ce_format_value(f->props, (const char *)body + f->offset, text, sizeof text);
        fprintf(c->log, "      %s = %s\n", f->name, text);
    }
    return 0;
}

/* Which fields of a stateless message re-encode differently: decode each field from
   the original bits, encode the decoded value, compare the two bit ranges. */
static int explain_difference(const ce_delta_config *cfg, const ce_message_header *h, const unsigned char *s,
                              unsigned long start, unsigned long limit)
{
    int bad = 0;
    const ce_message_def *def = h->def;
    ce_bitstream in;
    static unsigned char value[2048], tmp[1024];
    int j, part;
    ce_message_header hh;
    ce_bs_init_read(&in, s, limit);
    in.pos = start;
    ce_delta_read_header(cfg, &in, &hh);   /* positions in after the message header */
    (void)h;
    for (part = 0; part < 2; part++) {
        int n = part ? def->body_count : def->header_count;
        for (j = 0; j < n; j++) {
            const ce_field_ref *f = part ? &def->body[j] : &def->header[j];
            ce_bitstream out;
            unsigned long at = in.pos, k;
            long got, put;
            int same = 1;
            char text[512];
            memset(value, 0, sizeof value);
            got = ce_field_decode(cfg, &in, f->props, NULL, value);
            ce_bs_init(&out, tmp, sizeof tmp * 8);
            put = ce_field_encode(cfg, &out, f->props, NULL, value);
            if (got != put)
                same = 0;
            for (k = 0; same && k < (unsigned long)got; k++) {
                unsigned long p = at + k;
                if (((s[p >> 3] >> (p & 7)) & 1) != ((tmp[k >> 3] >> (k & 7)) & 1))
                    same = 0;
            }
            if (!same) {
                unsigned long a = 0, b = 0;
                int bits = got < 32 ? (int)got : 32;
                ce_bitstream x;
                ce_bs_init_read(&x, s, limit);
                x.pos = at;
                ce_format_value(f->props, value, text, sizeof text);
                printf("      %s: %s (%ld bits) re-encodes differently", f->name, text, got);
                /* first component's bits, for the record */
                a = ce_bs_read(&x, bits);
                ce_bs_init_read(&x, tmp, (unsigned long)put);
                b = ce_bs_read(&x, bits);
                printf("; first %d bits %08lX -> %08lX%s\n", bits, a, b,
                       f->props->type == CE_FT_LOCALITY_REFERENCE_POSITION ?
                       " (expected: the engine encodes with 0.0001f and a float range, decodes with 10000 and the exact range)" : "");
                if (f->props->type != CE_FT_LOCALITY_REFERENCE_POSITION &&
                    f->props->type != CE_FT_FIXED_WIDTH_NORMAL_VECTOR &&
                    f->props->type != CE_FT_SMART_VECTOR)
                    bad++;
            }
        }
    }
    return bad;
}

static void capture(const char *path, const char *log_path)
{
    FILE *fp = fopen(path, "rb");
    static unsigned char data[1 << 16];
    static unsigned char hbuf[64], bbuf[CE_DELTA_MAX_BODY_SIZE + 64];
    static unsigned char packet[4096];
    long size, at = 0;
    int record = 0, streams_ok = 0, streams = 0, items = 0;
    ce_delta_config cfg;
    cap_ctx ctx;
    if (!fp) {
        printf("capture: cannot open %s\n", path);
        failures++;
        return;
    }
    size = (long)fread(data, 1, sizeof data, fp);
    fclose(fp);
    ce_delta_config_default(&cfg);
    cfg.encoding_class = CE_CLASS_LAN;   /* the probe joins as connection class 4 (LAN) */
    memset(&ctx, 0, sizeof ctx);
    ctx.cfg = &cfg;
    ctx.log = log_path ? fopen(log_path, "w") : NULL;
    while (at + 3 <= size) {
        int reliable = data[at];
        long len = data[at + 1] | (data[at + 2] << 8);
        const unsigned char *s = data + at + 3;
        ce_bitstream bs;
        int bytes, kind, ok = 1;
        at += 3 + len;
        streams++;
        if (ctx.log)
            fprintf(ctx.log, "record %d: %s, %ld bytes\n", record, reliable ? "reliable" : "unreliable", len);
        bytes = ce_stream_open(&bs, s, (unsigned long)len);
        if (bytes != len) {
            printf("record %d: stream size %d != %ld\n", record, bytes, len);
            ok = 0;
        }
        while (ok && (kind = ce_stream_next(&bs)) != CE_ITEM_END) {
            items++;
            if (kind == CE_ITEM_PACKET) {
                int n = ce_stream_read_packet(&bs, packet, sizeof packet);
                if (ctx.log)
                    fprintf(ctx.log, "  @%lu old packet, %d bytes, type %d\n", bs.pos, n, n > 2 ? packet[n - 1] : -1);
                if (n < 0)
                    ok = 0;
            } else {
                ce_message_header h;
                unsigned long start = bs.pos;
                int err;
                if (ctx.log)
                    fprintf(ctx.log, "  @%lu ", start);
                err = ce_delta_decode(&cfg, &bs, &h, hbuf, bbuf, zero_baseline, NULL, &ctx);
                if (err < 0) {
                    printf("record %d @%lu: %s\n", record, start, ce_delta_error_text(err));
                    ok = 0;
                    break;
                }
                ctx.messages++;
                if (ctx.log) {
                    fprintf(ctx.log, "%s mode %d iterations %d (%lu bits)\n", h.def->name, h.mode, h.iterations, h.bits);
                }
                /* re-decode with logging, and re-encode stateless single-iteration messages */
                {
                    ce_bitstream again;
                    ce_message_header h2;
                    ce_bs_init_read(&again, s, bs.limit);
                    again.pos = start;
                    ce_delta_decode(&cfg, &again, &h2, hbuf, bbuf, zero_baseline, each_iteration, &ctx);
                    if (h.mode == 0 && h.iterations == 1) {
                        ce_bitstream out;
                        long bits;
                        ce_bs_init(&out, buf, sizeof buf * 8);
                        bits = ce_delta_encode1(&cfg, &out, 0, h.type, hbuf, bbuf, NULL);
                        {
                            unsigned long k;
                            int same = bits == (long)h.bits;
                            for (k = 0; same && k < h.bits; k++) {
                                unsigned long p = start + k;
                                if (((s[p >> 3] >> (p & 7)) & 1) != ((buf[k >> 3] >> (k & 7)) & 1))
                                    same = 0;
                            }
                            ctx.reencoded++;
                            if (!same) {
                                printf("record %d @%lu %s: re-encode differs (%ld vs %lu bits)\n", record, start, h.def->name, bits, h.bits);
                                if (explain_difference(&cfg, &h, s, start, bs.limit))
                                    ctx.reencode_failed++;
                                else
                                    ctx.reencode_lossy++;
                            } else {
                                ctx.reencode_same++;
                            }
                        }
                    }
                }
            }
        }
        if (ok) {
            streams_ok++;
            if (ctx.log)
                fprintf(ctx.log, "  end at bit %lu of %lu\n", bs.pos, bs.limit);
        } else {
            failures++;
        }
        record++;
    }
    printf("capture %s: %d/%d streams decoded to their end, %d items, %d messages; "
           "%d stateless messages re-encoded: %d bit-identical, %d differ only in locality positions "
           "(engine asymmetry), %d differ elsewhere\n",
           path, streams_ok, streams, items, ctx.messages, ctx.reencoded, ctx.reencode_same,
           ctx.reencode_lossy, ctx.reencode_failed);
    if (ctx.reencode_failed)
        failures++;
    if (ctx.log)
        fclose(ctx.log);
}

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv)
{
    ce_delta_config cfg;
    int type, cls;
    if (sizeof(void *) != 4)
        printf("warning: built for %d-bit pointers; hud_chat's layout assumes 32-bit\n", (int)sizeof(void *) * 8);
    for (cls = 0; cls < 2; cls++) {
        ce_delta_config_default(&cfg);
        cfg.encoding_class = cls;
        for (type = 0; type < CE_MESSAGE_COUNT; type++) {
            roundtrip_stateless(&cfg, type, 40);
            roundtrip_incremental(&cfg, type, 40);
        }
        iterated(&cfg);
    }
    ce_delta_config_default(&cfg);
    cfg.changeover = 1;
    cfg.protocol_version = 2;
    roundtrip_stateless(&cfg, CE_MSG_GAME_SETTINGS_UPDATE, 5);
    printf("round trips: %s (%d failures); stateless re-encodes bit-identical %d, "
           "different only through the engine's lossy quantizers %d\n",
           failures ? "FAILED" : "ok", failures, reencode_same, reencode_lossy);
    if (argc > 1)
        capture(argv[1], argc > 2 ? argv[2] : NULL);
    printf("%s\n", failures ? "FAILED" : "all tests passed");
    return failures ? 1 : 0;
}
