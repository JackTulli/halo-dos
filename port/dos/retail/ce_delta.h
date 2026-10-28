/* Halo Custom Edition 1.10 "message delta" codec (Bungie/Gearbox bit-packed
   messages), worked out statically from haloceded.exe 01.00.10.0621 and checked
   against a real capture of a CE 1.10 server's streams. See ce_delta_notes.md.

   Data model: messages are encoded from / decoded into plain byte buffers laid out
   exactly as the engine's network structures (field offsets from the engine's own
   tables, ce_delta_tables.c). Field values keep the engine's raw representation:
   integers are unsigned bit patterns, translated indexes are network indexes,
   booleans 0/1, reals IEEE floats, vectors 3 floats, strings NUL-terminated (wide
   strings 16-bit units). Use the accessors below or your own structs.

   Plain C99; no allocation; libm (floor, sqrt, atan, acos, sin, cos). Pointer
   fields (hud_chat's message) hold native pointers: the engine layout assumes a
   32-bit target (DOS/Win95 builds and the 32-bit host tests). */
#ifndef CE_DELTA_H
#define CE_DELTA_H

#include "ce_bitstream.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ tables */

typedef enum ce_field_type {
    CE_FT_INTEGER = 0,
    CE_FT_REAL,
    CE_FT_BOOLEAN,
    CE_FT_ASCII_CHARACTER,
    CE_FT_WIDE_CHARACTER,
    CE_FT_ASCII_STRING,
    CE_FT_WIDE_STRING,
    CE_FT_ARBITRARY_DATA,
    CE_FT_ARRAY,
    CE_FT_STRUCTURE,
    CE_FT_POINTER,
    CE_FT_ENUMERATION,
    CE_FT_BOUNDED_INDEX,
    CE_FT_TRANSLATED_INDEX,
    CE_FT_POINT,
    CE_FT_VECTOR,
    CE_FT_ANGLE,
    CE_FT_FLAGS,
    CE_FT_TIME,
    CE_FT_GRENADE_COUNTS,
    CE_FT_FIXED_WIDTH,
    CE_FT_FIXED_WIDTH_NORMAL_VECTOR,
    CE_FT_LOCALITY_REFERENCE_POSITION,
    CE_FT_DIGITAL_THROTTLE,
    CE_FT_FIXED_WIDTH_WEAPON_INDEX,
    CE_FT_FIXED_WIDTH_GRENADE_INDEX,
    CE_FT_SMART_VECTOR,
    CE_FT_ITEM_PLACEMENT_POSITION,
    CE_FT_COUNT
} ce_field_type;

struct ce_field_ref;

/* One field_properties_definition (a "field type" in OpenSauce's XML, e.g.
   object_index, integer_small, translational_velocity). Which members mean what
   depends on type:
     INTEGER            bits (1,3,5,6,8,16,32); size = storage bytes (1, 2 or 4)
     ASCII/WIDE_STRING  count = max length (characters, no NUL), bits = length bits
     ARRAY              count, stride (bytes), element
     STRUCTURE          count = member count, members
     POINTER            element = referent
     TRANSLATED_INDEX   bits, count = maximum active at once
     POINT, VECTOR      count = components (2 or 3)
     FLAGS              count, valid_mask (bits actually sent)
     FIXED_WIDTH        bits, range (value * range, rounded)
     FIXED_WIDTH_NORMAL bits_internet / bits_lan = { theta bits, phi bits }
     SMART_VECTOR       minimum, maximum, bits_internet[0], bits_lan[0], range
                        (internet), range_lan, count = special vectors, special */
typedef struct ce_field_props {
    const char *name;
    int type;                  /* ce_field_type */
    unsigned size;             /* bytes the value occupies in the engine structure */
    unsigned max_bits;         /* engine maximum_size (bits) */
    unsigned header_bits;      /* engine header_bit_size */
    int bits;
    int count;
    unsigned stride;
    unsigned long range;
    unsigned long range_lan;
    unsigned long valid_mask;
    int bits_internet[2];
    int bits_lan[2];
    float minimum, maximum;
    const struct ce_field_props *element;
    const struct ce_field_ref *members;
    const float (*special)[3];
} ce_field_props;

typedef struct ce_field_ref {
    const ce_field_props *props;
    unsigned short offset;           /* in the source/destination structure */
    unsigned short baseline_offset;  /* in the baseline structure (always == offset in 1.10) */
    const char *name;                /* member name (OpenSauce's names; see notes) */
} ce_field_ref;

typedef struct ce_message_def {
    int type;                  /* definition index = message type id (0..56) */
    const char *name;
    int max_iterations;
    int header_count;
    const ce_field_ref *header;
    int body_count;
    const ce_field_ref *body;
    unsigned header_size;      /* bytes spanned by the header fields (lower bound of the engine struct) */
    unsigned body_size;        /* bytes spanned by the body fields */
    unsigned iteration_max_bits;
    unsigned total_max_bits;
} ce_message_def;

