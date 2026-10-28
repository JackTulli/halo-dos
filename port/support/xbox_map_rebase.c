/*
XBOX_MAP_REBASE.C

Moving the retail maps' absolute addresses to another memory window
(xbox_map_rebase.h).

The walk starts at the fixed structures (the tag header and its instance
table, a structure BSP's header, the D3D header arrays) and follows every
tag block through the layouts of xbox_map_layouts.h, so every address it
moves comes from a field known to hold one. Each pointer is checked before
it is moved or followed: an array must lie inside the data, a name must end
inside it, a vertex buffer's format must be one of the D3D headers, and so
on. Anything out of place stops the walk with a sentence naming the tag,
the field and the value; the data is then partly moved and must not be
used.

A map of one bit per word records the words already moved, so an array
that two fields reach is moved once; a moved word reads back as the value
it was linked with.
*/

#include "xbox_map_rebase.h"
#include "xbox_map_layouts.h"

#include <stdlib.h>

/* ---------- constants */

#define MAXIMUM_DEPTH 16
#define NAME_LENGTH_LIMIT 256
#define TAG_NAME_LENGTH_SHOWN 64

typedef char xbox_map_rebase_word_size_assert[sizeof(unsigned long) == 4 ? 1 : -1];
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error the maps' words are little-endian, and so must this build be
#endif

/* where the pointer is in each kind of field */
static const unsigned char field_pointer_offsets[NUMBER_OF_XBOX_MAP_FIELD_KINDS] =
{
	4, /* tag_block address */
	4, /* tag_reference name */
	12, /* tag_data address */
	0, 0, 0, 0
};

/* ---------- structures */

struct rebase_step
{
	unsigned long field_offset;
	unsigned long element_index;
};

struct rebase
{
	unsigned char *data;
	unsigned long size;
	unsigned long linked_address;
	unsigned long delta;
	/* what a pointer to outside the data moves by (a BSP's tag names, in
	the tag data, when the BSP is not where the tag data's window puts it:
	xbox_map_rebase_structure_bsp_apart); delta otherwise */
	unsigned long outside_delta;
	unsigned long *moved;
	/* the words moved by outside_delta */
	unsigned long *moved_outside;

	/* the D3D headers a format field may point at */
	unsigned long vertex_headers[2];
	unsigned long vertex_header_counts[2];
	unsigned long index_headers;
	unsigned long index_header_count;

	/* where a tag reference's name may point: inside the data, ending
	there (the tag data), or in [names_begin, names_end) (a BSP's names
	are in the tag data below it) */
	int names_inside;
	unsigned long names_begin;
	unsigned long names_end;

	/* where a scenario's structure BSPs may load: the tag cache past the
	tag data */
	unsigned long bsps_begin;
	unsigned long bsps_end;

	/* each tag reference's index word, handed over as it is walked
	(xbox_map_rebase_tag_data_visiting) */
	void (*visit_reference)(void *context, unsigned long *index);
	void *visit_context;

	/* what is being walked, for the error */
	const char *what;
	long tag_index;
	unsigned long group_tag;
	unsigned long tag_name;
	struct rebase_step steps[MAXIMUM_DEPTH];
	int depth;

	char *error;
	unsigned long error_size;
	unsigned long error_length;
};

/* ---------- errors */

static void error_text(struct rebase *rebase, const char *text)
{
	if (!rebase->error || !rebase->error_size)
		return;
	while (*text && rebase->error_length + 1 < rebase->error_size)
		rebase->error[rebase->error_length++] = *text++;
	rebase->error[rebase->error_length] = 0;
}

static void error_number(struct rebase *rebase, unsigned long value, int hexadecimal)
{
	char text[16];
	int position = sizeof(text) - 1;

	text[position] = 0;
	do
	{
		unsigned long digit = hexadecimal ? value & 0xF : value % 10;

		text[--position] = (char)(digit < 10 ? '0' + digit : 'A' + digit - 10);
		value = hexadecimal ? value >> 4 : value / 10;
	}
	while (value && position > 2);
	if (hexadecimal)
	{
		text[--position] = 'x';
		text[--position] = '0';
	}
	error_text(rebase, text + position);
}

static void error_hexadecimal(struct rebase *rebase, unsigned long value)
{
	error_number(rebase, value, 1);
}

static void error_decimal(struct rebase *rebase, unsigned long value)
{
	error_number(rebase, value, 0);
}

