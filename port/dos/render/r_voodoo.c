/*
R_VOODOO.C

The 3dfx Voodoo Graphics rasterizer (r_voodoo.h): what the software
rasterizer draws, drawn by the card, its registers written directly (the
DOS build has no Glide library to call). The front end (r_main.c) culls,
transforms, lights, fogs and clips as ever; its screen-space triangles come
here (r_internal.h's backend) and go to the card with their gradients, as
Glide's grDrawTriangle sends them: the vertices in the card's pixels (the
picture's scaled to its 640x480), each parameter's value at the first
vertex and its change a pixel across and down, through the float
registers, written fresh for every triangle (the sub-pixel correction adds
into them) but where they would not change: a parameter the triangle's
state does not read, or one the same at every corner as the card already
has it. On a Pentium the CPU's work for each triangle is most of the cost
(and each register a write across the PCI bus): a vertex the front end
projected once is made a corner once (the corners' cache), a draw's state
set once, the status register read once for a triangle's writes.

	- light: the texel times the iterated colour; 0x80 is the texel as it
	  is, so the colour is doubled (a Voodoo Graphics has no 2x modulate:
	  nothing brighter than the texel);
	- depth: the W buffer, W = 1/z scaled into (2^-16, 0.5] for the world;
	  the first-person weapon, drawn first and in front of it all
	  (r_set_in_front), takes (0.5, 1] with its texture affine (its W no
	  longer 1/z), so the world never covers it;
	- fog: the front end's per vertex, in the Z iterator (the W buffer
	  leaves it free), the fog colour the view's;
	- blends: added (ONE, ONE), half and by alpha (SRC_ALPHA, ONE MINUS
	  SRC_ALPHA, the vertex alpha the half or the alpha scale), the screen's
	  four, alpha-tested textures in ARGB1555 (A >= 0x80 drawn);
	- textures: RGB565, ARGB1555 or ARGB4444 (each channel rounded) in the
	  first texture unit's memory (2 MB, or 4 where r_voodoo_use finds
	  them), every level, put there the first time they are drawn and
	  evicted least recently used first, those of the same frame too; an
	  aspect past 8:1 is stretched to it (the coordinates are the texture's
	  0-1 all the same);
	- the 2D helpers: rectangles and rows as untextured triangles, text's
	  glyphs as small alpha textures (A8), pictures as 256x256 tiles.
The card is kept to a small set of states: DOSBox-X's emulation keeps every
combination it has seen, a thousand at most.
*/

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "r_internal.h"
#include "r_voodoo.h"

#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

/* ---------- the card's registers (byte offsets in its space) */

#define STATUS 0x000
#define FVERTEX_AX 0x088
#define FSTART 0x0A0 /* R, G, B, Z, A, S, T, W: 4 bytes apart */
#define FD_DX 0x0C0
#define FD_DY 0x0E0
#define FTRIANGLE_CMD 0x100
#define FBZ_COLOR_PATH 0x104
#define FOG_MODE 0x108
#define ALPHA_MODE 0x10C
#define FBZ_MODE 0x110
#define LFB_MODE 0x114
#define CLIP_LEFT_RIGHT 0x118
#define CLIP_LOW_Y_HIGH_Y 0x11C
#define FASTFILL_CMD 0x124
#define SWAPBUFFER_CMD 0x128
#define FOG_COLOR 0x12C
#define ZA_COLOR 0x130
#define COLOR1 0x148
/* the first texture unit's (its chip select) */
#define TMU0 0x800
#define TEXTURE_MODE 0x300
#define T_LOD 0x304
#define T_DETAIL 0x308
#define TEX_BASE_ADDR 0x30C
#define LFB 0x400000
#define TEXTURE_SPACE 0x800000

enum { P_R, P_G, P_B, P_Z, P_A, P_S, P_T, P_W, PARAMETERS };

/* fbzColorPath: the texel times the iterated colour and alpha / the
iterated alone (both with the sub-pixel correction) */
#define PATH_TEXTURED 0x0C482405UL
#define PATH_COLOR 0x04000000UL
/* fbzMode: clip, W buffer, depth test less, dither, colour and depth
writes, the back buffer; without depth writes; 2D; fast fills. Blends take
the dither back out of what is under before blending (the alpha-blend
dither subtraction): the card reads its 16-bit pixels without it, and a
blend that adds nothing (a texel of no alpha) wrote back a pixel a step
darker nearly half the time, so the HUD's large quads darkened the
rectangles they cover, more with every layer */
#define MODE_SOLID 0x84739UL
#define MODE_TRANSPARENT 0x84339UL
#define MODE_DEPTH_TEST 0x0010UL
/* (the depth test's function, and less or equal: see voodoo_triangle) */
#define MODE_DEPTH_FUNCTION 0x00E0UL
#define MODE_DEPTH_LESS_EQUAL 0x0060UL
#define MODE_2D 0x84301UL
#define MODE_FILL 0x4600UL
#define MODE_FILL_DEPTH 0x4400UL
/* alphaMode */
#define BLEND_NONE 0UL
#define BLEND_ADD 0x4410UL /* ONE, ONE */
#define BLEND_ALPHA 0x5110UL /* SRC_ALPHA, ONE MINUS SRC_ALPHA */
#define BLEND_ADD_ALPHA 0x4110UL /* SRC_ALPHA, ONE */
#define BLEND_PREMULTIPLIED 0x5410UL /* ONE, ONE MINUS SRC_ALPHA */
#define ALPHA_TEST 0x8000000DUL /* A >= 0x80 */
/* textureMode: the unit's own texel ("local"), and its formats */
#define TEXTURE_LOCAL 0x08241000UL
#define TEXTURE_PERSPECTIVE 0x01UL
#define TEXTURE_BILINEAR 0x06UL
#define TEXTURE_CLAMP 0xC0UL
enum
{
	FORMAT_A8 = 2,
	FORMAT_RGB565 = 10,
	FORMAT_ARGB1555 = 11,
	FORMAT_ARGB4444 = 12,
};

/* the world's W: 1/z times this, 0.5 at the nearest the view draws */
#define WORLD_W 0.03125f
/* the W buffer's farthest: a W below it is the depth 0xFFFF */
#define FARTHEST_W (1.0f / 65536.0f)
/* the first-person weapon's: 0.5 to 1 as 1/z goes to this */
#define IN_FRONT_W_RANGE 0.01171875f
/* texture memory below this is not handed out: a texture's levels count
from LOD 0 (256 texels across), whose start may be this far before its
own */
#define TEXTURE_MEMORY_START 0x30000UL

/* ---------- state */

#define SLOTS 2048
#define SLOT_BITS 12

struct slot
{
	int used;
	unsigned long address, size;
	unsigned long texture_mode, t_lod, tex_base;
	float s_scale, t_scale;
	/* when it was last drawn with (v.uses) */
	unsigned long last_used;
	unsigned long generation;
};

static struct
{
	volatile unsigned long *base;
	/* (the same, its float registers written straight from the FPU) */
	volatile float *floats;
	int width, height;
	/* the screen's far edges as a float's bits (a corner is on the screen
	when its coordinates' bits are at most these: no negative float is) */
	unsigned long width_bits, height_bits;
	/* the parameters (bit P_x) whose gradients on the card are 0, and of
	those the ones whose start is known (starts[]): a triangle whose
	parameter is the same at all three corners writes it only if it differs
	(the sub-pixel correction adds nothing to a start with no gradients) */
	unsigned zero_gradients, known_starts;
	unsigned long starts[PARAMETERS];
	/* the texture drawn last and its slot (until the slot is freed) */
	const struct r_texture *last_texture;
	struct slot *last_slot;
	/* the last 3D triangle's state (state_valid: the registers as it left
	them), what of its flags it read, its slot, the parameters it needed,
	its alpha, the way its corners were made */
	int state_valid;
	unsigned state_flags;
	const struct slot *state_slot;
	unsigned state_needed;
	unsigned long state_alpha, state_mode;
	/* (the frame's number in the corners' modes) */
	unsigned long corner_generation;
	/* the last unfogged corner's light (its vertex's colour's bits) */
	unsigned long last_light[3];
	float last_light_color[3];
	float scale_x, scale_y;
	unsigned long memory;
	/* the registers as last written (0xFFFFFFFF: not known) */
	unsigned long color_path, fog_mode, alpha_mode, fbz_mode, texture_mode, t_lod, tex_base, fog_color;
	unsigned long clip_x, clip_y;
	int in_front;
	unsigned long alpha_scale;
	int screen_blend;
	int fogged;
	unsigned long view_fog_color;
	int smooth;
	/* textures drawn with so far, counted: the least recently used are
	evicted first */
	unsigned long uses;
	/* the largest side a texture keeps on the card (log2): 64 texels. A
	frame of Blood Gulch at 128 needs more than even 4 MB holds (every frame
	evicted and downloaded again): 4 MB keeps more textures, not larger */
	int largest_shift;
	/* added to a clip rectangle's right edge for triangles: 1 where the
	card leaves out the last column a span reaches (DOSBox-X's emulation
	does: the screen's right column and a split view's were never drawn),
	0 where it draws up to the edge (r_voodoo_use tries it) */
	int clip_extra;
	/* the writes the card's FIFO has room for, as it last said */
	int room;
	struct slot slots[SLOTS];
	struct
	{
		const unsigned long *argb;
		int width, height;
		unsigned long check;
		int columns, rows;
		unsigned long tiles[12];
		unsigned long used;
	} pictures[4];
	unsigned long clock;
} v;