#include "ce_delta_tables.h"   /* ce_message_defs[], CE_MSG_* ids, ce_fp_* field types */

const ce_message_def *ce_delta_message(int type);   /* NULL if out of range */

/* ------------------------------------------------------------------ config */

#define CE_CLASS_LAN      0
#define CE_CLASS_INTERNET 1

typedef struct ce_delta_config {
    int encoding_class;          /* g_message_delta_encoding_class: CE_CLASS_LAN or CE_CLASS_INTERNET */
    int changeover;              /* parameters-protocol changeover on: 3 more message-header bits */
    int protocol_version;        /* 2 bits, sent/checked only with changeover */
    int parameters_flag;         /* 1 bit, sent only with changeover */
    int locality_bits_full;      /* 25 */
    int locality_bits_delta;     /* 9 */
    int locality_bits_lan;       /* 31 */
    float locality_delta_cutoff; /* 8.0 */
    float locality_min_move;     /* 0.01 */
} ce_delta_config;

/* Engine defaults (haloceded 0x5AB35C), changeover off, class = internet (the
   file's default; the class actually used is negotiated per connection, see notes:
   a client announcing connection class 4 (LAN) gets the LAN class). */
void ce_delta_config_default(ce_delta_config *cfg);

/* ------------------------------------------------------------------ errors */

#define CE_OK               0
#define CE_ERR_OVERFLOW    -1   /* buffer too small / read past the end */
#define CE_ERR_TYPE        -2   /* message type out of range */
#define CE_ERR_MODE        -3   /* mode not 0/1 */
#define CE_ERR_ITERATIONS  -4
#define CE_ERR_STRING      -5   /* string longer than the field's maximum */
#define CE_ERR_POINTER     -6   /* NULL pointer field */
#define CE_ERR_VERSION     -7   /* changeover version mismatch (engine drops the message) */
#define CE_ERR_UNSUPPORTED -8   /* field type no message uses */
#define CE_ERR_EMPTY       -9   /* a present field decoded to 0 bits (engine: discard) */
#define CE_ERR_STREAM      -10  /* stream framing */

const char *ce_delta_error_text(int err);

/* ------------------------------------------------------------------ stream framing
   One decrypted GT2 application message is a bit stream (LSB first):
     11 bits  total size in bytes, including these 11 bits (max 2047)
     items while at least 8 bits are left:
       1 bit   kind: 1 = message delta, 0 = old network-game packet
       item    a message delta (below), or an old packet (16-bit header with its
               byte size in the top 12 bits, then size-2 bytes, read 8 bits at a time)
     the last (< 8) bits of the last byte are left-overs, not necessarily zero. */

#define CE_STREAM_MAX_BYTES 2047
#define CE_ITEM_PACKET 0
#define CE_ITEM_DELTA  1
#define CE_ITEM_END   -1

/* Reads the 11-bit size and limits bs to it. Returns the size in bytes or CE_ERR_STREAM. */
int ce_stream_open(ce_bitstream *bs, const void *data, unsigned long bytes);
/* Returns CE_ITEM_DELTA, CE_ITEM_PACKET or CE_ITEM_END (fewer than 8 bits left). */
int ce_stream_next(ce_bitstream *bs);
/* Skips an old network-game packet (after its kind bit); copies its bytes (header
   included, as 2 little-endian bytes) to out when out is not NULL. Returns its
   size in bytes or an error. */
int ce_stream_read_packet(ce_bitstream *bs, unsigned char *out, int out_size);

/* Writing: begin reserves the 11 size bits; put_kind writes a kind bit before each
   item; finish pads to a byte (zero bits) and writes the size. Returns bytes. */
int ce_stream_begin(ce_bitstream *bs, void *buffer, unsigned long bytes);
int ce_stream_put_kind(ce_bitstream *bs, int kind);
long ce_stream_finish(ce_bitstream *bs);

/* ------------------------------------------------------------------ messages
   A message delta (after its kind bit):
     1 bit   mode (0 stateless, 1 incremental)
     6 bits  type (definition index)
     [changeover only: 1 bit parameters flag, 2 bits protocol version]
     [max_iterations > 1: iterations-1 in bits(max_iterations-1) bits]
     per iteration:
       [incremental: one presence bit per body field]
       header fields (always encoded without baseline)
       body fields (all, or the present ones), encoded against the baseline when
       the message is incremental and a baseline is used. */

typedef struct ce_message_header {
    int mode;                /* 0 stateless, 1 incremental */
    int type;
    int iterations;
    int parameters_flag;     /* changeover only */
    int protocol_version;    /* changeover only */
    unsigned long bits;      /* size of this part */
    const ce_message_def *def;
} ce_message_header;