static void error_tag(struct rebase *rebase)
{
	char group[5];
	int k;

	error_text(rebase, rebase->what);
	if (rebase->tag_index < 0)
		return;
	for (k = 0; k < 4; k++)
	{
		char c = (char)(rebase->group_tag >> (24 - 8 * k));

		group[k] = c >= 32 && c < 127 ? c : '?';
	}
	group[4] = 0;
	error_text(rebase, " ");
	error_decimal(rebase, (unsigned long)rebase->tag_index);
	error_text(rebase, " (");
	error_text(rebase, group);
	if (rebase->tag_name)
	{
		const char *name = (const char *)rebase->data + (rebase->tag_name - rebase->linked_address);
		char shown[TAG_NAME_LENGTH_SHOWN + 1];

		for (k = 0; k < TAG_NAME_LENGTH_SHOWN && name[k]; k++)
			shown[k] = name[k] >= 32 && name[k] < 127 ? name[k] : '?';
		shown[k] = 0;
		error_text(rebase, " ");
		error_text(rebase, shown);
	}
	error_text(rebase, ")");
}

/* "<the tag>, field +0x5a4[2]+0x10 holds <what> <value>" */
static int fail(struct rebase *rebase, unsigned long field_offset, const char *what, unsigned long value,
	const char *after)
{
	int k;

	rebase->error_length = 0;
	error_tag(rebase);
	error_text(rebase, ", field ");
	for (k = 0; k < rebase->depth; k++)
	{
		error_text(rebase, "+");
		error_hexadecimal(rebase, rebase->steps[k].field_offset);
		error_text(rebase, "[");
		error_decimal(rebase, rebase->steps[k].element_index);
		error_text(rebase, "]");
	}
	error_text(rebase, "+");
	error_hexadecimal(rebase, field_offset);
	error_text(rebase, " holds ");
	error_text(rebase, what);
	error_text(rebase, " ");
	error_hexadecimal(rebase, value);
	error_text(rebase, after);
	return 0;
}

/* "<the data>: <what> <value><after>" */
static int fail_fixed(struct rebase *rebase, const char *what, unsigned long value, const char *after)
{
	rebase->error_length = 0;
	error_text(rebase, rebase->what);
	error_text(rebase, ": ");
	error_text(rebase, what);
	error_text(rebase, " ");
	error_hexadecimal(rebase, value);
	error_text(rebase, after);
	return 0;
}

/* ---------- words */

static int inside(const struct rebase *rebase, unsigned long address, unsigned long length)
{
	unsigned long offset = address - rebase->linked_address;

	return offset <= rebase->size && length <= rebase->size - offset;
}

/* the value a word was linked with; the address is inside and 4-aligned */
static unsigned long word_at(const struct rebase *rebase, unsigned long address)
{
	unsigned long index = (address - rebase->linked_address) >> 2;
	unsigned long value = ((const unsigned long *)rebase->data)[index];

	if (rebase->moved[index >> 5] & (1UL << (index & 31)))
		value -= (rebase->moved_outside[index >> 5] & (1UL << (index & 31))) ? rebase->outside_delta : rebase->delta;
	return value;
}

static int moved(const struct rebase *rebase, unsigned long address)
{
	unsigned long index = (address - rebase->linked_address) >> 2;

	return (rebase->moved[index >> 5] & (1UL << (index & 31))) != 0;
}

static void move(struct rebase *rebase, unsigned long address)
{
	unsigned long index = (address - rebase->linked_address) >> 2;
	unsigned long *word = &((unsigned long *)rebase->data)[index];

	if (rebase->outside_delta != rebase->delta && !inside(rebase, *word, 1))
	{
		*word += rebase->outside_delta;
		rebase->moved_outside[index >> 5] |= 1UL << (index & 31);
	}
	else
	{
		*word += rebase->delta;
	}
	rebase->moved[index >> 5] |= 1UL << (index & 31);
}

static int is_header(unsigned long address, unsigned long first, unsigned long count)
{
	unsigned long offset = address - first;

	return offset < count * XBOX_MAP_D3D_HEADER_SIZE && offset % XBOX_MAP_D3D_HEADER_SIZE == 0;
}

static int name_ends_inside(const struct rebase *rebase, unsigned long address)
{
	const unsigned char *name;
	unsigned long length;
	unsigned long k;

	if (!inside(rebase, address, 1))
		return 0;
	name = rebase->data + (address - rebase->linked_address);
	length = rebase->size - (address - rebase->linked_address);
	if (length > NAME_LENGTH_LIMIT)
		length = NAME_LENGTH_LIMIT;
	for (k = 0; k < length; k++)
	{
		if (!name[k])
			return 1;
	}
	return 0;
}