static const struct r_backend voodoo_backend;

/* every write goes through the card's PCI FIFO (64 entries): a write to a
full one is not taken on every chipset, so each waits for room, the room
counted down from what the status register last said (a read for every
63 writes; DOSBox-X's always says the FIFO is empty) */
static void fifo_reserve(void)
{
	if (v.room <= 0)
	{
		for (long i = 0; i < 1000000 && (v.room = (int)(v.base[STATUS >> 2] & 0x3F)) == 0; i++)
			;
		if (v.room <= 0)
			v.room = 1;
	}
	v.room--;
}

static void write_register(unsigned offset, unsigned long value)
{
	fifo_reserve();
	v.base[offset >> 2] = value;
}

static void write_float(unsigned offset, float value)
{
	union { float f; unsigned long u; } bits;

	bits.f = value;
	fifo_reserve();
	v.base[offset >> 2] = bits.u;
}

/* the card done with every write (its FIFO empty and the chip idle: the
status register's busy bit clear three reads running, as Glide waits),
before the linear frame buffer is read */
static void wait_for_idle(void)
{
	int quiet = 0;

	for (long i = 0; i < 1000000 && quiet < 3; i++)
		quiet = (v.base[STATUS >> 2] & 0x200) ? 0 : quiet + 1;
	v.room = 0;
}

/* room in the FIFO for count writes at once (a triangle's): the status
register read until it says so, the writes then counted off v.room as
fifo_reserve's are */
static void fifo_room(int count)
{
	if (v.room < count)
	{
		for (long i = 0; i < 1000000 && (v.room = (int)(v.base[STATUS >> 2] & 0x3F)) < count; i++)
			;
		if (v.room < count)
			v.room = count;
	}
}

/* the registers of a 3D triangle's state are as the last one's left them
no longer (anything else that writes them, or changes what they would be,
says so) */
static void state_changed(void)
{
	v.state_valid = FALSE;
}

static void set_register(unsigned offset, unsigned long *cached, unsigned long value)
{
	if (*cached != value)
	{
		*cached = value;
		write_register(offset, value);
	}
}

/* the card's clip rectangle [x0, x1) x [y0, y1): for a fast fill as it is,
for triangles with the right edge the card needs to draw up to it */
static void clip_to(int x0, int y0, int x1, int y1, int fill)
{
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > v.width) x1 = v.width;
	if (y1 > v.height) y1 = v.height;
	if (x1 < x0) x1 = x0;
	if (y1 < y0) y1 = y0;
	if (!fill && x1 > x0)
		x1 += v.clip_extra;
	set_register(CLIP_LEFT_RIGHT, &v.clip_x, ((unsigned long)x0 << 16) | (unsigned long)x1);
	set_register(CLIP_LOW_Y_HIGH_Y, &v.clip_y, ((unsigned long)y0 << 16) | (unsigned long)y1);
}

/* the view's rectangle (r_view_begin to r_view_end), or the whole */
static int view_active, view_clip[4];

/* the clip rectangle of the triangles drawn from now */
static void clip_view(void)
{
	if (view_active)
		clip_to(view_clip[0], view_clip[1], view_clip[2], view_clip[3], FALSE);
	else
		clip_to(0, 0, v.width, v.height, FALSE);
}

static void voodoo_view_rect(int x0, int y0, int x1, int y1, int active)
{
	view_active = active;
	view_clip[0] = (int)((float)x0 * v.scale_x);
	view_clip[1] = (int)((float)y0 * v.scale_y);
	view_clip[2] = (int)((float)x1 * v.scale_x + 0.999f);
	view_clip[3] = (int)((float)y1 * v.scale_y + 0.999f);
	clip_view();
}

/* ---------- texture memory */

static unsigned long handle_of(int index)
{
	return (v.slots[index].generation << SLOT_BITS) | (unsigned long)(index + 1);
}

static struct slot *slot_of(unsigned long handle)
{
	int index = (int)(handle & ((1UL << SLOT_BITS) - 1)) - 1;

	if (index < 0 || index >= SLOTS || !v.slots[index].used || v.slots[index].generation != handle >> SLOT_BITS)
		return NULL;
	return &v.slots[index];
}

static void slot_free(struct slot *slot)
{
	if (slot == v.last_slot)
		v.last_texture = NULL;
	slot->used = FALSE;
	slot->generation = (slot->generation + 1) & 0xFFFFF;
}

static int compare_addresses(const void *left, const void *right)
{
	const struct slot *a = *(const struct slot *const *)left, *b = *(const struct slot *const *)right;

	return a->address < b->address ? -1 : a->address > b->address;
}

/* size bytes (a multiple of 8) of texture memory: the first gap that holds
it, the least recently used textures evicted until one does; the slot's
index, or -1 if it is larger than the memory. Textures drawn earlier in
the same frame go too if they must: the card takes its writes in order,
so their triangles are drawn before their texels are overwritten (when a
frame's textures were kept, the ones it drew last found no room once the
memory was full -- the HUD's drawn as solid rectangles of their colour,
text without letters, the world's last triangles untextured) */
static int slot_allocate(unsigned long size)
{
	static struct slot *sorted[SLOTS];
	int count = 0, free_index = -1;

	for (int i = 0; i < SLOTS; i++)
	{
		if (v.slots[i].used)
			sorted[count++] = &v.slots[i];
		else if (free_index < 0)
			free_index = i;
	}
	qsort(sorted, (size_t)count, sizeof(sorted[0]), compare_addresses);
	/* (every slot taken: the least recently used one's) */
	if (free_index < 0)
	{
		int oldest = 0;

		for (int i = 1; i < count; i++)
			if (sorted[i]->last_used < sorted[oldest]->last_used)
				oldest = i;
		slot_free(sorted[oldest]);
		free_index = (int)(sorted[oldest] - v.slots);
	}
	/* (the gaps between the textures still there, in order of address;
	evicted ones are passed over, the list sorted once) */
	for (;;)
	{
		unsigned long at = TEXTURE_MEMORY_START;
		int oldest = -1;

		for (int i = 0; i <= count; i++)
		{
			unsigned long end;

			if (i < count && !sorted[i]->used)
				continue;
			end = i < count ? sorted[i]->address : v.memory;
			if (end >= at && end - at >= size)
			{
				struct slot *slot = &v.slots[free_index];

				slot->used = TRUE;
				slot->address = at;
				slot->size = size;
				slot->last_used = ++v.uses;
				return free_index;
			}
			if (i < count)
			{
				at = sorted[i]->address + sorted[i]->size;
				if (oldest < 0 || sorted[i]->last_used < sorted[oldest]->last_used)
					oldest = i;
			}
		}
		if (oldest < 0)
			return -1;
		slot_free(sorted[oldest]);
	}
}

/* the bytes of a mip level (the card keeps at least 4 texels a level from
LOD 3 on) */
static unsigned long level_texels(int lod, unsigned long wide_mask, unsigned long narrow_mask)
{
	unsigned long count = ((wide_mask >> lod) + 1) * ((narrow_mask >> lod) + 1);

	return lod >= 3 && count < 4 ? 4 : count;
}

/* the TMU's texture registers for a slot (for drawing with it, and for
writing its texels: the download's addresses are decoded with them) */
static void slot_select(const struct slot *slot, int perspective)
{
	unsigned long mode = slot->texture_mode | (perspective ? TEXTURE_PERSPECTIVE : 0) |
		(v.smooth ? TEXTURE_BILINEAR : 0);

	set_register(TMU0 + TEXTURE_MODE, &v.texture_mode, mode);
	set_register(TMU0 + T_LOD, &v.t_lod, slot->t_lod);
	set_register(TMU0 + TEX_BASE_ADDR, &v.tex_base, slot->tex_base);
}