/* Encodes one message (mdpi_encode, haloceded 0x4D93E0). headers/bodies/baselines
   are arrays of `iterations` pointers (headers may be NULL when the message has no
   header fields; baselines NULL or entries NULL = no baseline). Stateless messages
   never use baselines. In incremental mode an iteration whose body encodes to 0 bits
   (nothing changed) is dropped unless keep_empty. Returns the bits written (the
   message, without the stream's kind bit), 0 when every iteration was dropped
   (nothing written, bs restored), or a negative error. */
long ce_delta_encode(const ce_delta_config *cfg, ce_bitstream *bs, int mode, int type,
                     const void *const *headers, const void *const *bodies,
                     const void *const *baselines, int iterations, int keep_empty);

/* Single-iteration shorthand. */
long ce_delta_encode1(const ce_delta_config *cfg, ce_bitstream *bs, int mode, int type,
                      const void *header, const void *body, const void *baseline);

/* Decoding, step by step (mirrors 0x4D9910 / 0x4D9B10 / 0x4D9C70):
   read_header, then per iteration read_iteration_header (presence bits + header
   fields into `header`, which it zeroes first), then pick the baseline (e.g. from
   the header's baseline_index) and read_iteration_body. */
int ce_delta_read_header(const ce_delta_config *cfg, ce_bitstream *bs, ce_message_header *h);
int ce_delta_read_iteration_header(const ce_delta_config *cfg, ce_bitstream *bs,
                                   const ce_message_header *h, unsigned char present[64],
                                   void *header);
/* body: destination; prepared first (copy of baseline, else zeroed; top-level pointer
   fields keep the caller's pointer). baseline: NULL for none (stateless messages
   always decode without). Returns bits read or an error. */
int ce_delta_read_iteration_body(const ce_delta_config *cfg, ce_bitstream *bs,
                                 const ce_message_header *h, const unsigned char present[64],
                                 const void *baseline, void *body);

/* Whole message with callbacks. baseline_fn may be NULL (incremental bodies then
   decode without a baseline, which is NOT what the server does for object updates:
   pass a baseline (even zeros) to get the layout right). header_buf/body_buf must
   hold CE_DELTA_MAX_HEADER_SIZE / CE_DELTA_MAX_BODY_SIZE bytes. */
typedef const void *(*ce_baseline_fn)(void *user, const ce_message_header *h, int iteration,
                                      const void *header);
typedef int (*ce_iteration_fn)(void *user, const ce_message_header *h, int iteration,
                               const unsigned char *present, const void *header, const void *body);
int ce_delta_decode(const ce_delta_config *cfg, ce_bitstream *bs, ce_message_header *h,
                    void *header_buf, void *body_buf,
                    ce_baseline_fn baseline_fn, ce_iteration_fn each, void *user);

/* One field on its own (what the message functions do per field): encode against
   base (NULL = none) / decode with base. Return bits or a negative error. */
long ce_field_encode(const ce_delta_config *cfg, ce_bitstream *bs, const ce_field_props *p,
                     const void *base, const void *src);
long ce_field_decode(const ce_delta_config *cfg, ce_bitstream *bs, const ce_field_props *p,
                     const void *base, void *dst);

/* ------------------------------------------------------------------ field access */

/* Finds a field by member name ("position", "players", ...); part 0 = header, 1 = body. */
const ce_field_ref *ce_delta_field(const ce_message_def *def, int part, const char *name);
#define ce_field_ptr(base, ref) ((void *)((char *)(base) + (ref)->offset))

unsigned long ce_get_uint(const ce_field_props *p, const void *at);  /* integer-like fields, raw */
long ce_get_int(const ce_field_props *p, const void *at);            /* sign-extended from the storage size */
float ce_get_real(const ce_field_props *p, const void *at);          /* real/fixed width */
void ce_get_vector(const ce_field_props *p, const void *at, float out[3]); /* 2 or 3 components */
void ce_set_uint(const ce_field_props *p, void *at, unsigned long v);
void ce_set_real(const ce_field_props *p, void *at, float v);
void ce_set_vector(const ce_field_props *p, void *at, const float v[3]);

/* Formats one value (recursively for arrays/structures) as text; returns length. */
int ce_format_value(const ce_field_props *p, const void *at, char *out, int size);

/* Visits every field (depth first). path is "name", "name[3]" or "name.member". */
typedef void (*ce_visit_fn)(void *user, const char *path, const ce_field_props *p, const void *at);
void ce_delta_visit(const ce_message_def *def, int part, const void *data, ce_visit_fn fn, void *user);

#ifdef __cplusplus
}
#endif

#endif