static int name_valid(const struct rebase *rebase, unsigned long address)
{
	if (rebase->names_inside)
		return name_ends_inside(rebase, address);
	return address >= rebase->names_begin && address < rebase->names_end;
}

/* ---------- the walk */

static int walk_array(struct rebase *rebase, unsigned long layout_index, unsigned long address, unsigned long count);

static int walk_field(struct rebase *rebase, const struct xbox_map_field *field, unsigned long element)
{
	unsigned long field_address = element + field->offset;
	unsigned long pointer_address = field_address + field_pointer_offsets[field->kind];
	unsigned long value = word_at(rebase, pointer_address);

	if (!value || moved(rebase, pointer_address))
		return 1;
	switch (field->kind)
	{
	case _xbox_map_field_tag_block:
	{
		unsigned long count = word_at(rebase, field_address);
		unsigned long element_size = xbox_map_layouts[field->layout].size;
		int result;

		if (!count || count >= 0x80000000UL)
			return fail(rebase, field->offset, "a tag block of", count, " elements");
		if (word_at(rebase, field_address + 8))
			return fail(rebase, field->offset, "a tag block with a definition", word_at(rebase, field_address + 8), "");
		if (count > rebase->size / element_size || !inside(rebase, value, count * element_size))
			return fail(rebase, field->offset, "a tag block outside the data at", value, "");
		if ((value & 3) && xbox_map_layouts[field->layout].field_count)
			return fail(rebase, field->offset, "a tag block of pointers at an odd address", value, "");
		move(rebase, pointer_address);
		if (rebase->depth >= MAXIMUM_DEPTH)
			return fail(rebase, field->offset, "a tag block nested too deep at", value, "");
		rebase->steps[rebase->depth].field_offset = field->offset;
		rebase->steps[rebase->depth].element_index = 0;
		rebase->depth++;
		result = walk_array(rebase, field->layout, value, count);
		rebase->depth--;
		return result;
	}
	case _xbox_map_field_tag_reference:
		if (!name_valid(rebase, value))
			return fail(rebase, field->offset, "a tag reference whose name is at", value, ", outside the names");
		if (rebase->visit_reference)
		{
			rebase->visit_reference(rebase->visit_context,
				&((unsigned long *)rebase->data)[(field_address + 12 - rebase->linked_address) >> 2]);
		}
		break;
	case _xbox_map_field_tag_data:
	{
		unsigned long size = word_at(rebase, field_address);

		if (size >= 0x80000000UL || !inside(rebase, value, size ? size : 1))
			return fail(rebase, field->offset, "tag data outside the data at", value, "");
		break;
	}
	case _xbox_map_field_vertex_buffer_format:
		if (!is_header(value, rebase->vertex_headers[0], rebase->vertex_header_counts[0]) &&
			!is_header(value, rebase->vertex_headers[1], rebase->vertex_header_counts[1]))
		{
			return fail(rebase, field->offset, "a vertex buffer format that is no vertex buffer header:", value, "");
		}
		break;
	case _xbox_map_field_index_buffer_format:
		if (!is_header(value, rebase->index_headers, rebase->index_header_count))
			return fail(rebase, field->offset, "an index buffer format that is no index buffer header:", value, "");
		break;
	case _xbox_map_field_index_buffer_data:
	{
		unsigned long header = word_at(rebase, pointer_address + 4);

		if (!is_header(header, rebase->index_headers, rebase->index_header_count) ||
			word_at(rebase, header + XBOX_MAP_D3D_HEADER_DATA) != value)
		{
			return fail(rebase, field->offset, "index data that is not its index buffer header's:", value, "");
		}
		break;
	}
	case _xbox_map_field_structure_bsp:
	{
		unsigned long size = word_at(rebase, pointer_address - 4);

		if (value < rebase->bsps_begin || value > rebase->bsps_end || size > rebase->bsps_end - value)
			return fail(rebase, field->offset, "a structure BSP outside the tag cache at", value, "");
		break;
	}
	default:
		return fail(rebase, field->offset, "a field of the unknown kind", field->kind, "");
	}
	move(rebase, pointer_address);
	return 1;
}