/* a slot for a texture of levels top_shift_w x top_shift_h (log2) down
levels, 16 bits a texel unless eight: its registers set, the TMU selected
for its download; -1 if there is no room */
static int slot_make(int width_shift, int height_shift, int levels, int format, int clamp, int eight)
{
	int wide = width_shift > height_shift ? width_shift : height_shift;
	int aspect = abs(width_shift - height_shift), lod_top = 8 - wide, index;
	unsigned long wide_mask = 0xFF, narrow_mask = 0xFFUL >> aspect, size = 0, before = 0;
	struct slot *slot;

	if (wide > 8 || aspect > 3 || levels < 1)
		return -1;
	if (lod_top + levels - 1 > 8)
		levels = 9 - lod_top;
	for (int lod = 0; lod < lod_top; lod++)
		before += level_texels(lod, wide_mask, narrow_mask) << (eight ? 0 : 1);
	for (int lod = lod_top; lod < lod_top + levels; lod++)
		size += level_texels(lod, wide_mask, narrow_mask) << (eight ? 0 : 1);
	size = (size + 7) & ~7UL;
	index = slot_allocate(size);
	if (index < 0)
		return -1;
	slot = &v.slots[index];
	slot->texture_mode = TEXTURE_LOCAL | ((unsigned long)format << 8) | (clamp ? TEXTURE_CLAMP : 0);
	slot->t_lod = (unsigned long)(lod_top * 4) | ((unsigned long)((lod_top + levels - 1) * 4) << 6) |
		(width_shift >= height_shift ? 0x100000UL : 0) | ((unsigned long)aspect << 21);
	slot->tex_base = ((slot->address - before) >> 3) & 0xFFFFF;
	slot->s_scale = (float)(width_shift >= height_shift ? 256 : 256 >> aspect);
	slot->t_scale = (float)(width_shift >= height_shift ? 256 >> aspect : 256);
	state_changed();
	slot_select(slot, TRUE);
	return index;
}

/* a texel pair (or four 8-bit texels) at level lod, row t, column s */
static void texel_write(int lod, int t, int s, unsigned long value)
{
	fifo_reserve();
	v.base[(TEXTURE_SPACE + ((unsigned long)lod << 17) + ((unsigned long)t << 9) + ((unsigned long)s << 1)) >> 2] =
		value;
}

/* an 8-bit channel in 4, 5 and 6 bits, to the nearest (the card widens
them again by repeating their top bits: cut off, the texels came out
darker, by about 1.5 of 255 in red and blue on average) */
static unsigned char narrow4[256], narrow5[256], narrow6[256];

static void narrow_tables(void)
{
	for (unsigned long c = 0; c < 256; c++)
	{
		narrow4[c] = (unsigned char)((c * 15 + 127) / 255);
		narrow5[c] = (unsigned char)((c * 31 + 127) / 255);
		narrow6[c] = (unsigned char)((c * 63 + 127) / 255);
	}
}

static unsigned long texel16(unsigned long color, int format)
{
	unsigned long a = color >> 24, r = (color >> 16) & 0xFF, g = (color >> 8) & 0xFF, b = color & 0xFF;

	switch (format)
	{
	case FORMAT_ARGB1555:
		return (a >= 0x80 ? 0x8000UL : 0) | ((unsigned long)narrow5[r] << 10) | ((unsigned long)narrow5[g] << 5) |
			narrow5[b];
	case FORMAT_ARGB4444:
		return ((unsigned long)narrow4[a] << 12) | ((unsigned long)narrow4[r] << 8) | ((unsigned long)narrow4[g] << 4) |
			narrow4[b];
	default:
		return ((unsigned long)narrow5[r] << 11) | ((unsigned long)narrow6[g] << 5) | narrow5[b];
	}
}

/* a texture's slot, made and its levels written the first time: NULL if
there is no room */
static struct slot *texture_slot(const struct r_texture *texture)
{
	struct r_texture *writable = (struct r_texture *)texture;
	struct slot *slot = writable->hardware ? slot_of(writable->hardware) : NULL;
	int alpha = (texture->flags & R_TEXTURE_ALPHA) != 0, alpha_test = (texture->flags & R_TEXTURE_ALPHA_TEST) != 0;
	int format = alpha ? FORMAT_ARGB4444 : alpha_test ? FORMAT_ARGB1555 : FORMAT_RGB565;
	int width_shift = texture->width_shift, height_shift = texture->height_shift;
	int index, lod_top, skip = 0;

	if (slot)
	{
		slot->last_used = ++v.uses;
		v.last_texture = texture;
		v.last_slot = slot;
		return slot;
	}
	/* (its largest levels left out past the card's budget) */
	while ((width_shift > v.largest_shift || height_shift > v.largest_shift) && skip < texture->levels - 1)
	{
		skip++;
		width_shift = width_shift > 0 ? width_shift - 1 : 0;
		height_shift = height_shift > 0 ? height_shift - 1 : 0;
	}
	/* (past 8:1, the narrow side stretched to an eighth of the wide one) */
	if (width_shift - height_shift > 3)
		height_shift = width_shift - 3;
	if (height_shift - width_shift > 3)
		width_shift = height_shift - 3;
	if (width_shift > 8 || height_shift > 8)
		return NULL;
	index = slot_make(width_shift, height_shift, texture->levels - skip, format,
		(texture->flags & R_TEXTURE_CLAMP) != 0, FALSE);
	if (index < 0)
		return NULL;
	slot = &v.slots[index];
	lod_top = 8 - (width_shift > height_shift ? width_shift : height_shift);
	for (int level = 0; level + skip < texture->levels && lod_top + level <= 8; level++)
	{
		int source = level + skip;
		int source_width = 1 << (texture->width_shift > source ? texture->width_shift - source : 0);
		int source_height = 1 << (texture->height_shift > source ? texture->height_shift - source : 0);
		int width = 1 << (width_shift > level ? width_shift - level : 0);
		int height = 1 << (height_shift > level ? height_shift - level : 0);
		const unsigned char *texels = texture->texels[source];

		for (int t = 0; t < height; t++)
		{
			const unsigned char *row = texels + (size_t)(t * source_height / height) * source_width;

			for (int s = 0; s < width; s += 2)
			{
				unsigned long pair[2];

				for (int k = 0; k < 2; k++)
				{
					int column = (s + k < width ? s + k : s) * source_width / width;
					unsigned char index8 = row[column];
					unsigned long color = texture->palette[index8];

					if (!alpha)
						color = (color & 0xFFFFFF) | (alpha_test && !index8 ? 0 : 0xFF000000UL);
					pair[k] = texel16(color, format);
				}
				texel_write(lod_top + level, t, s, pair[0] | (pair[1] << 16));
			}
		}
	}
	writable->hardware = handle_of(index);
	return slot;
}

static void voodoo_texture_delete(struct r_texture *texture)
{
	struct slot *slot = slot_of(texture->hardware);

	if (slot)
		slot_free(slot);
	texture->hardware = 0;
	if (v.last_texture == texture)
		v.last_texture = NULL;
}

/* a texture's slot as texture_slot finds it (counted as drawn with), the
same texture as the last triangle's (most of a draw's) found at once */
static __inline__ struct slot *texture_slot_drawn(const struct r_texture *texture)
{
	if (!texture)
		return NULL;
	if (texture == v.last_texture)
	{
		v.last_slot->last_used = ++v.uses;
		return v.last_slot;
	}
	return texture_slot(texture);
}

/* ---------- triangles */

struct corner
{
	/* the position snapped, the parameters, the position as it was (and
	its y as an integer in the same order, -0 as 0, for sorting) */
	float x, y;
	float p[PARAMETERS];
	float ux, uy;
	long order_y;
};

/* the corners' cache. A vertex the front end projected once is in most of
its draw's triangles: its corner is made once and found again by the
vertex's address and its contents (all a corner is made from, compared: a
vertex at the same address with other contents, a polygon clipped near the
eye, is made anew), made the same way (mode: the frame, fog, the weapon's
depth, the texture's shape) */
#define CORNER_CACHE 256

struct cached_corner
{
	struct corner corner;
	unsigned long mode;
	unsigned long key[9];
};

static struct cached_corner corner_cache[CORNER_CACHE];

