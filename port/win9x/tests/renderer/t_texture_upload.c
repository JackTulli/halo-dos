/*
T_TEXTURE_UPLOAD.C

What xbox_textures.c's upload hands to OpenGL for every bitmap of maps
(bitmap_dump.py), as one hash a map: the program is built once with the
file as it is and once with a changed copy (-DXBOX_TEXTURES_C="<path>"),
and the two outputs must be the same text.

    t_texture_upload <dump>...

Each map goes through twice: with compressed textures handed over as they
are, and with the file's own DXT decoding (its HALO_ANDROID path without
S3TC).
*/

#include "../w9x_test.h"

#include "xbox_textures_under_test.h"

static D3DCOLOR palette[256];

static void upload_map(const char *path, BOOL s3tc)
{
	FILE *file = fopen(path, "rb");
	unsigned long bitmaps = 0;
	unsigned char *data;
	long size, position = 0;

	if (!CHECK(file != NULL))
		return;
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fseek(file, 0, SEEK_SET);
	data = malloc((size_t)size + 64);
	size = (long)fread(data, 1, (size_t)size, file);
	fclose(file);
	memset(data + size, 0, 64);

	xgpu_capabilities.s3tc = s3tc;
	gl_image_hash = 1469598103934665603ULL;
	gl_image_count = 0;
	while (position + (long)sizeof(struct dump_header) <= size)
	{
		const struct dump_header *bitmap = (const struct dump_header *)(data + position);
		const unsigned char *pixels = data + position + sizeof(struct dump_header);
		struct xgpu_texture_description description;
		DWORD format_word, size_word;

		if (!CHECK(!memcmp(bitmap->signature, "bmp1", 4)))
			break;
		position += (long)sizeof(struct dump_header) + (long)bitmap->pixels_size;
		if (!bitmap_header(bitmap, &format_word, &size_word))
			continue;
		xgpu_texture_describe(format_word, size_word, &description);
		if (xgpu_texture_face_size(&description) * (description.cube_map ? 5 : 0) +
			xgpu_texture_level_offset(&description, description.levels) > bitmap->pixels_size)
			continue;
		upload(1, description.cube_map ? GL_TEXTURE_CUBE_MAP : description.depth > 1 ? GL_TEXTURE_3D : GL_TEXTURE_2D,
			&description, pixels, palette);
		bitmaps++;
	}
	free(data);
	printf("%s, %s: %lu bitmaps, %lu images, hash %016llx\n", path, s3tc ? "compressed as they are" : "DXT decoded",
		bitmaps, gl_image_count, gl_image_hash);
}

int main(int argc, char **argv)
{
	int index;

	for (index = 0; index < 256; index++)
		palette[index] = (D3DCOLOR)test_random();
	for (index = 1; index < argc; index++)
	{
		upload_map(argv[index], TRUE);
		upload_map(argv[index], FALSE);
	}
	return test_finish("t_texture_upload");
}