/* the caller has checked that the array lies inside the data and, when its
elements hold pointers, is 4-aligned */
static int walk_array(struct rebase *rebase, unsigned long layout_index, unsigned long address, unsigned long count)
{
	const struct xbox_map_layout *layout = &xbox_map_layouts[layout_index];
	const struct xbox_map_field *fields = &xbox_map_fields[layout->first_field];
	unsigned long index;

	if (!layout->field_count)
		return 1;
	for (index = 0; index < count; index++)
	{
		unsigned long element = address + index * layout->size;
		unsigned long k;

		if (rebase->depth)
			rebase->steps[rebase->depth - 1].element_index = index;
		for (k = 0; k < layout->field_count; k++)
		{
			if (!walk_field(rebase, &fields[k], element))
				return 0;
		}
	}
	return 1;
}

static int walk_headers(struct rebase *rebase, unsigned long first, unsigned long count, const char *what)
{
	unsigned long k;

	if (!count)
		return 1;
	if ((first & 3) || count > rebase->size / XBOX_MAP_D3D_HEADER_SIZE ||
		!inside(rebase, first, count * XBOX_MAP_D3D_HEADER_SIZE))
	{
		return fail_fixed(rebase, what, first, " lie outside the data");
	}
	for (k = 0; k < count; k++)
	{
		unsigned long address = first + k * XBOX_MAP_D3D_HEADER_SIZE + XBOX_MAP_D3D_HEADER_DATA;
		unsigned long data = word_at(rebase, address);

		if (!data || moved(rebase, address))
			continue;
		if (!inside(rebase, data, 1))
			return fail_fixed(rebase, "a D3D header's Data lies outside the data at", data, "");
		move(rebase, address);
	}
	return 1;
}

static void move_if_set(struct rebase *rebase, unsigned long address)
{
	if (word_at(rebase, address) && !moved(rebase, address))
		move(rebase, address);
}

static int group_layout(unsigned long group_tag, unsigned long *layout)
{
	int low = 0;
	int high = NUMBER_OF_XBOX_MAP_GROUP_LAYOUTS - 1;

	while (low <= high)
	{
		int middle = (low + high) / 2;
		unsigned long tag = xbox_map_group_layouts[middle].group_tag;

		if (tag == group_tag)
		{
			*layout = xbox_map_group_layouts[middle].layout;
			return 1;
		}
		if (tag < group_tag)
			low = middle + 1;
		else
			high = middle - 1;
	}
	return 0;
}

static int rebase_begin(struct rebase *rebase, void *data, unsigned long size, unsigned long linked_address,
	long delta, char *error, unsigned long error_size, const char *what)
{
	unsigned long words = (size + 3) / 4;

	rebase->data = (unsigned char *)data;
	rebase->size = size;
	rebase->linked_address = linked_address;
	rebase->delta = (unsigned long)delta;
	rebase->outside_delta = (unsigned long)delta;
	rebase->vertex_headers[0] = rebase->vertex_headers[1] = 0;
	rebase->vertex_header_counts[0] = rebase->vertex_header_counts[1] = 0;
	rebase->index_headers = 0;
	rebase->index_header_count = 0;
	rebase->names_inside = 0;
	rebase->names_begin = rebase->names_end = 0;
	rebase->bsps_begin = rebase->bsps_end = 0;
	rebase->what = what;
	rebase->tag_index = -1;
	rebase->group_tag = 0;
	rebase->tag_name = 0;
	rebase->depth = 0;
	rebase->error = error;
	rebase->error_size = error_size;
	rebase->error_length = 0;
	if (error && error_size)
		error[0] = 0;
	rebase->moved = NULL;
	rebase->moved_outside = NULL;
	rebase->visit_reference = NULL;
	rebase->visit_context = NULL;
	if (((size_t)data & 3) || (linked_address & 3))
		return fail_fixed(rebase, "the data is not 4-aligned at", linked_address, "");
	if ((unsigned long)delta & 3)
		return fail_fixed(rebase, "the displacement is not a multiple of 4:", (unsigned long)delta, "");
	if (size > 0xFFFFFFFFUL - linked_address)
		return fail_fixed(rebase, "the data runs past the end of the address space from", linked_address, "");
	rebase->moved = (unsigned long *)calloc(2 * ((words + 31) / 32 + 1), sizeof(unsigned long));
	if (!rebase->moved)
		return fail_fixed(rebase, "no memory for the map of", words, " words");
	rebase->moved_outside = rebase->moved + (words + 31) / 32 + 1;
	return 1;
}