/* the parameters a state reads (bits 1 << P_x): the iterated colour always,
its alpha only when blended or alpha tested, Z only when fogged (the depth
is W's), S and T when textured, W but in 2D (no depth there, its textures
not in perspective). The card keeps the others as they were, unread */
#define NEEDS(parameter) (1u << (parameter))
#define NEEDS_COLOR (NEEDS(P_R) | NEEDS(P_G) | NEEDS(P_B))
#define NEEDS_TEXTURE (NEEDS(P_S) | NEEDS(P_T))

/* the most writes a triangle takes: its corners, three a parameter, its
command */
#define TRIANGLE_WRITES (6 + 3 * PARAMETERS + 1)

/* the FPU's rounding (to the nearest: r_round's) of a double */
static __inline__ long round_double(double value)
{
	long result;

	__asm__("fistpl %0" : "=m"(result) : "t"(value) : "st");
	return result;
}

/* floor(value), exactly: rounded to the nearest, one less where that was
up (no library call, no control word changes) */
static __inline__ long floor_double(double value)
{
	long n = round_double(value);

	return (double)n > value ? n - 1 : n;
}

/* a corner at (x, y) on sixteenths of a pixel, as the card takes them
(the vertices are sorted by y as it was: order_y) */
static __inline__ void snap(struct corner *corner, float x, float y)
{
	long bits = *(const long *)&y;

	corner->x = (float)floor_double((double)x * 16.0 + 0.5) * 0.0625f;
	corner->y = (float)floor_double((double)y * 16.0 + 0.5) * 0.0625f;
	corner->order_y = bits < 0 ? -(bits & 0x7FFFFFFFL) : bits;
}

/* a colour channel for the card: 0 to 255, rounded half up (what is not a
number, 0) */
static __inline__ float color_byte(double value)
{
	long n = floor_double(value + 0.5);

	return (float)(n < 0 ? 0 : n > 255 ? 255 : n);
}

static __inline__ unsigned long bits_of(const float *value)
{
	return *(const unsigned long *)value;
}

/* a register written: a float straight from the FPU (rounded as a float's
store rounds), or a word. (The card's writes go in the order they are
written, and nothing a compiler could leave out) */
static __inline__ void put_float(volatile float *at, double value)
{
	__asm__ __volatile__("fstps %0" : "=m"(*(float *)at) : "t"(value) : "st");
}

static __inline__ void put_word(volatile float *at, unsigned long value)
{
	__asm__ __volatile__("movl %1, %0" : "=m"(*(float *)at) : "r"(value));
}

/* the plane through a triangle's corners: the second and third corners
from the first, and one over the determinant */
struct plane
{
	double dxb, dyb, dxc, dyc, inverse;
};

/* a parameter's start and gradients written (the writes it took); one
that may be the same at every corner (the light of an object lit evenly,
an alpha) has no gradients there: those are written only if the card's are
not already 0, its start only if it differs */
static __inline__ __attribute__((always_inline)) int parameter(volatile float *card, int i, const struct corner *a,
	const struct corner *b, const struct corner *c, const struct plane *plane, int may_be_constant)
{
	unsigned bit = 1u << i;
	unsigned long start = bits_of(&a->p[i]);

	if (may_be_constant && start == bits_of(&b->p[i]) && start == bits_of(&c->p[i]))
	{
		int written = 0;

		if (!(v.zero_gradients & bit))
		{
			put_word(&card[(FD_DX >> 2) + i], 0);
			put_word(&card[(FD_DY >> 2) + i], 0);
			written = 2;
			v.zero_gradients |= bit;
		}
		if (!(v.known_starts & bit) || v.starts[i] != start)
		{
			put_word(&card[(FSTART >> 2) + i], start);
			written++;
			v.starts[i] = start;
			v.known_starts |= bit;
		}
		return written;
	}
	{
		double ab = (double)b->p[i] - a->p[i], ac = (double)c->p[i] - a->p[i];

		put_word(&card[(FSTART >> 2) + i], start);
		put_float(&card[(FD_DX >> 2) + i], (ab * plane->dyc - ac * plane->dyb) * plane->inverse);
		put_float(&card[(FD_DY >> 2) + i], (ac * plane->dxb - ab * plane->dxc) * plane->inverse);
		v.zero_gradients &= ~bit;
		v.known_starts &= ~bit;
		return 3;
	}
}

/* a triangle of corners in the card's pixels, on its sixteenths, each
parameter needed at its corners: the vertices top to bottom, the start
values at the first, the gradients of the plane through the three */
static void emit(const struct corner *a, const struct corner *b, const struct corner *c, unsigned needed)
{
	volatile float *card = v.floats;
	const struct corner *swap;
	struct plane plane;
	double determinant;
	int written = 7;

	if (b->order_y < a->order_y) { swap = a; a = b; b = swap; }
	if (c->order_y < b->order_y) { swap = b; b = c; c = swap; }
	if (b->order_y < a->order_y) { swap = a; a = b; b = swap; }
	/* (sixteenths: the differences, their products exact) */
	plane.dxb = (double)b->x - a->x;
	plane.dyb = (double)b->y - a->y;
	plane.dxc = (double)c->x - a->x;
	plane.dyc = (double)c->y - a->y;
	determinant = plane.dxb * plane.dyc - plane.dxc * plane.dyb;
	if (!(determinant > 1.0 / 256.0 || determinant < -1.0 / 256.0))
		return;
	plane.inverse = 1.0 / determinant;
	fifo_room(TRIANGLE_WRITES);
	put_word(&card[(FVERTEX_AX >> 2) + 0], bits_of(&a->x));
	put_word(&card[(FVERTEX_AX >> 2) + 1], bits_of(&a->y));
	put_word(&card[(FVERTEX_AX >> 2) + 2], bits_of(&b->x));
	put_word(&card[(FVERTEX_AX >> 2) + 3], bits_of(&b->y));
	put_word(&card[(FVERTEX_AX >> 2) + 4], bits_of(&c->x));
	put_word(&card[(FVERTEX_AX >> 2) + 5], bits_of(&c->y));
	/* (the colour, alpha and fog may be even; the depth and the texture's
	coordinates hardly ever are) */
	written += parameter(card, P_R, a, b, c, &plane, TRUE);
	written += parameter(card, P_G, a, b, c, &plane, TRUE);
	written += parameter(card, P_B, a, b, c, &plane, TRUE);
	if (needed & NEEDS(P_Z))
		written += parameter(card, P_Z, a, b, c, &plane, TRUE);
	if (needed & NEEDS(P_A))
		written += parameter(card, P_A, a, b, c, &plane, TRUE);
	if (needed & NEEDS(P_S))
	{
		written += parameter(card, P_S, a, b, c, &plane, FALSE);
		written += parameter(card, P_T, a, b, c, &plane, FALSE);
	}
	if (needed & NEEDS(P_W))
		written += parameter(card, P_W, a, b, c, &plane, FALSE);
	/* (the area, Glide's (A - B) x (B - C), is the determinant) */
	put_float(&card[FTRIANGLE_CMD >> 2], determinant);
	v.room -= written;
}

/* a corner on the screen: its coordinates' bits at most the far edges'
(a negative float's, its sign set, and a NaN's are more) */
static __inline__ int on_screen(const struct corner *corner)
{
	return bits_of(&corner->ux) <= v.width_bits && bits_of(&corner->uy) <= v.height_bits;
}

/* how far a corner (its position not snapped) is inside one of the
screen's sides: x >= 0, x <= width, y >= 0, y <= height */
static __inline__ double inside(int side, const struct corner *corner)
{
	switch (side)
	{
	case 0: return corner->ux;
	case 1: return (double)v.width - corner->ux;
	case 2: return corner->uy;
	default: return (double)v.height - corner->uy;
	}
}

