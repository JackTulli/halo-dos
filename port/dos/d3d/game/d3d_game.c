/*
D3D_GAME.C

What the Direct3D build (HALOD3D.EXE, port/dos/DIRECT3D.md) gives Halo's
own renderer that the DOS and Windows 95 builds leave out: the units these
come from (progress_bar.c, ChupathingyCE's d3d8_gl.c) are not compiled in.
*/

#include "cseries.h"
#include "bitmaps/bitmaps_inlines.h"
#include <xtl.h>
#include "rasterizer/xbox/rasterizer_xbox.h"

/* rasterizer_xbox.c's stencil modes set their states through the loading
screen's helper (interface/progress_bar.c): the device's own dispatch */
void SetRenderStateSmart(
	D3DRENDERSTATETYPE state,
	unsigned long value)
{
	D3DDevice_SetRenderState(state, value);
}

void SetTextureStageStateSmart(
	unsigned long stage,
	D3DTEXTURESTAGESTATETYPE type,
	unsigned long value)
{
	D3DDevice_SetTextureStageState(stage, type, value);
}

/* the screen's targets are the Xbox's size (the PC half scales the back
buffer as a whole): the screen effects' convolutions as the Xbox's */
float halo_screen_scale(void)
{
	return 1.0f;
}

/* the shadow maps as the Xbox drew them, 128x128 */
long halo_shadow_map_scale(void)
{
	return 1;
}

/* (no anti-aliasing pass) */
void halo_screen_anti_alias(short x0, short y0, short x1, short y1)
{
	(void)x0;
	(void)y0;
	(void)x1;
	(void)y1;
}

/* the rasterizer's debug drawing (rasterizer_debug.c) is left out; its
initialisation succeeds, or the rasterizer's would fail with it (the rest
of its functions are tools/dos_stubs.py's empty stand-ins) */
boolean rasterizer_debug_initialize(
	void)
{
	return TRUE;
}

/* the editor's say over each object drawn (render_objects.c draws an object
only when this is TRUE): outside the editor, every one is drawn */
struct render_lighting;

boolean editor_preprocess_rendered_object(
	long object_index,
	struct render_lighting *lighting)
{
	(void)object_index;
	(void)lighting;
	return TRUE;
}

/* testing (setting d3d_trace_frame, xd3d_device.c): what the renderer's
visibility found in the traced frame */
#include "render/render.h"
#include "objects/objects.h"
#include "halo_system.h"
#include <stdio.h>

void d3d_trace_visibility(void)
{
	char line[200];
	struct object_iterator iterator;
	struct object_datum *object;
	long count = 0, in_cluster = 0;

	object_iterator_new(&iterator, _object_mask_all, 0);
	while ((object = object_iterator_next(&iterator)) != NULL)
	{
		count++;
		if (object->object.location.cluster_index != NONE)
			in_cluster++;
	}
	snprintf(line, sizeof(line), "visibility: leaf %ld, cluster %ld, sky %ld, rendered clusters %ld; objects %ld, "
		"%ld in a cluster", (long)render.leaf_index, (long)render.cluster_index, (long)render.visible_sky_index,
		(long)render.rendered_cluster_count, count, in_cluster);
	system_log(line);
}

/* the external copies of header inlines some units call through a
prototype (port/dos/game/dos_comdat.c has the DOS build's) */
static void *const d3d_inline_copies[] __attribute__((used)) =
{
	(void *)real_alpha_to_pixel32,
};