static void rebase_end(struct rebase *rebase)
{
	free(rebase->moved);
	rebase->moved = NULL;
	rebase->moved_outside = NULL;
}

/* ---------- public code */

const char *xbox_map_rebase_build(void)
{
	return XBOX_MAP_LAYOUTS_BUILD;
}

int xbox_map_rebase_tag_data(void *data, unsigned long size, unsigned long linked_address, long delta,
	char *error, unsigned long error_size)
{
	return xbox_map_rebase_tag_data_visiting(data, size, linked_address, delta, NULL, NULL, error, error_size);
}

int xbox_map_rebase_tag_data_visiting(void *data, unsigned long size, unsigned long linked_address, long delta,
	void (*visit_reference)(void *context, unsigned long *index), void *context, char *error,
	unsigned long error_size)
{
	struct rebase rebase;
	unsigned long instances;
	unsigned long count;
	unsigned long k;
	int result = 1;

	if (!rebase_begin(&rebase, data, size, linked_address, delta, error, error_size, "the tag data"))
		return 0;
	rebase.visit_reference = visit_reference;
	rebase.visit_context = context;
	if (size < XBOX_MAP_TAG_HEADER_SIZE ||
		word_at(&rebase, linked_address + XBOX_MAP_TAG_HEADER_SIGNATURE) != XBOX_MAP_TAG_HEADER_SIGNATURE_VALUE)
	{
		rebase_end(&rebase);
		return fail_fixed(&rebase, "the tag header has no signature; its size is", size, "");
	}
	rebase.names_inside = 1;
	rebase.bsps_begin = linked_address + size;
	rebase.bsps_end = linked_address <= 0xFFFFFFFFUL - XBOX_MAP_TAG_CACHE_SIZE ?
		linked_address + XBOX_MAP_TAG_CACHE_SIZE : 0xFFFFFFFFUL;
	instances = word_at(&rebase, linked_address + XBOX_MAP_TAG_HEADER_INSTANCES);
	count = word_at(&rebase, linked_address + XBOX_MAP_TAG_HEADER_INSTANCE_COUNT);
	rebase.vertex_headers[0] = word_at(&rebase, linked_address + XBOX_MAP_TAG_HEADER_VERTEX_BUFFERS);
	rebase.vertex_header_counts[0] = word_at(&rebase, linked_address + XBOX_MAP_TAG_HEADER_VERTEX_BUFFER_COUNT);
	rebase.index_headers = word_at(&rebase, linked_address + XBOX_MAP_TAG_HEADER_INDEX_BUFFERS);
	rebase.index_header_count = word_at(&rebase, linked_address + XBOX_MAP_TAG_HEADER_INDEX_BUFFER_COUNT);
	if ((instances & 3) || count > size / XBOX_MAP_TAG_INSTANCE_SIZE ||
		!inside(&rebase, instances, count * XBOX_MAP_TAG_INSTANCE_SIZE))
	{
		result = fail_fixed(&rebase, "the tag instances lie outside the data at", instances, "");
	}
	if (result)
	{
		move_if_set(&rebase, linked_address + XBOX_MAP_TAG_HEADER_INSTANCES);
		move_if_set(&rebase, linked_address + XBOX_MAP_TAG_HEADER_VERTEX_BUFFERS);
		move_if_set(&rebase, linked_address + XBOX_MAP_TAG_HEADER_INDEX_BUFFERS);
		result = walk_headers(&rebase, rebase.vertex_headers[0], rebase.vertex_header_counts[0],
			"the vertex buffer headers at") &&
			walk_headers(&rebase, rebase.index_headers, rebase.index_header_count, "the index buffer headers at");
	}
	for (k = 0; result && k < count; k++)
	{
		unsigned long instance = instances + k * XBOX_MAP_TAG_INSTANCE_SIZE;
		unsigned long name = word_at(&rebase, instance + XBOX_MAP_TAG_INSTANCE_NAME);
		unsigned long address = word_at(&rebase, instance + XBOX_MAP_TAG_INSTANCE_ADDRESS);
		unsigned long layout;

		rebase.what = "tag";
		rebase.tag_index = (long)k;
		rebase.group_tag = word_at(&rebase, instance + XBOX_MAP_TAG_INSTANCE_GROUP);
		rebase.tag_name = 0;
		if (!name_ends_inside(&rebase, name))
		{
			result = fail(&rebase, XBOX_MAP_TAG_INSTANCE_NAME, "a name at", name, ", outside the names");
			break;
		}
		rebase.tag_name = name;
		move_if_set(&rebase, instance + XBOX_MAP_TAG_INSTANCE_NAME);
		if (!address)
			continue;
		if (!group_layout(rebase.group_tag, &layout))
		{
			result = fail(&rebase, XBOX_MAP_TAG_INSTANCE_ADDRESS, "a definition of a group with no layout at",
				address, "");
			break;
		}
		if ((address & 3) || !inside(&rebase, address, xbox_map_layouts[layout].size))
		{
			result = fail(&rebase, XBOX_MAP_TAG_INSTANCE_ADDRESS, "a definition outside the data at", address, "");
			break;
		}
		move_if_set(&rebase, instance + XBOX_MAP_TAG_INSTANCE_ADDRESS);
		result = walk_array(&rebase, layout, address, 1);
	}
	rebase_end(&rebase);
	return result;
}