/* a triangle cut to the screen: what is left of it in polygon (count
corners, at most 7: the triangle's own, and those cut, in made, their
positions not snapped in ux and uy, snapped as they are made), 0 if
nothing is. A side no corner is beyond cuts nothing and is passed over
(what is cut from corners inside it stays inside it). The parameters are
the card's iterated ones, straight across the screen, so they are cut
where the edges are */
static int __attribute__((noinline)) clip_polygon(const struct corner *a, const struct corner *b,
	const struct corner *c, unsigned needed, struct corner made[8], const struct corner *polygon[9])
{
	const struct corner *lists[2][9], **in = lists[0], **out = lists[1], **swap;
	int count = 3, made_count = 0;
	unsigned beyond = 0;

	in[0] = a;
	in[1] = b;
	in[2] = c;
	for (int side = 0; side < 4; side++)
	{
		for (int i = 0; i < 3; i++)
		{
			if (!(inside(side, in[i]) >= 0.0))
				beyond |= 1u << side;
		}
	}
	for (int side = 0; side < 4 && count >= 3; side++)
	{
		int kept = 0;

		if (!(beyond & (1u << side)))
			continue;
		for (int i = 0; i < count; i++)
		{
			const struct corner *from = in[i], *to = in[i + 1 < count ? i + 1 : 0];
			double d0 = inside(side, from), d1 = inside(side, to);

			/* (more corners than a triangle cut by the four sides has, or
			cuts: corners not numbers; it is left out) */
			if (kept >= 8)
				return 0;
			if (d0 >= 0.0)
				out[kept++] = from;
			if ((d0 >= 0.0) != (d1 >= 0.0))
			{
				double f = d0 / (d0 - d1);
				struct corner *cut;

				if (made_count >= 8)
					return 0;
				cut = &made[made_count++];
				cut->ux = (float)(from->ux + ((double)to->ux - from->ux) * f);
				cut->uy = (float)(from->uy + ((double)to->uy - from->uy) * f);
				for (int k = 0; k < PARAMETERS; k++)
				{
					if (needed & (1u << k))
						cut->p[k] = (float)(from->p[k] + ((double)to->p[k] - from->p[k]) * f);
				}
				snap(cut, cut->ux, cut->uy);
				out[kept++] = cut;
			}
		}
		count = kept;
		swap = in;
		in = out;
		out = swap;
	}
	for (int i = 0; i < count; i++)
		polygon[i] = in[i];
	return count;
}

/* a triangle cut to the screen first where it is not on it (the card
takes coordinates within 2048 of its corner, and the front end leaves a
near triangle's corners far off the sides): what is left of it, a fan of
triangles. On the screen, the corners as they were snapped */
static void emit_clipped(const struct corner *a, const struct corner *b, const struct corner *c, unsigned needed)
{
	struct corner made[8];
	const struct corner *polygon[9];
	int count, valid[9];

	if (on_screen(a) && on_screen(b) && on_screen(c))
	{
		emit(a, b, c, needed);
		return;
	}
	count = clip_polygon(a, b, c, needed, made, polygon);
	/* (a corner not a number, cut from one that was not, is left out with
	its triangles, as their area, not a number, left them out) */
	for (int i = 0; i < count; i++)
		valid[i] = polygon[i]->ux == polygon[i]->ux && polygon[i]->uy == polygon[i]->uy;
	for (int i = 1; i + 1 < count; i++)
	{
		if (valid[0] && valid[i] && valid[i + 1])
			emit(polygon[0], polygon[i], polygon[i + 1], needed);
	}
}

static unsigned long screen_alpha_mode(void)
{
	switch (v.screen_blend)
	{
	case _r_screen_alpha: return BLEND_ALPHA;
	case _r_screen_add: return BLEND_ADD_ALPHA;
	case _r_screen_premultiplied: return BLEND_PREMULTIPLIED;
	default: return BLEND_NONE;
	}
}

/* the state of 2D drawing: a texture slot (or none), its blend */
static void state_2d(struct slot *slot, unsigned long alpha_mode)
{
	state_changed();
	set_register(FBZ_COLOR_PATH, &v.color_path, slot ? PATH_TEXTURED : PATH_COLOR);
	set_register(FBZ_MODE, &v.fbz_mode, MODE_2D);
	set_register(ALPHA_MODE, &v.alpha_mode, alpha_mode);
	set_register(FOG_MODE, &v.fog_mode, 0);
	if (slot)
		slot_select(slot, FALSE);
}

/* the parameters 2D drawing reads, in the blend given */
static unsigned needs_2d(const struct slot *slot, unsigned long alpha_mode)
{
	return NEEDS_COLOR | (alpha_mode ? NEEDS(P_A) : 0) | (slot ? NEEDS_TEXTURE : 0);
}

/* a corner's position in the card's pixels, and snapped (a float in
memory: rounded as it is stored) */
static __inline__ void place(struct corner *corner, float x, float y)
{
	corner->ux = x * v.scale_x;
	corner->uy = y * v.scale_y;
	__asm__("" : "+m"(corner->ux), "+m"(corner->uy));
	snap(corner, corner->ux, corner->uy);
}

/* a 2D quad in the picture's pixels: its colour (0xAARRGGBB, the texel
times it), its texture coordinates 0-1 across the slot's texture, in the
state state_2d set (alpha_mode its blend) */
static void quad_2d(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
	const struct slot *slot, unsigned long color, unsigned long alpha_mode)
{
	struct corner corners[4];
	const float xs[4] = { x0, x1, x1, x0 }, ys[4] = { y0, y0, y1, y1 };
	const float us[4] = { u0, u1, u1, u0 }, vs[4] = { v0, v0, v1, v1 };
	unsigned needed = needs_2d(slot, alpha_mode);

	for (int i = 0; i < 4; i++)
	{
		place(&corners[i], xs[i], ys[i]);
		corners[i].p[P_R] = (float)((color >> 16) & 0xFF);
		corners[i].p[P_G] = (float)((color >> 8) & 0xFF);
		corners[i].p[P_B] = (float)(color & 0xFF);
		corners[i].p[P_A] = (float)(color >> 24);
		corners[i].p[P_S] = slot ? us[i] * slot->s_scale : 0.0f;
		corners[i].p[P_T] = slot ? vs[i] * slot->t_scale : 0.0f;
	}
	emit(&corners[0], &corners[1], &corners[2], needed);
	emit(&corners[0], &corners[2], &corners[3], needed);
}

/* a 3D triangle's corner from its vertex: the position, the light (before
the front end's fog: the card fogs it again; 0x80 the texel as it is) and
fog, the depth, the texture's coordinates (the weapon's affine: S itself;
the world's over W) */
static void make_corner(const struct r_raster_vertex *vertex, struct corner *corner, int fog,
	const struct slot *slot)
{
	float w;

	if (v.in_front)
	{
		w = vertex->w * IN_FRONT_W_RANGE;
		w = 0.5f + 0.5f * (w > 1.0f ? 1.0f : w);
	}
	else
		w = vertex->w * WORLD_W;
	place(corner, vertex->x, vertex->y);
	if (!fog)
	{
		/* (the last vertex's light, most often an object's, lit evenly) */
		if (bits_of(&vertex->r) != v.last_light[0] || bits_of(&vertex->g) != v.last_light[1] ||
			bits_of(&vertex->b) != v.last_light[2])
		{
			v.last_light[0] = bits_of(&vertex->r);
			v.last_light[1] = bits_of(&vertex->g);
			v.last_light[2] = bits_of(&vertex->b);
			v.last_light_color[0] = color_byte(vertex->r * 2.0);
			v.last_light_color[1] = color_byte(vertex->g * 2.0);
			v.last_light_color[2] = color_byte(vertex->b * 2.0);
		}
		corner->p[P_R] = v.last_light_color[0];
		corner->p[P_G] = v.last_light_color[1];
		corner->p[P_B] = v.last_light_color[2];
	}
	else
	{
		double f = vertex->fog / (float)(R_FOG_LEVELS - 1);
		double light = 2.0 * (f < 0.999f ? 1.0 / (1.0 - f) : 1.0);

		corner->p[P_R] = color_byte(vertex->r * light);
		corner->p[P_G] = color_byte(vertex->g * light);
		corner->p[P_B] = color_byte(vertex->b * light);
		corner->p[P_Z] = (float)((f > 1.0 ? 1.0 : f) * 255.0 * 256.0);
	}
	corner->p[P_W] = w;
	if (slot)
	{
		/* (z rounded to a float, as ever) */
		float z = 1.0f / vertex->w;

		__asm__("" : "+m"(z));
		corner->p[P_S] = vertex->s * z * slot->s_scale * (v.in_front ? 1.0f : w);
		corner->p[P_T] = vertex->t * z * slot->t_scale * (v.in_front ? 1.0f : w);
	}
}

/* (the vertex's words a corner is made from: x, y, w, s, t; r, g, b; fog) */
static __inline__ int corner_made_from(const struct cached_corner *entry, const unsigned long *words)
{
	return entry->key[0] == words[0] && entry->key[1] == words[1] && entry->key[2] == words[2] &&
		entry->key[3] == words[3] && entry->key[4] == words[4] && entry->key[5] == words[7] &&
		entry->key[6] == words[8] && entry->key[7] == words[9] && entry->key[8] == words[14];
}