int xbox_map_rebase_structure_bsp(void *data, unsigned long size, unsigned long linked_address, long delta,
	char *error, unsigned long error_size)
{
	return xbox_map_rebase_structure_bsp_apart(data, size, linked_address, delta, delta, error, error_size);
}

int xbox_map_rebase_structure_bsp_apart(void *data, unsigned long size, unsigned long linked_address, long delta,
	long names_delta, char *error, unsigned long error_size)
{
	struct rebase rebase;
	unsigned long root;
	int result = 1;

	if (!rebase_begin(&rebase, data, size, linked_address, delta, error, error_size, "the structure BSP"))
		return 0;
	rebase.outside_delta = (unsigned long)names_delta;
	if (size < XBOX_MAP_BSP_HEADER_SIZE ||
		word_at(&rebase, linked_address + XBOX_MAP_BSP_HEADER_SIGNATURE) != XBOX_MAP_BSP_HEADER_SIGNATURE_VALUE)
	{
		rebase_end(&rebase);
		return fail_fixed(&rebase, "the BSP header has no signature; its size is", size, "");
	}
	/* the BSP loads at the end of the tag cache, above the tag data, whose
	names its tag references point at */
	rebase.names_end = linked_address;
	rebase.names_begin = linked_address + size >= XBOX_MAP_TAG_CACHE_SIZE ?
		linked_address + size - XBOX_MAP_TAG_CACHE_SIZE : 0;
	root = word_at(&rebase, linked_address + XBOX_MAP_BSP_HEADER_ADDRESS);
	rebase.vertex_headers[0] = word_at(&rebase, linked_address + XBOX_MAP_BSP_HEADER_VERTEX_BUFFERS);
	rebase.vertex_header_counts[0] = word_at(&rebase, linked_address + XBOX_MAP_BSP_HEADER_VERTEX_BUFFER_COUNT);
	rebase.vertex_headers[1] = word_at(&rebase, linked_address + XBOX_MAP_BSP_HEADER_LIGHTMAP_VERTEX_BUFFERS);
	rebase.vertex_header_counts[1] =
		word_at(&rebase, linked_address + XBOX_MAP_BSP_HEADER_LIGHTMAP_VERTEX_BUFFER_COUNT);
	if ((root & 3) || !inside(&rebase, root, xbox_map_layouts[XBOX_MAP_STRUCTURE_BSP_LAYOUT].size))
		result = fail_fixed(&rebase, "the BSP's definition lies outside the data at", root, "");
	if (result)
	{
		move_if_set(&rebase, linked_address + XBOX_MAP_BSP_HEADER_ADDRESS);
		move_if_set(&rebase, linked_address + XBOX_MAP_BSP_HEADER_VERTEX_BUFFERS);
		move_if_set(&rebase, linked_address + XBOX_MAP_BSP_HEADER_LIGHTMAP_VERTEX_BUFFERS);
		result = walk_headers(&rebase, rebase.vertex_headers[0], rebase.vertex_header_counts[0],
			"the vertex buffer headers at") &&
			walk_headers(&rebase, rebase.vertex_headers[1], rebase.vertex_header_counts[1],
				"the lightmap vertex buffer headers at");
	}
	if (result)
	{
		rebase.what = "the structure BSP's definition";
		result = walk_array(&rebase, XBOX_MAP_STRUCTURE_BSP_LAYOUT, root, 1);
	}
	rebase_end(&rebase);
	return result;
}