/* (what of a triangle's flags its state reads, its texture's alpha test,
and whether a corner is past the W buffer's reach) */
#define STATE_FLAGS (R_DRAW_ADDITIVE | R_DRAW_BLEND | R_DRAW_NO_DEPTH_TEST | R_DRAW_NO_FOG | R_DRAW_ALPHA)
#define STATE_ALPHA_TEST 0x10000
#define STATE_FAR 0x20000

static void voodoo_triangle(const struct r_raster_vertex *a, const struct r_raster_vertex *b,
	const struct r_raster_vertex *c, const struct r_texture *texture, unsigned flags)
{
	const struct r_raster_vertex *in[3] = { a, b, c };
	struct corner local[3], *corners[3];
	struct slot *slot = texture_slot_drawn(texture);
	unsigned long alpha_mode, fbz_mode, mode, alpha;
	unsigned indices[3], state_flags;

	if (flags & R_RASTER_SCREEN)
	{
		alpha_mode = screen_alpha_mode();
		if (texture && (texture->flags & R_TEXTURE_ALPHA_TEST))
			alpha_mode |= ALPHA_TEST;
		state_2d(slot, alpha_mode);
		for (int i = 0; i < 3; i++)
		{
			const struct r_raster_vertex *vertex = in[i];

			place(&local[i], vertex->x, vertex->y);
			local[i].p[P_R] = color_byte(vertex->r);
			local[i].p[P_G] = color_byte(vertex->g);
			local[i].p[P_B] = color_byte(vertex->b);
			local[i].p[P_A] = color_byte(vertex->fog_r);
			local[i].p[P_S] = slot ? vertex->u * slot->s_scale : 0.0f;
			local[i].p[P_T] = slot ? vertex->v * slot->t_scale : 0.0f;
		}
		emit_clipped(&local[0], &local[1], &local[2], needs_2d(slot, alpha_mode));
		return;
	}
	/* the state: as the last triangle's when nothing it reads differs
	(most of a draw's) */
	state_flags = (flags & STATE_FLAGS) | (texture && (texture->flags & R_TEXTURE_ALPHA_TEST) ? STATE_ALPHA_TEST : 0);
	if (!(flags & R_DRAW_NO_DEPTH_TEST) && !v.in_front &&
		(a->w * WORLD_W < FARTHEST_W || b->w * WORLD_W < FARTHEST_W || c->w * WORLD_W < FARTHEST_W))
	{
		state_flags |= STATE_FAR;
	}
	if (!v.state_valid || state_flags != v.state_flags || slot != v.state_slot)
	{
		float alpha_value = 255.0f;
		/* (added light is faded by the front end: R_DRAW_ADDITIVE) */
		int fog = v.fogged && !(flags & (R_DRAW_NO_FOG | R_DRAW_ADDITIVE));

		if (flags & R_DRAW_ADDITIVE)
		{
			alpha_mode = BLEND_ADD;
			fbz_mode = MODE_TRANSPARENT;
		}
		else if (flags & R_DRAW_BLEND)
		{
			alpha_mode = BLEND_ALPHA;
			fbz_mode = MODE_TRANSPARENT;
			alpha_value = (flags & R_DRAW_ALPHA) ? (float)(v.alpha_scale > 255 ? 255 : v.alpha_scale) : 128.0f;
		}
		else
		{
			alpha_mode = BLEND_NONE;
			fbz_mode = MODE_SOLID;
		}
		if (texture && (texture->flags & R_TEXTURE_ALPHA_TEST))
			alpha_mode |= ALPHA_TEST;
		if (flags & R_DRAW_NO_DEPTH_TEST)
			fbz_mode &= ~MODE_DEPTH_TEST;
		else if (state_flags & STATE_FAR)
		{
			/* (past the W buffer's reach, 2048 units off, the card's depth is
			the farthest there is, the cleared depth's: tested less, nothing
			there was drawn -- the skies, 1:1 about the eye, are mostly that
			far. Less or equal draws them, one over the other as they come) */
			fbz_mode = (fbz_mode & ~MODE_DEPTH_FUNCTION) | MODE_DEPTH_LESS_EQUAL;
		}
		set_register(FBZ_COLOR_PATH, &v.color_path, slot ? PATH_TEXTURED : PATH_COLOR);
		set_register(FBZ_MODE, &v.fbz_mode, fbz_mode);
		set_register(ALPHA_MODE, &v.alpha_mode, alpha_mode);
		set_register(FOG_MODE, &v.fog_mode, fog ? 0x11UL : 0UL);
		if (fog)
			set_register(FOG_COLOR, &v.fog_color, v.view_fog_color);
		if (slot)
			slot_select(slot, !v.in_front);
		v.state_valid = TRUE;
		v.state_flags = state_flags;
		v.state_slot = slot;
		v.state_needed = NEEDS_COLOR | NEEDS(P_W) | (alpha_mode ? NEEDS(P_A) : 0) | (fog ? NEEDS(P_Z) : 0) |
			(slot ? NEEDS_TEXTURE : 0);
		v.state_alpha = bits_of(&alpha_value);
		/* (what a corner is made with: the frame, fog, the weapon's depth,
		the texture's shape) */
		v.state_mode = (v.corner_generation << 8) | (unsigned long)fog | (v.in_front ? 2UL : 0UL) |
			(slot ? 4UL | ((bits_of(&slot->s_scale) >> 23) & 3) << 3 | ((bits_of(&slot->t_scale) >> 23) & 3) << 5 :
			0UL);
	}
	mode = v.state_mode;
	alpha = v.state_alpha;
	/* the corners: each vertex's made once (the cache's), but where two of
	this triangle's would share a place there */
	for (int i = 0; i < 3; i++)
	{
		const struct r_raster_vertex *vertex = in[i];
		const unsigned long *words = (const unsigned long *)vertex;
		unsigned index = ((unsigned long)vertex >> 2) & (CORNER_CACHE - 1);
		struct cached_corner *entry = &corner_cache[index];

		indices[i] = index;
		if (entry->mode == mode && corner_made_from(entry, words))
			corners[i] = &entry->corner;
		else if ((i >= 1 && index == indices[0] && vertex != in[0]) || (i == 2 && index == indices[1] && vertex != in[1]))
		{
			make_corner(vertex, &local[i], mode & 1, slot);
			corners[i] = &local[i];
		}
		else
		{
			make_corner(vertex, &entry->corner, mode & 1, slot);
			entry->mode = mode;
			entry->key[0] = words[0];
			entry->key[1] = words[1];
			entry->key[2] = words[2];
			entry->key[3] = words[3];
			entry->key[4] = words[4];
			entry->key[5] = words[7];
			entry->key[6] = words[8];
			entry->key[7] = words[9];
			entry->key[8] = words[14];
			corners[i] = &entry->corner;
		}
		*(unsigned long *)&corners[i]->p[P_A] = alpha;
	}
	emit_clipped(corners[0], corners[1], corners[2], v.state_needed);
}

/* ---------- the frame, rows, rectangles */

static void fast_fill(unsigned long mode, unsigned long color)
{
	state_changed();
	set_register(FBZ_MODE, &v.fbz_mode, mode);
	write_register(COLOR1, color & 0xFFFFFF);
	write_register(ZA_COLOR, 0xFFFF);
	write_register(FASTFILL_CMD, 0);
}

static void voodoo_clear(unsigned long color)
{
	v.corner_generation = (v.corner_generation + 1) & 0xFFFFFF;
	if (!v.corner_generation)
		v.corner_generation = 1;
	if (r_frame.width > 0 && r_frame.height > 0)
	{
		v.scale_x = (float)v.width / (float)r_frame.width;
		v.scale_y = (float)v.height / (float)r_frame.height;
	}
	v.smooth = r_frame.settings.texture_smooth || r_frame.settings.texture_dither;
	view_active = FALSE;
	clip_to(0, 0, v.width, v.height, TRUE);
	fast_fill(MODE_FILL, color);
	clip_view();
}

static void voodoo_clear_depth(int x0, int y0, int x1, int y1)
{
	clip_to((int)((float)x0 * v.scale_x), (int)((float)y0 * v.scale_y), (int)((float)x1 * v.scale_x + 0.999f),
		(int)((float)y1 * v.scale_y + 0.999f), TRUE);
	fast_fill(MODE_FILL_DEPTH, 0);
	clip_view();
}

static void voodoo_fill_row(int x0, int x1, int y, unsigned long color)
{
	if (x0 >= x1)
		return;
	state_2d(NULL, BLEND_NONE);
	quad_2d((float)x0, (float)y, (float)x1, (float)(y + 1), 0.0f, 0.0f, 0.0f, 0.0f, NULL, 0xFF000000UL | color,
		BLEND_NONE);
}

static void voodoo_screen_rect(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
	const struct r_texture *texture, unsigned long color)
{
	struct slot *slot = texture ? texture_slot(texture) : NULL;
	unsigned long alpha_mode = screen_alpha_mode();

	if (texture && (texture->flags & R_TEXTURE_ALPHA_TEST))
		alpha_mode |= ALPHA_TEST;
	state_2d(slot, alpha_mode);
	quad_2d(x0, y0, x1, y1, u0, v0, u1, v1, slot, color, alpha_mode);
}

static void voodoo_fill_rectangle(int x0, int y0, int x1, int y1, unsigned long argb)
{
	unsigned long alpha_mode = (argb >> 24) >= 255 ? BLEND_NONE : BLEND_ALPHA;

	if (x0 >= x1 || y0 >= y1)
		return;
	state_2d(NULL, alpha_mode);
	quad_2d((float)x0, (float)y0, (float)x1, (float)y1, 0.0f, 0.0f, 0.0f, 0.0f, NULL, argb, alpha_mode);
}

static void voodoo_set_near(float z_near)
{
	(void)z_near;
}

static void voodoo_set_in_front(int in_front)
{
	state_changed();
	v.in_front = in_front;
}

static void voodoo_set_alpha_scale(unsigned long scale)
{
	state_changed();
	v.alpha_scale = scale > 256 ? 256 : scale;
}

static void voodoo_screen_blend(int blend)
{
	v.screen_blend = blend;
}

static void voodoo_view_fog(int fogged, float fog_start, float fog_end, unsigned long fog_color)
{
	(void)fog_start;
	(void)fog_end;
	state_changed();
	v.fogged = fogged;
	v.view_fog_color = fog_color & 0xFFFFFF;
}

/* ---------- text and pictures */

static int power_of_two_shift(int value, int least)
{
	int shift = 0;

	while ((1 << shift) < value || (1 << shift) < least)
		shift++;
	return shift;
}

static void voodoo_coverage(int x, int y, const unsigned char *cover, int width, int height, unsigned long argb,
	unsigned long *handle, int clip_x0, int clip_y0, int clip_x1, int clip_y1)
{
	struct slot *slot = *handle ? slot_of(*handle) : NULL;
	int width_shift = power_of_two_shift(width, 8), height_shift = power_of_two_shift(height, 8);
	unsigned long alpha = argb >> 24, color;

	if (width <= 0 || height <= 0 || x >= clip_x1 || y >= clip_y1 || x + width <= clip_x0 || y + height <= clip_y0)
		return;
	if (width_shift - height_shift > 3)
		height_shift = width_shift - 3;
	if (height_shift - width_shift > 3)
		width_shift = height_shift - 3;
	if (!slot)
	{
		/* (an A8 texture: the glyph's coverage, the rest none) */
		int index = slot_make(width_shift, height_shift, 1, FORMAT_A8, TRUE, TRUE);

		if (index < 0)
			return;
		slot = &v.slots[index];
		for (int t = 0; t < (1 << height_shift); t++)
		{
			for (int s = 0; s < (1 << width_shift); s += 4)
			{
				unsigned long word = 0;

				for (int k = 0; k < 4; k++)
				{
					unsigned long value = s + k < width && t < height ? cover[t * width + s + k] : 0;

					word |= value << (8 * k);
				}
				texel_write(8 - (width_shift > height_shift ? width_shift : height_shift), t, s, word);
			}
		}
		*handle = handle_of((int)(slot - v.slots));
	}
	slot->last_used = ++v.uses;
	/* (premultiplied: the texel's alpha in its colour too) */
	color = (alpha << 24) | ((((argb >> 16) & 0xFF) * alpha / 255) << 16) | ((((argb >> 8) & 0xFF) * alpha / 255) << 8) |
		((argb & 0xFF) * alpha / 255);
	state_2d(slot, BLEND_PREMULTIPLIED);
	clip_to((int)((float)clip_x0 * v.scale_x), (int)((float)clip_y0 * v.scale_y),
		(int)((float)clip_x1 * v.scale_x + 0.999f), (int)((float)clip_y1 * v.scale_y + 0.999f), FALSE);
	quad_2d((float)x, (float)y, (float)(x + width), (float)(y + height), 0.0f, 0.0f,
		(float)width / (float)(1 << width_shift), (float)height / (float)(1 << height_shift), slot, color,
		BLEND_PREMULTIPLIED);
	clip_view();
}

static void voodoo_coverage_forget(unsigned long handle)
{
	struct slot *slot = slot_of(handle);

	if (slot)
		slot_free(slot);
}

/* a picture's 256x256 tile (column, row) as an ARGB4444 texture */
static unsigned long picture_tile(const unsigned long *argb, int width, int height, int column, int row)
{
	int index = slot_make(8, 8, 1, FORMAT_ARGB4444, TRUE, FALSE);

	if (index < 0)
		return 0;
	for (int t = 0; t < 256; t++)
	{
		int y = row * 256 + t;

		for (int s = 0; s < 256; s += 2)
		{
			unsigned long pair = 0;

			for (int k = 0; k < 2; k++)
			{
				int x = column * 256 + s + k;
				unsigned long color = x < width && y < height ? argb[(size_t)y * width + x] : 0;

				pair |= texel16(color, FORMAT_ARGB4444) << (16 * k);
			}
			texel_write(0, t, s, pair);
		}
	}
	return handle_of(index);
}

static void voodoo_picture(int x, int y, const unsigned long *argb, int width, int height)
{
	unsigned long check = 0;
	int slot_index = -1, oldest = 0, columns = (width + 255) / 256, rows = (height + 255) / 256;

	if (width <= 0 || height <= 0 || columns * rows > 12)
		return;
	for (int i = 0; i < 8; i++)
		check = check * 31 + argb[(size_t)i * width * height / 8];
	v.clock++;
	for (int i = 0; i < 4; i++)
	{
		if (v.pictures[i].argb == argb && v.pictures[i].width == width && v.pictures[i].height == height &&
			v.pictures[i].check == check)
		{
			slot_index = i;
			break;
		}
		if (v.pictures[i].used < v.pictures[oldest].used)
			oldest = i;
	}
	if (slot_index < 0)
	{
		slot_index = oldest;
		for (int i = 0; i < v.pictures[slot_index].columns * v.pictures[slot_index].rows; i++)
			voodoo_coverage_forget(v.pictures[slot_index].tiles[i]);
		v.pictures[slot_index].argb = argb;
		v.pictures[slot_index].width = width;
		v.pictures[slot_index].height = height;
		v.pictures[slot_index].check = check;
		v.pictures[slot_index].columns = columns;
		v.pictures[slot_index].rows = rows;
		memset(v.pictures[slot_index].tiles, 0, sizeof(v.pictures[slot_index].tiles));
	}
	v.pictures[slot_index].used = v.clock;
	for (int row = 0; row < rows; row++)
	{
		for (int column = 0; column < columns; column++)
		{
			unsigned long *handle = &v.pictures[slot_index].tiles[row * columns + column];
			struct slot *slot = *handle ? slot_of(*handle) : NULL;
			int tile_width = width - column * 256 < 256 ? width - column * 256 : 256;
			int tile_height = height - row * 256 < 256 ? height - row * 256 : 256;

			if (!slot)
			{
				*handle = picture_tile(argb, width, height, column, row);
				slot = *handle ? slot_of(*handle) : NULL;
				if (!slot)
					continue;
			}
			slot->last_used = ++v.uses;
			state_2d(slot, BLEND_ALPHA);
			quad_2d((float)(x + column * 256), (float)(y + row * 256), (float)(x + column * 256 + tile_width),
				(float)(y + row * 256 + tile_height), 0.0f, 0.0f, (float)tile_width / 256.0f,
				(float)tile_height / 256.0f, slot, 0xFFFFFFFFUL, BLEND_ALPHA);
		}
	}
}

/* ---------- reading back, showing */

/* a row of the back buffer, 16-bit pixels (the linear frame buffer's rows
are 2048 bytes apart whatever the card's own are) */
static void read_row(int y, unsigned short *pixels)
{
	for (int x = 0; x < v.width; x += 2)
	{
		unsigned long pair = v.base[(LFB + (unsigned long)y * 2048 + (unsigned long)x * 2) >> 2];

		pixels[x] = (unsigned short)(pair & 0xFFFF);
		pixels[x + 1] = (unsigned short)(pair >> 16);
	}
}

/* each pixel of the picture the average of the card's under it: the
card's are dithered to 16 bits, and one of them in two or three (a
320x200 picture's) showed the dither's pattern as stripes */
static void voodoo_read_back(void)
{
	static unsigned short line[1024];
	unsigned long *sums;
	int *columns;

	if (!r_frame.pixels || r_frame.bits != 32 || r_frame.width <= 0 || r_frame.height <= 0 || v.width > 1024)
		return;
	sums = malloc((size_t)r_frame.width * 4 * sizeof(unsigned long));
	columns = malloc(((size_t)r_frame.width + 1) * sizeof(int));
	if (!sums || !columns)
	{
		free(sums);
		free(columns);
		return;
	}
	/* (the card's columns of each: at least the one under its corner) */
	for (int x = 0; x <= r_frame.width; x++)
	{
		int card_x = (int)((float)x * v.scale_x);

		columns[x] = card_x < v.width ? card_x : v.width;
	}
	/* (the back buffer, once the card has drawn all there is) */
	write_register(LFB_MODE, 0x50);
	wait_for_idle();
	for (int y = 0; y < r_frame.height; y++)
	{
		unsigned long *row = (unsigned long *)(r_frame.pixels + (size_t)y * r_frame.pitch);
		int top = (int)((float)y * v.scale_y), bottom = (int)((float)(y + 1) * v.scale_y);

		if (top >= v.height)
			top = v.height - 1;
		if (bottom > v.height)
			bottom = v.height;
		if (bottom <= top)
			bottom = top + 1;
		memset(sums, 0, (size_t)r_frame.width * 4 * sizeof(unsigned long));
		for (int card_y = top; card_y < bottom; card_y++)
		{
			read_row(card_y, line);
			for (int x = 0; x < r_frame.width; x++)
			{
				int first = columns[x] < v.width ? columns[x] : v.width - 1, last = columns[x + 1];

				if (last <= first)
					last = first + 1;
				for (int card_x = first; card_x < last; card_x++)
				{
					unsigned long pixel = line[card_x], r = (pixel >> 11) & 0x1F, g = (pixel >> 5) & 0x3F, b = pixel & 0x1F;

					sums[x * 4] += (r << 3) | (r >> 2);
					sums[x * 4 + 1] += (g << 2) | (g >> 4);
					sums[x * 4 + 2] += (b << 3) | (b >> 2);
					sums[x * 4 + 3]++;
				}
			}
		}
		for (int x = 0; x < r_frame.width; x++)
		{
			unsigned long n = sums[x * 4 + 3], half = n / 2;

			row[x] = ((sums[x * 4] + half) / n << 16) | ((sums[x * 4 + 1] + half) / n << 8) | ((sums[x * 4 + 2] + half) / n);
		}
	}
	write_register(LFB_MODE, 0x10);
	free(sums);
	free(columns);
}

void r_voodoo_swap(void)
{
	if (!v.base)
		return;
	/* (at the vertical blank on a real card; at once in DOSBox-X) */
	write_register(SWAPBUFFER_CMD, 1);
}

/* the back buffer's first two pixels, as the card has drawn them */
static unsigned long first_pixels(void)
{
	unsigned long pair;

	write_register(LFB_MODE, 0x50);
	wait_for_idle();
	pair = v.base[LFB >> 2];
	write_register(LFB_MODE, 0x10);
	return pair;
}

/* the first texture unit's memory: 4 MB when a texel pair written 2 MB in
is still there after one written at 0 (on a unit of 2 MB the addresses
wrap, and the second overwrote the first), drawn and read back as Glide
finds it (its sst1InitGetTmuMemory); else what the card was said to have */
static unsigned long sense_texture_memory(unsigned long least)
{
	const unsigned long far_pair = 0x92F56EB0UL, near_pair = 0xBADBEEF1UL;
	struct slot sense;

	if (least >= (4UL << 20))
		return least;
	memset(&sense, 0, sizeof(sense));
	sense.s_scale = sense.t_scale = 256.0f;
	/* (one level of 256 x 256, as it is: no perspective, filter or dither) */
	set_register(TMU0 + TEXTURE_MODE, &v.texture_mode, TEXTURE_LOCAL | ((unsigned long)FORMAT_RGB565 << 8) |
		TEXTURE_CLAMP);
	set_register(TMU0 + T_LOD, &v.t_lod, 0);
	set_register(TMU0 + TEX_BASE_ADDR, &v.tex_base, (2UL << 20) >> 3);
	texel_write(0, 0, 0, far_pair);
	set_register(TMU0 + TEX_BASE_ADDR, &v.tex_base, 0);
	texel_write(0, 0, 0, near_pair);
	set_register(TMU0 + TEX_BASE_ADDR, &v.tex_base, (2UL << 20) >> 3);
	set_register(FBZ_COLOR_PATH, &v.color_path, PATH_TEXTURED);
	set_register(FBZ_MODE, &v.fbz_mode, MODE_2D & ~0x100UL);
	set_register(ALPHA_MODE, &v.alpha_mode, BLEND_NONE);
	set_register(FOG_MODE, &v.fog_mode, 0);
	quad_2d(0.0f, 0.0f, 2.0f, 1.0f, 0.0f, 0.0f, 2.0f / 256.0f, 1.0f / 256.0f, &sense, 0xFFFFFFFFUL, BLEND_NONE);
	return first_pixels() == far_pair ? 4UL << 20 : least;
}

int r_voodoo_use(struct r_voodoo_card *card)
{
	memset(&v, 0, sizeof(v));
	memset(corner_cache, 0, sizeof(corner_cache));
	v.corner_generation = 1;
	v.base = card->base;
	v.floats = (volatile float *)card->base;
	v.width = card->width;
	v.height = card->height;
	{
		float width = (float)v.width, height = (float)v.height;

		v.width_bits = bits_of(&width);
		v.height_bits = bits_of(&height);
	}
	v.scale_x = v.scale_y = 1.0f;
	v.alpha_scale = 256;
	v.color_path = v.fog_mode = v.alpha_mode = v.fbz_mode = v.texture_mode = v.t_lod = v.tex_base = v.fog_color =
		0xFFFFFFFFUL;
	v.clip_x = v.clip_y = 0xFFFFFFFFUL;
	view_active = FALSE;
	narrow_tables();
	write_register(TMU0 + T_DETAIL, 0);
	write_register(LFB_MODE, 0x10);
	clip_to(0, 0, v.width, v.height, TRUE);
	/* (both buffers cleared: the front one is shown at once) */
	fast_fill(MODE_FILL & ~0xC000UL, 0);
	fast_fill(MODE_FILL, 0);
	/* how the card takes a clip rectangle's right edge: a white row 8
	pixels across clipped at 4, read back. The emulation stops a span that
	reaches the edge a pixel short of it (pixel 3 left black), so the
	triangles' right edges go a pixel further; a card that draws pixel 3 (or
	whose pixels cannot be read) takes the edge as it is */
	clip_to(0, 0, 4, 1, TRUE);
	state_2d(NULL, BLEND_NONE);
	quad_2d(0.0f, 0.0f, 8.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, NULL, 0xFFFFFFFFUL, BLEND_NONE);
	write_register(LFB_MODE, 0x50);
	wait_for_idle();
	{
		unsigned long pair = v.base[(LFB + 4) >> 2];

		v.clip_extra = (pair & 0xFFFF) == 0xFFFF && (pair >> 16) != 0xFFFF;
	}
	write_register(LFB_MODE, 0x10);
	clip_view();
	v.memory = sense_texture_memory(card->texture_memory);
	v.largest_shift = 6;
	clip_to(0, 0, v.width, v.height, TRUE);
	fast_fill(MODE_FILL, 0);
	clip_view();
	card->texture_memory = v.memory;
	card->clip_extra = v.clip_extra;
	r_backend = &voodoo_backend;
	return TRUE;
}

static const struct r_backend voodoo_backend =
{
	voodoo_triangle,
	voodoo_screen_blend,
	voodoo_clear,
	voodoo_fill_row,
	voodoo_screen_rect,
	voodoo_set_near,
	voodoo_set_in_front,
	voodoo_set_alpha_scale,
	voodoo_view_fog,
	voodoo_clear_depth,
	voodoo_coverage,
	voodoo_coverage_forget,
	voodoo_picture,
	voodoo_fill_rectangle,
	voodoo_texture_delete,
	voodoo_read_back,
	NULL,
	NULL,
	voodoo_view_rect,
};
