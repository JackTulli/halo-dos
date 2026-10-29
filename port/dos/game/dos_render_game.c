/*
DOS_RENDER_GAME.C

The renderer as the game sees it in the DOS and Windows 95 builds (render/
and rasterizer/ are left out, as on the N64: port/n64/game/
n64_render_game.c is this file's model): the globals other subsystems read,
with the Xbox build's defaults, the lifecycle calls, and the frame entry
points main.c calls (render_frame, render_frame_present).

A frame: the picture cleared to the sky's colour; for each view, the
level's clusters the eye can see (dos_level.c), the objects in them (Halo's
render_model through dos_models.c, lit by Halo's lighting from the
lightmap), then the first-person weapon over a cleared depth buffer, then
the HUD (dos_hud_game.c). The game lays its views and HUD out on a 640x480
screen; the picture's pixels are that scaled.
*/

#include "cseries.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game/game.h"
#include "game/players.h"
#include "game/local_players.h"
#include "interface/first_person_weapons.h"
#include "interface/interface.h"
#include "interface/ui_widget.h"
#include "main/main.h"
#include "models/models.h"
#include "models/model_definitions.h"
#include "rasterizer/rasterizer.h"
#include "rasterizer/rasterizer_console_vars.h"
#include "render/render.h"
#include "objects/objects.h"
#include "objects/object_definitions.h"
#include "objects/object_lights_rendering.h"
#include "scenario/scenario.h"
#include "scenario/sky_definitions.h"
#include "structures/structures.h"
#include "structures/structure_bsp_definitions.h"
#include "tag_files/tag_groups.h"

#include "halo_system.h"
#include "dos_render.h"

const char *dos_setting(const char *name, const char *default_value);
long dos_setting_integer(const char *name, long default_value);
float dos_setting_real(const char *name, float default_value);
void dos_log_scripts(void);
void dos_menu_frame(void);
void dos_p2p_poll(void);
void dos_models_set_change_color(unsigned long color);
void dos_hud_frame_begin(void);
void dos_hud_dispose_from_old_map(void);
void dos_art_new_map(void);
void r_clear_depth(int x0, int y0, int x1, int y1);

/* ---------- globals (rasterizer.c, render.c) */

struct render_globals render;

struct rasterizer_globals_definition rasterizer_globals =
{
	.push_buffer_size = 768,
	/* framerate_throttle: the frame waits for nothing */
	.framerate_throttle = FALSE,
	.near_clip_distance = 0.0625f,
	.far_clip_distance = 1024.f,
	.first_person_weapon_near_clip_distance = 0.01171875f,
	.first_person_weapon_far_clip_distance = 1024.f,
};

struct rasterizer_debug_options rasterizer_debug_options =
{
	.debug_model_lod = NONE,
	.draw_models = TRUE,
	.draw_model_transparent_geometry = TRUE,
	.draw_first_person_weapon_first = TRUE,
	.stencil_mask_enabled = TRUE,
	.draw_environment = 2,
	.draw_environment_lightmaps = TRUE,
	.draw_environment_shadows = TRUE,
	.draw_environment_diffuse_lights = TRUE,
	.draw_environment_textures = TRUE,
	.draw_environment_decals = TRUE,
	.draw_environment_specular_lights = TRUE,
	.draw_environment_specular_lightmaps = TRUE,
	.draw_environment_reflection_lightmap_masks = TRUE,
	.draw_environment_reflection_mirrors = TRUE,
	.draw_environment_reflections = TRUE,
	.draw_environment_transparent_geometry = TRUE,
	.draw_environment_fog = TRUE,
	.draw_environment_fog_screen = TRUE,
	.draw_water = TRUE,
	.draw_lens_flares = TRUE,
	.draw_dynamic_unlit_geometry = TRUE,
	.draw_dynamic_lit_geometry = TRUE,
	.draw_dynamic_screen_geometry = TRUE,
	.draw_hud_motion_sensor = TRUE,
	.draw_detail_objects = TRUE,
	.draw_debug_geometry = TRUE,
	.fog_atmospheric_enabled = TRUE,
	.fog_planar_enabled = TRUE,
	.bump_mapping_enabled = TRUE,
	.lightmap_ambient = 1.0f,
	.lightmap_incident_radiosity_enabled = TRUE,
	.lightmap_filtering_enabled = TRUE,
	.environment_alpha_testing_enabled = TRUE,
	.environment_specular_mask_enabled = TRUE,
	.shadow_convolution_enabled = TRUE,
	.active_camouflage_enabled = TRUE,
	.active_camouflage_multipass_enabled = TRUE,
	.plasma_energy_enabled = TRUE,
	.lens_flare_occlusion_enabled = TRUE,
	.lens_flare_sun_glow_enabled = TRUE,
	.screen_flash_enabled = TRUE,
	.screen_effects_enabled = TRUE,
	.detail_object_screen_facing_offset_multiplier = 0.4f,
	.zbias = 8,
	.zoffset = 1.0f / 256.0f,
	.zsprite_enabled = TRUE,
	.filthy_decal_fog_hack_enabled = TRUE,
	.smart_states_enabled = TRUE,
	.pad3_scale = 1.0f,
};

real_argb_color *global_rasterizer_model_ambient_reflection_tint;
boolean rasterizer_model_cortana_hack;

struct r_settings dos_render_settings;
float dos_screen_scale_x = 0.5f, dos_screen_scale_y = 0.5f;
long dos_screen_width = 640;
float dos_screen_offset_x;
/* DOUBLE WIDE, DOUBLE TALL in force (rasterizer_initialize) */
static int doubled_wide, doubled_tall;

float dos_screen_centred_offset(void)
{
	return (float)(dos_screen_width - 640) * 0.5f * dos_screen_scale_x;
}
/* a pixel's width over its height on the display (the views' projection) */
static float dos_pixel_aspect = 1.0f;
/* under how many pixels across an object is a dot (render_objects_for_view) */
static float object_dot_pixels;

static struct system_display display;
static void (*vertical_blank_callback)(unsigned long context);
static long frame_number;
static boolean frame_begun;

/* where a frame's time goes (the log's line every 5 seconds) */
enum
{
	_time_game, _time_level, _time_objects, _time_first_person, _time_hud, _time_present,
	NUMBER_OF_FRAME_TIMES
};
static unsigned long long frame_times[NUMBER_OF_FRAME_TIMES];
static unsigned long long time_mark;
/* the triangles each phase drew, the last frame */
static long phase_triangles[NUMBER_OF_FRAME_TIMES], triangle_mark;

static void time_phase(int phase)
{
	unsigned long long now = system_microseconds();

	if (time_mark)
		frame_times[phase] += now - time_mark;
	time_mark = now;
	{
		long drawn = r_frame_statistics()->triangles_drawn;

		/* (a phase may come twice a frame: the level's, then its water) */
		phase_triangles[phase] += drawn - triangle_mark;
		triangle_mark = drawn;
	}
}
static float draw_distance = 90.0f;
/* the sky_model setting (-1: not read yet) */
static int sky_models_setting = -1;

long dos_frame_number(void)
{
	return frame_number;
}

/* the game's time in seconds, between its ticks too (Halo's frame's game
time: the water's ripples move by it) */
float dos_game_seconds(void)
{
	return (float)game_time_get() / 30.0f + render.time_delta_since_tick_sec;
}

/* models' sizes on the screen as their level of detail takes them
(models.c render_model, HALO_DOS): the DETAIL setting's LOWEST and LOW
take coarser models, NORMAL and HIGH the game's own */
/* under what size on the screen (the game's level-of-detail pixels) a
model's coarse copies are drawn (dos_models.c): HIGH never */
real dos_coarse_model_pixels(void)
{
	static const real sizes[4] = { 96.0f, 64.0f, 40.0f, 0.0f };

	return sizes[PIN(dos_render_settings.detail, 0, 3)];
}

real dos_model_detail_scale(void)
{
	static const real scales[4] = { 0.35f, 0.6f, 1.0f, 1.0f };

	return scales[PIN(dos_render_settings.detail, 0, 3)];
}

/* ---------- lifecycle */

/* the settings the menu changes as the game runs (its SETTINGS page) */
void dos_render_reload_settings(void)
{
	dos_render_settings.detail = (int)PIN(dos_setting_integer("detail", 2), 0, 3);
	dos_render_settings.texture_dither = (int)dos_setting_integer("texture_dither", dos_render_settings.bits != 8);
	dos_render_settings.texture_smooth = (int)dos_setting_integer("texture_smooth", 0);
	/* (the 8-bit picture's light dithered across large triangles: at HIGH
	detail, unless light_dither says; 4 to 6 ms a frame at 100 MHz) */
	dos_render_settings.light_dither = (int)dos_setting_integer("light_dither", dos_render_settings.detail >= 3);
	r_settings_changed(&dos_render_settings);
	draw_distance = (float)dos_setting_integer("draw_distance", 90);
	if (draw_distance < 10.0f)
		draw_distance = 10.0f;
	sky_models_setting = (int)dos_setting_integer("sky_model", 1);
	system_display_settings_changed();
}

boolean rasterizer_initialize(
	void)
{
	char line[128];
	int display_bits;

	dos_render_settings.width = (int)dos_setting_integer("width", 320);
	dos_render_settings.height = (int)dos_setting_integer("height", 240);
	display_bits = (int)dos_setting_integer("bits", 16);
	dos_render_settings.bits = display_bits == 8 ? 8 : 32;
	dos_render_settings.texture_size = (int)dos_setting_integer("texture_size", 128);
	dos_render_settings.detail = (int)PIN(dos_setting_integer("detail", 2), 0, 3);
	dos_render_settings.affine_tolerance = dos_setting_real("affine_tolerance", -1.0f);
	/* (the 32-bit picture's: 8-bit spans do not dither or filter) */
	dos_render_settings.texture_dither = (int)dos_setting_integer("texture_dither", dos_render_settings.bits != 8);
	dos_render_settings.texture_smooth = (int)dos_setting_integer("texture_smooth", 0);
	/* (the 8-bit picture's light dithered across large triangles: at HIGH
	detail, unless light_dither says; 4 to 6 ms a frame at 100 MHz) */
	dos_render_settings.light_dither = (int)dos_setting_integer("light_dither", dos_render_settings.detail >= 3);
	/* (the level's lightmaps as its vertices' colours, read at load, as the
	N64 port's: on unless level_light = 0; the older "lighting" setting of
	the files written before is not read) */
	dos_lighting = (int)dos_setting_integer("level_light", 1) != 0;
	object_dot_pixels = dos_setting_real("object_dot_pixels", 8.0f);
	draw_distance = (float)dos_setting_integer("draw_distance", 90);
	if (draw_distance < 10.0f)
		draw_distance = 10.0f;
	if (!system_display_open(dos_render_settings.width, dos_render_settings.height, display_bits, &display))
	{
		system_error("No display mode works for the picture's size and colours (HALO.CFG: width, height, bits).");
		return FALSE;
	}
	{
		/* DOUBLE WIDE and DOUBLE TALL: the picture half as wide or as tall as
		the display (320x240 drawn as 160x240, or 160x120 with both), each
		pixel shown twice that way -- for slow machines; the software
		renderer's alone (a graphics card draws at its own resolution) */
		int wide = dos_setting_integer("double_wide", 0) != 0, tall = dos_setting_integer("double_tall", 0) != 0;

		doubled_wide = doubled_tall = FALSE;
		if ((wide || tall) && system_display_doubling(wide, tall))
		{
			doubled_wide = wide;
			doubled_tall = tall;
			dos_render_settings.width >>= wide;
			dos_render_settings.height >>= tall;
		}
	}
	/* (the display's kind: OpenGL and a Voodoo draw millions of colours,
	asked for 256; the textures' dither, filtering there, follows it) */
	if (dos_render_settings.bits == 8 && display.format != _system_pixels_8)
	{
		dos_render_settings.bits = 32;
		dos_render_settings.texture_dither = (int)dos_setting_integer("texture_dither", 1);
	}
	if (!r_initialize(&dos_render_settings))
	{
		system_error("There is not enough memory for the picture.");
		return FALSE;
	}
	if (display.format == _system_pixels_8)
		system_display_palette(r_palette());
	dos_pixel_aspect = display.pixel_aspect > 0.0f ? display.pixel_aspect : 1.0f;
	/* (a doubled pixel twice as wide, or half as wide for its height) */
	dos_pixel_aspect *= (doubled_wide ? 2.0f : 1.0f) / (doubled_tall ? 2.0f : 1.0f);
	{
		/* the display's shape (the setting aspect: 0 the picture's own, else
		its width over its height times 100, for a monitor that stretches
		the picture: a DOS mode on a wide screen), and the logical screen's
		width from it: 480 lines and as many columns as that shape has, 640
		at least (narrower shapes are stretched as before), even */
		long setting = dos_setting_integer("aspect", 0);
		float aspect = (float)dos_render_settings.width * dos_pixel_aspect / (float)dos_render_settings.height;
		long wanted;

		if (setting >= 100 && setting <= 400)
		{
			aspect = (float)setting / 100.0f;
			dos_pixel_aspect = aspect * (float)dos_render_settings.height / (float)dos_render_settings.width;
		}
		wanted = (long)(480.0f * aspect + 0.5f) & ~1L;
		dos_screen_width = wanted < 640 ? 640 : wanted > 1600 ? 1600 : wanted;
	}
	snprintf(line, sizeof(line), "render: %dx%d at %d bits, shown in %dx%d (%s)", dos_render_settings.width,
		dos_render_settings.height, dos_render_settings.bits, display.width, display.height, display.description);
	system_log(line);

	/* the logical screen the game lays its views and HUD out on */
	rasterizer_globals.reserved04.screen_bounds.x0 = 0;
	rasterizer_globals.reserved04.screen_bounds.y0 = 0;
	rasterizer_globals.reserved04.screen_bounds.x1 = (short)dos_screen_width;
	rasterizer_globals.reserved04.screen_bounds.y1 = 480;
	/* (the original's title-safe frame, 7.5% in: where the HUD's corners
	are; on a wide screen the same 48 in from its sides, as the PC ports'
	frame) */
	rasterizer_globals.reserved04.frame_bounds.x0 = 48;
	rasterizer_globals.reserved04.frame_bounds.y0 = 36;
	rasterizer_globals.reserved04.frame_bounds.x1 = (short)(dos_screen_width - 48);
	rasterizer_globals.reserved04.frame_bounds.y1 = 444;
	dos_screen_scale_x = (float)dos_render_settings.width / (float)dos_screen_width;
	dos_screen_scale_y = (float)dos_render_settings.height / 480.0f;
	dos_screen_offset_x = 0.0f;
	snprintf(line, sizeof(line), "render: the logical screen %ldx480 (%s)", dos_screen_width,
		dos_screen_width > 640 ? "wide: the views wider, the HUD at its sides, the menus centred" : "4:3");
	system_log(line);
	rasterizer_globals.initialized = TRUE;
	return TRUE;
}

void rasterizer_dispose(void)
{
	r_dispose();
	system_display_close();
}

void rasterizer_preinitialize__fill_you_up_with_the_devils_cock(void) { return; }

void rasterizer_set_vblank_callback(
	void (*callback)(unsigned long context))
{
	vertical_blank_callback = callback;
}

void render_initialize(void) { return; }
void render_dispose(void) { return; }

/* the objects' lights kept (object_get_cached_render_lighting) emptied
with each map: a new map's objects take the same indices as the old one's
first ones, and would have stepped from the old map's light */
static int object_lights_cleared;

void render_dispose_from_old_map(
	void)
{
	dos_level_unload();
	dos_models_dispose_from_old_map();
	dos_hud_dispose_from_old_map();
	dos_textures_dispose_from_old_map();
	object_lights_cleared = FALSE;
}

void render_initialize_for_new_map(
	void)
{
	dos_textures_initialize_for_new_map();
	dos_level_load();
	/* (the menu's art looked for again: this map's tags) */
	dos_art_new_map();
}

/* ---------- objects' lighting (render_objects.c's cache): none, as the
N64 port's models take a white ambient. Every object, the first-person
weapon among them, is its textures' own colour wherever it is: no light is
worked out while playing, and nothing darkens in the shade. The level's
light is its vertices' colours, read from its lightmaps at load
(dos_level.c, level_light). */

int dos_lighting;

/* On Direct3D 8 (the Xbox's look, the setting d3d_object_light) every
object is lit as Halo lights it (object_lights.c,
lights_prepare_for_object_static: the lightmap under its middle and four
corners, its ambient and two distant lights), kept for it and worked out
again as it moves (render_objects.c's cache, which is not in these
builds), a few objects a frame at most; dos_models.c then lights each
vertex by it. */

#define OBJECT_LIGHT_SLOTS 512
/* objects whose light is worked out again in one frame (a new object's
always is) */
#define OBJECT_LIGHT_REFRESHES 16
/* how far an object moves (world units, squared) before its light is
worked out again */
#define OBJECT_LIGHT_MOVE 0.04f
/* how far the light it is drawn in moves toward its new light in a tick
(render_objects.c's OBJECT_LIGHTING_MAXIMUM_COLOR_DELTA: walking into the
shade darkens over a second, not at once) */
#define OBJECT_LIGHT_STEP 0.03f

int dos_object_light(void)
{
	return r_draws_lightmaps() && dos_lighting && dos_setting_integer("d3d_object_light", 1);
}

static struct
{
	long object_index;
	real_point3d position;
	long step_tick;
	/* the light it is drawn in, and the one last worked out */
	struct render_lighting lighting;
	struct render_lighting desired;
} object_lights[OBJECT_LIGHT_SLOTS];

static void step_values(real *values, real const *desired, int count)
{
	for (int i = 0; i < count; i++)
		values[i] += PIN(desired[i] - values[i], -OBJECT_LIGHT_STEP, OBJECT_LIGHT_STEP);
}

/* a tick's step of the drawn light toward the desired */
static void step_lighting(struct render_lighting *lighting, struct render_lighting const *desired)
{
	step_values(&lighting->ambient_color.red, &desired->ambient_color.red, 3);
	lighting->distant_light_count = desired->distant_light_count;
	for (int i = 0; i < desired->distant_light_count && i < 2; i++)
	{
		real_vector3d *direction = &lighting->distant_lights[i].direction;

		step_values(&lighting->distant_lights[i].color.red, &desired->distant_lights[i].color.red, 3);
		step_values(&direction->i, &desired->distant_lights[i].direction.i, 3);
		if (direction->i != 0.0f || direction->j != 0.0f || direction->k != 0.0f)
			normalize3d(direction);
	}
}

struct render_lighting *object_get_cached_render_lighting(
	long object_index,
	real level_of_detail_pixels)
{
	/* (0x80 a channel: the texture as it is) */
	static struct render_lighting unlit = { { 128.0f / 255.0f, 128.0f / 255.0f, 128.0f / 255.0f } };
	static long refresh_frame = -1;
	static int refreshes;

	(void)level_of_detail_pixels;
	if (dos_object_light() && object_index != NONE)
	{
		if (!object_lights_cleared)
		{
			for (int i = 0; i < OBJECT_LIGHT_SLOTS; i++)
				object_lights[i].object_index = NONE;
			object_lights_cleared = TRUE;
		}
		struct object_datum *object = object_get(object_index);
		long slot_index = DATUM_INDEX_TO_ABSOLUTE_INDEX(object_index) % OBJECT_LIGHT_SLOTS;
		real_point3d const *center = &object->object.bounding_sphere_center;
		real dx, dy, dz;

		if (refresh_frame != dos_frame_number())
		{
			refresh_frame = dos_frame_number();
			refreshes = 0;
		}
		dx = center->x - object_lights[slot_index].position.x;
		dy = center->y - object_lights[slot_index].position.y;
		dz = center->z - object_lights[slot_index].position.z;
		if (object_lights[slot_index].object_index != object_index ||
			(dx * dx + dy * dy + dz * dz > OBJECT_LIGHT_MOVE && refreshes < OBJECT_LIGHT_REFRESHES))
		{
			lights_prepare_for_object_static(object_index, &object_lights[slot_index].desired);
			/* (a new object in its light at once) */
			if (object_lights[slot_index].object_index != object_index)
			{
				object_lights[slot_index].lighting = object_lights[slot_index].desired;
				object_lights[slot_index].step_tick = game_time_get();
			}
			object_lights[slot_index].object_index = object_index;
			object_lights[slot_index].position = *center;
			refreshes++;
			/* (testing, setting debug_object_light=1: each light worked out) */
			if (dos_setting_integer("debug_object_light", 0))
			{
				const struct render_lighting *l = &object_lights[slot_index].desired;
				char line[300];

				snprintf(line, sizeof(line), "object light: %s at %.1f %.1f %.1f: ambient %.2f %.2f %.2f, %d distant: "
					"%.2f %.2f %.2f from %.2f %.2f %.2f, %.2f %.2f %.2f from %.2f %.2f %.2f",
					tag_get_name(object->definition_index), (double)center->x, (double)center->y, (double)center->z,
					(double)l->ambient_color.red, (double)l->ambient_color.green, (double)l->ambient_color.blue,
					l->distant_light_count, (double)l->distant_lights[0].color.red,
					(double)l->distant_lights[0].color.green, (double)l->distant_lights[0].color.blue,
					(double)l->distant_lights[0].direction.i, (double)l->distant_lights[0].direction.j,
					(double)l->distant_lights[0].direction.k, (double)l->distant_lights[1].color.red,
					(double)l->distant_lights[1].color.green, (double)l->distant_lights[1].color.blue,
					(double)l->distant_lights[1].direction.i, (double)l->distant_lights[1].direction.j,
					(double)l->distant_lights[1].direction.k);
				system_log(line);
			}
		}
		/* (a step a tick, at most a second's worth behind) */
		for (int step = 0; step < 30 && object_lights[slot_index].step_tick != game_time_get(); step++)
		{
			step_lighting(&object_lights[slot_index].lighting, &object_lights[slot_index].desired);
			object_lights[slot_index].step_tick++;
		}
		object_lights[slot_index].step_tick = game_time_get();
		return &object_lights[slot_index].lighting;
	}
	return &unlit;
}

/* ---------- objects in a view */

static unsigned long const *view_clusters;
static short view_cluster_count;

static boolean object_reaches_visible_cluster(
	long object_index)
{
	struct object_cluster_iterator iterator;
	short cluster;

	for (cluster = object_get_first_cluster(&iterator, object_index); cluster != NONE;
		cluster = object_get_next_cluster(&iterator, object_index))
	{
		if (cluster < 0 || cluster >= view_cluster_count ||
			(view_clusters[cluster >> 5] & (1UL << (cluster & 31))))
			return TRUE;
	}
	return FALSE;
}

static unsigned long color_of(real_rgb_color const *color)
{
	unsigned long r = (unsigned long)PIN(color->red * 255.f, 0.f, 255.f);
	unsigned long g = (unsigned long)PIN(color->green * 255.f, 0.f, 255.f);
	unsigned long b = (unsigned long)PIN(color->blue * 255.f, 0.f, 255.f);

	return (r << 16) | (g << 8) | b;
}

/* Objects only a few pixels across (Halo's models keep hundreds of
triangles at their coarsest: a rocket launcher 50 units off, 4 pixels, was
788) are drawn as a dot: a square facing the eye, half the object's
bounding sphere, of its first shader's texture's mean colour (kept by
definition). The setting object_dot_pixels: under how many pixels (0 none). */

#define OBJECT_DOT_COLORS 1024

static float object_dot_pixels = 8.0f;
static struct
{
	long definition_index;
	unsigned long color;
} object_dot_colors[OBJECT_DOT_COLORS];

static unsigned long object_dot_color(long definition_index, long model_index)
{
	long slot = DATUM_INDEX_TO_ABSOLUTE_INDEX(definition_index) & (OBJECT_DOT_COLORS - 1);

	if (object_dot_colors[slot].definition_index != definition_index)
	{
		struct model *model = model_definition_get(model_index);
		unsigned long color = 0x808080;

		if (model && model->shaders.count > 0)
		{
			/* (models.c's model_shader_reference: the shader's reference first) */
			const struct tag_reference *shader = (const struct tag_reference *)xbox_pointer(model->shaders.address);
			const struct dos_material *material = dos_shader_material(shader->index);

			if (!material->skip && material->texture)
				color = r_texture_average(material->texture) & 0xFFFFFF;
		}
		object_dot_colors[slot].definition_index = definition_index;
		object_dot_colors[slot].color = color;
	}
	return object_dot_colors[slot].color;
}

/* testing (setting camera = "x y z yaw pitch", degrees, spaces or commas
between): every view from
there, whatever the game's camera does, so timings of different builds see
the same picture (the timing lines print the game's camera to pick one) */
static struct render_camera last_view_camera;

/* testing (settings debug_camera=<frames>, debug_camera_from=<frame>): each
frame's game tick, its fraction between ticks, the camera, and the first
objects' positions as drawn (their node 0) -- how smoothly things move */
static void debug_camera_log(const struct render_camera *camera)
{
	extern real render_interpolation_fraction(void);
	static long frames = -1, from;
	struct object_iterator iterator;
	struct object_datum *object;
	char line[400];
	int at, count = 0;

	if (frames < 0)
	{
		frames = dos_setting_integer("debug_camera", 0);
		from = dos_setting_integer("debug_camera_from", 0);
	}
	if (!frames || frame_number < from || frame_number >= from + frames)
		return;
	at = snprintf(line, sizeof(line), "camera %ld: tick %ld + %.3f, at %.4f %.4f %.4f, forward %.4f %.4f %.4f",
		frame_number, (long)game_time_get(), (double)render_interpolation_fraction(), (double)camera->position.x,
		(double)camera->position.y, (double)camera->position.z, (double)camera->forward.i, (double)camera->forward.j,
		(double)camera->forward.k);
	object_iterator_new(&iterator, _object_mask_all, 0);
	while ((object = object_iterator_next(&iterator)) != NULL && count < 3 && at < (int)sizeof(line) - 80)
	{
		const real_matrix4x3 *nodes = object_get_node_matrices(iterator.index);

		at += snprintf(line + at, sizeof(line) - at, "; %08lX %.4f %.4f %.4f", (unsigned long)iterator.index,
			(double)nodes[0].position.x, (double)nodes[0].position.y, (double)nodes[0].position.z);
		count++;
	}
	system_log(line);
}

static int fixed_camera(struct render_camera *camera)
{
	static int parsed, set;
	static real values[5];

	if (!parsed)
	{
		const char *text = dos_setting("camera", "");
		char *end;
		int count = 0;

		parsed = TRUE;
		/* (spaces or commas between) */
		while (count < 5)
		{
			while (*text == ',' || *text == ' ')
				text++;
			values[count] = (real)strtod(text, &end);
			if (end == text)
				break;
			text = end;
			count++;
		}
		set = count == 5;
	}
	if (set)
	{
		real yaw = values[3] * (real)(3.14159265358979 / 180.0), pitch = values[4] * (real)(3.14159265358979 / 180.0);

		camera->position.x = values[0];
		camera->position.y = values[1];
		camera->position.z = values[2];
		camera->forward.i = cosf(pitch) * cosf(yaw);
		camera->forward.j = cosf(pitch) * sinf(yaw);
		camera->forward.k = sinf(pitch);
		camera->up.i = -sinf(pitch) * cosf(yaw);
		camera->up.j = -sinf(pitch) * sinf(yaw);
		camera->up.k = cosf(pitch);
	}
	return set;
}

/* the frame each object was last drawn in (by its absolute index): the
bots' and Covenant's level of detail asks (dos_object_recently_seen) */
#define SEEN_OBJECT_COUNT 2048
static long object_seen_frame[SEEN_OBJECT_COUNT];

boolean dos_object_recently_seen(long object_index)
{
	long absolute = DATUM_INDEX_TO_ABSOLUTE_INDEX(object_index);

	return object_index != NONE && absolute >= 0 && absolute < SEEN_OBJECT_COUNT &&
		object_seen_frame[absolute] && frame_number - object_seen_frame[absolute] < 15;
}

static void mark_seen(long object_index)
{
	long absolute = DATUM_INDEX_TO_ABSOLUTE_INDEX(object_index);

	if (absolute >= 0 && absolute < SEEN_OBJECT_COUNT)
		object_seen_frame[absolute] = frame_number;
}

/* ---------- the sky: the model of the eye's cluster's sky (Halo's sky
tag), drawn about the eye before the world, as Halo draws it (setting
sky_model: 1 on, 0 the gradient alone; the menu's ui.map always has it) */

static short sky_index_now = NONE;

static long sky_model_at(short cluster_index)
{
	struct structure_bsp *bsp = global_structure_bsp_get();
	struct sky *sky;

	if (bsp && cluster_index >= 0 && cluster_index < bsp->clusters.count)
		sky_index_now = TAG_BLOCK_GET_ELEMENT(&bsp->clusters, cluster_index, struct structure_cluster)->sky_index;
	if (sky_index_now == NONE || !global_scenario_get() || !(sky = scenario_get_sky(sky_index_now)))
		return NONE;
	/* (the sky's model: its first field) */
	return ((struct tag_reference const *)sky)->index;
}

/* a sky model's colour: the mean of its shaders' textures (kept for the
last model asked) */
static unsigned long sky_model_color(long model_index)
{
	static long last_model = NONE;
	static unsigned long last_color;
	struct model *model;
	unsigned long sum[3] = { 0, 0, 0 }, count = 0;

	if (model_index == last_model)
		return last_color;
	model = model_definition_get(model_index);
	for (long i = 0; model && i < model->shaders.count; i++)
	{
		/* (models.c's model_shader_reference: 0x20 bytes, the shader's
		reference first) */
		const struct tag_reference *shader = (const struct tag_reference *)((const byte *)xbox_pointer(model->shaders.address) +
			i * 0x20);
		const struct dos_material *material = dos_shader_material(shader->index);

		/* (not the layers added or blended over the backdrop: a space sky's
		stars and planets over black) */
		if (!material->skip && material->texture && !material->transparent)
		{
			unsigned long c = r_texture_average(material->texture);

			sum[0] += (c >> 16) & 0xFF;
			sum[1] += (c >> 8) & 0xFF;
			sum[2] += c & 0xFF;
			count++;
		}
	}
	last_model = model_index;
	last_color = count ? ((sum[0] / count) << 16) | ((sum[1] / count) << 8) | (sum[2] / count) : 0;
	return last_color;
}

/* a viewer-centred cube map: a cube about the eye, each face its face of
the map (Direct3D's face order and coordinates: the direction's major axis
picks the face) */
static void draw_skybox(r_texture *const faces[6])
{
	/* each face's major axis and sign, and its (s, t) from the direction */
	static const struct
	{
		int axis;
		float sign;
		int s_axis, t_axis;
		float s_sign, t_sign;
	} cube_faces[6] =
	{
		{ 0, 1.0f, 2, 1, -1.0f, -1.0f },
		{ 0, -1.0f, 2, 1, 1.0f, -1.0f },
		{ 1, 1.0f, 0, 2, 1.0f, 1.0f },
		{ 1, -1.0f, 0, 2, 1.0f, -1.0f },
		{ 2, 1.0f, 0, 1, 1.0f, -1.0f },
		{ 2, -1.0f, 0, 1, -1.0f, -1.0f },
	};
	static const unsigned short indices[6] = { 0, 1, 2, 0, 2, 3 };
	static const float corners[4][2] = { { -1.0f, -1.0f }, { 1.0f, -1.0f }, { 1.0f, 1.0f }, { -1.0f, 1.0f } };

	for (int f = 0; f < 6; f++)
	{
		struct r_vertex vertices[4];

		if (!faces[f])
			continue;
		for (int c = 0; c < 4; c++)
		{
			float d[3];
			int a = cube_faces[f].axis, u = (a + 1) % 3, v = (a + 2) % 3;

			d[a] = cube_faces[f].sign;
			d[u] = corners[c][0];
			d[v] = corners[c][1];
			vertices[c].position[0] = d[0] * 10.0f;
			vertices[c].position[1] = d[1] * 10.0f;
			vertices[c].position[2] = d[2] * 10.0f;
			vertices[c].u = (cube_faces[f].s_sign * d[cube_faces[f].s_axis] + 1.0f) * 0.5f;
			vertices[c].v = (cube_faces[f].t_sign * d[cube_faces[f].t_axis] + 1.0f) * 0.5f;
			vertices[c].color = 0x808080;
		}
		r_draw_triangles(vertices, 4, indices, 2, NULL, faces[f], R_DRAW_TWO_SIDED | R_DRAW_NO_FOG);
	}
}

static void draw_sky_model(long model_index, const struct r_view *world_view)
{
	static real_matrix4x3 nodes[64];
	static char permutations[32];
	static real_rgb_color change_colors[4];
	static real function_values[4];
	static struct render_lighting unlit = { { 128.0f / 255.0f, 128.0f / 255.0f, 128.0f / 255.0f } };
	static const unsigned long black[R_SKY_STOPS] = { 0, 0, 0, 0, 0 };
	struct r_view sky = *world_view;
	real_point3d origin = { 0.0f, 0.0f, 0.0f };

	if (!nodes[0].scale)
	{
		for (int i = 0; i < NUMBEROF(nodes); i++)
		{
			nodes[i].scale = 1.0f;
			nodes[i].forward.i = 1.0f;
			nodes[i].left.j = 1.0f;
			nodes[i].up.k = 1.0f;
		}
	}
	/* (about the eye: the sky's own space, no fog, as far as it reaches) */
	sky.position[0] = sky.position[1] = sky.position[2] = 0.0f;
	/* (its layers are added over black, as Halo clears the frame first) */
	sky.sky_colors = black;
	sky.z_near = 0.05f;
	sky.z_far = 100000.0f;
	sky.fog_start = sky.fog_end = 0.0f;
	r_view_begin(&sky);
	{
		/* (a backdrop cube map of its shaders' first: under the rest) */
		struct model *model = model_definition_get(model_index);

		for (long s = 0; model && s < model->shaders.count; s++)
		{
			long shader = ((struct tag_reference const *)((byte const *)xbox_pointer(model->shaders.address) + s * 32))->index;
			const struct dos_material *material = dos_shader_material(shader);

			if (material && material->cube[0])
			{
				draw_skybox(material->cube);
				break;
			}
		}
	}
	render_model(model_index, 100000.0f, nodes, permutations, change_colors, function_values, &unlit, &origin,
		10000.0f, NULL, NONE, NONE, 0);
	r_view_end();
	r_clear_depth(world_view->x0, world_view->y0, world_view->x1, world_view->y1);
}

static void render_objects_for_view(
	short local_player_index)
{
	struct object_iterator iterator;
	struct object_datum *object;
	long viewer_unit = NONE;
	long player_index = local_player_get_player_index(local_player_index);
	/* (Halo's levels of detail by the object's size in the picture's own
	pixels: a 320-wide picture shows half the detail of the Xbox's 640, and
	takes a coarser level; the detail setting: fewer pixels choose a coarser
	one still) */
	static const real detail_scale[4] = { 0.25f, 0.5f, 1.0f, 2.0f };
	real scale = detail_scale[dos_render_settings.detail];

	if (player_index != NONE)
		viewer_unit = player_get(player_index)->unit_index;
	object_iterator_new(&iterator, _object_mask_all, FLAG(_object_header_visible_bit));
	while ((object = object_iterator_next(&iterator)) != NULL)
	{
		struct object_definition *definition;
		short cluster = object->object.location.cluster_index;
		float center[3];
		real pixels;
		long owner_index;
		struct object_datum *owner;

		if (TEST_FLAG(object->object.flags, _object_invisible_bit))
			continue;
		definition = object_definition_get(object->definition_index);
		if (definition->object.model.index == NONE)
			continue;
		/* in a cluster the eye cannot see, and reaching into none it can */
		if (view_clusters && cluster >= 0 && cluster < view_cluster_count &&
			!(view_clusters[cluster >> 5] & (1UL << (cluster & 31))) &&
			!object_reaches_visible_cluster(iterator.index))
			continue;
		owner_index = object_get_ultimate_parent(iterator.index);
		if (viewer_unit != NONE && owner_index == viewer_unit)
			continue;
		center[0] = object->object.bounding_sphere_center.x;
		center[1] = object->object.bounding_sphere_center.y;
		center[2] = object->object.bounding_sphere_center.z;
		if (!r_sphere_visible(center, object->object.bounding_sphere_radius))
			continue;
		pixels = 2.0f * object->object.bounding_sphere_radius * r_pixels_per_unit_at(r_distance(center)) * scale;
		mark_seen(iterator.index);
		owner = owner_index == iterator.index ? object : object_get(owner_index);
		dos_models_set_change_color(TEST_FLAG(_object_mask_unit, owner->object.type) ?
			color_of(&owner->object.base_change_colors[0]) : 0);
		if (pixels < object_dot_pixels)
		{
			/* (a few pixels across: a dot of its colour) */
			r_draw_billboard(center, object->object.bounding_sphere_radius * 0.5f,
				object_dot_color(object->definition_index, definition->object.model.index));
			continue;
		}
		render_model(
			definition->object.model.index,
			pixels,
			object_get_node_matrices(iterator.index),
			object->object.region_permutations,
			object->object.outgoing_change_colors,
			object->object.outgoing_function_values,
			object_get_cached_render_lighting(iterator.index, pixels),
			&object->object.bounding_sphere_center,
			object->object.bounding_sphere_radius,
			NULL,
			iterator.index,
			object->object.forced_shader_permutation_index,
			0);
	}
	dos_models_set_change_color(0);
}

/* ---------- frames */

/* the loading screen's (below) */
static struct
{
	int active;
	/* the screen at the start, 0xAARRGGBB at the picture's size */
	unsigned long *picture;
	int width, height;
	long permille;
	unsigned long long started, last_shown;
} loading;

static void loading_end(void);

static void frame_begin(
	void)
{
	if (!frame_begun)
	{
		/* (a map's first frame: its loading screen gone) */
		if (loading.active)
			loading_end();
		r_frame_begin(dos_level_sky_color());
		dos_hud_frame_begin();
		frame_begun = TRUE;
		/* (the frame's statistics start again) */
		triangle_mark = 0;
		memset(phase_triangles, 0, sizeof(phase_triangles));
	}
}

void render_frame(
	struct render_window *windows,
	short window_count,
	point2d const *screenshot_page_index,
	point2d const *screenshot_index,
	struct bitmap_data *screenshot_bitmap,
	real time_delta_since_tick_sec)
{
	(void)screenshot_page_index;
	(void)screenshot_index;
	(void)screenshot_bitmap;
	frame_number++;
	render.frame_index++;
	render.time_delta_since_tick_sec = time_delta_since_tick_sec;
	system_profile_frame();
	time_phase(_time_game);
	dos_level_follow_bsp();
	{
		/* testing (setting log_scripts=<frames>): the waiting scripts */
		static long every = -1;

		if (every < 0)
			every = dos_setting_integer("log_scripts", 0);
		if (every > 0 && frame_number % every == 0)
			dos_log_scripts();
	}
	{
		/* testing (setting test_script=<command>@<frame>[;...]): a script
		command run as the frame begins (cheat_all_weapons@90) */
		const char *script = dos_setting("test_script", "");

		while (*script)
		{
			const char *end = strchr(script, ';'), *at;
			char command[100];
			size_t length = end ? (size_t)(end - script) : strlen(script);

			if (length < sizeof(command))
			{
				memcpy(command, script, length);
				command[length] = 0;
				at = strrchr(command, '@');
				if (at && atol(at + 1) == frame_number)
				{
					extern boolean hs_compile_and_evaluate(char const *expression);

					command[at - command] = 0;
					hs_compile_and_evaluate(command);
				}
			}
			script = end ? end + 1 : script + length;
		}
	}
	frame_begin();
	for (short window_index = 0; window_index < window_count; window_index++)
	{
		struct render_window *window = &windows[window_index];
		struct render_camera const *camera = &window->render_camera;
		struct r_view view;
		short eye_cluster = NONE;

		if (window->console_window)
		{
			/* the whole screen's: the console, the frame rate, timers */
			render.local_player_index = window->local_player_index;
			render.window_index = window_index;
			render.camera = *camera;
			interface_draw_fullscreen_overlays();
			/* the menu or the pause over it all (dos_menu.c) */
			dos_menu_frame();
			continue;
		}
		{
			static struct render_camera fixed;

			fixed = *camera;
			if (fixed_camera(&fixed))
				camera = &fixed;
		}
		last_view_camera = *camera;
		debug_camera_log(camera);
		view.position[0] = camera->position.x;
		view.position[1] = camera->position.y;
		view.position[2] = camera->position.z;
		view.forward[0] = camera->forward.i;
		view.forward[1] = camera->forward.j;
		view.forward[2] = camera->forward.k;
		view.up[0] = camera->up.i;
		view.up[1] = camera->up.j;
		view.up[2] = camera->up.k;
		view.vertical_field_of_view = camera->vertical_field_of_view;
		view.pixel_aspect = dos_pixel_aspect;
		view.z_near = camera->z_near > 0.0f ? camera->z_near : 0.0625f;
		view.z_far = draw_distance;
		/* (rounded: a scale like 200/480 is a hair under, and truncated the
		picture's last row and column were never drawn) */
		view.x0 = (int)(camera->viewport_bounds.x0 * dos_screen_scale_x + 0.5f);
		view.y0 = (int)(camera->viewport_bounds.y0 * dos_screen_scale_y + 0.5f);
		view.x1 = (int)(camera->viewport_bounds.x1 * dos_screen_scale_x + 0.5f);
		view.y1 = (int)(camera->viewport_bounds.y1 * dos_screen_scale_y + 0.5f);
		/* the world fades into the sky's colour toward the draw distance */
		view.fog_color = dos_level_sky_color();
		view.fog_start = draw_distance * dos_level_fog_start();
		view.fog_end = draw_distance;
		view.fog_maximum = 1.0f;
		view.fog_far = draw_distance;
		view.sky_colors = dos_level_sky_gradient();
		view_clusters = NULL;
		view_cluster_count = 0;
		{
			/* the clusters the eye can see (Halo's potentially visible set) */
			struct location location;
			struct structure_bsp *bsp = global_structure_bsp_get();
			real_point3d eye = camera->position;

			scenario_location_from_point(&location, &eye);
			if (bsp && location.cluster_index != NONE && bsp->cluster_data.size > 0)
			{
				view_clusters = structure_bsp_get_cluster_pvs(bsp, location.cluster_index);
				view_cluster_count = (short)bsp->clusters.count;
			}
			eye_cluster = location.cluster_index;
		}
		render.local_player_index = window->local_player_index;
		render.window_index = window_index;
		render.camera = *camera;
		{
			/* the sky's model first (the gradient behind it), the world over */
			struct location location;
			real_point3d eye = camera->position;
			long sky_model;

			if (sky_models_setting < 0)
				sky_models_setting = (int)dos_setting_integer("sky_model", 1);
			scenario_location_from_point(&location, &eye);
			sky_model = sky_model_at(location.cluster_index);
			/* (the colour behind the level and its fog's: this sky's) */
			dos_level_set_sky(sky_index_now, sky_model != NONE ? sky_model_color(sky_model) : 0, sky_model != NONE);
			{
				static long logged_model = -2;

				if (sky_model != logged_model)
				{
					char line[160];

					logged_model = sky_model;
					snprintf(line, sizeof(line), "sky: cluster %d, sky %d, model %s", location.cluster_index,
						sky_index_now, sky_model != NONE ? tag_get_name(sky_model) : "none");
					system_log(line);
				}
			}
			/* (not where the level has its own gradient: dos_level.c's table) */
			if (sky_model != NONE && (sky_models_setting || main_menu_is_active()) && !dos_level_sky_colors())
			{
				draw_sky_model(sky_model, &view);
				view.sky_colors = NULL;
			}
		}
		{
			/* the fog: on the Xbox's look (Direct3D 8) Halo's own atmospheric
			fog, the sky's, as far as its maximum density, then all of it in the
			draw distance's last stretch (Halo draws to 1024 units; past the
			draw distance here is the sky); then the eye's cluster's water fog
			over what is under its plane (dos_planar_fog.c) */
			float start, end, maximum, air_distance = 0.0f;
			unsigned long color;

			if (dos_xbox_fog() && dos_xbox_atmospheric_fog(window->local_player_index, sky_index_now, view.position,
				&start, &end, &maximum, &color))
			{
				float stretch = draw_distance * 0.85f;

				air_distance = end;
				/* (a fog longer than the draw distance in it, as it rises) */
				if (end > stretch)
				{
					start *= stretch / end;
					end = stretch;
				}
				view.fog_color = color;
				view.fog_start = start < end ? start : end - 1.0f;
				view.fog_end = end;
				view.fog_maximum = maximum;
				view.fog_far = stretch;
			}
			dos_planar_fog_view(eye_cluster, &view, sky_index_now, air_distance);
		}
		r_view_begin(&view);
		/* the first-person weapon first, in front of everything (its depth
		raised above the world's): the world's pixels behind it stop at the
		depth test. Its transparent parts go after the world, over it */
		/* (with the weapon's own near clip, as Halo draws it: at the world's
		its nearest parts were cut away) */
		dos_models_first_person(TRUE);
		r_view_set_near(rasterizer_globals.first_person_weapon_near_clip_distance);
		r_set_in_front(TRUE);
		dos_models_pass(DOS_MODELS_OPAQUE);
		/* (posed all the same with VIEW MODEL OFF: only not drawn) */
		first_person_weapon_render_update();
		if (dos_view_model_quality() != 3)
			first_person_weapon_draw();
		r_set_in_front(FALSE);
		r_view_set_near(view.z_near);
		dos_models_pass(DOS_MODELS_ALL);
		dos_models_first_person(FALSE);
		time_phase(_time_first_person);
		dos_level_draw(view_clusters, view_cluster_count);
		time_phase(_time_level);
		render_objects_for_view(window->local_player_index);
		time_phase(_time_objects);
		dos_level_draw_transparent(view_clusters, view_cluster_count);
		time_phase(_time_level);
		/* the particles over the world (dos_particles.c) */
		dos_particles_draw(window->local_player_index, FALSE, view.y1 - view.y0);
		time_phase(_time_objects);
		if (dos_models_transparent_left_out())
		{
			dos_models_first_person(TRUE);
			r_view_set_near(rasterizer_globals.first_person_weapon_near_clip_distance);
			r_set_in_front(TRUE);
			dos_models_pass(DOS_MODELS_TRANSPARENT);
			if (dos_view_model_quality() != 3)
				first_person_weapon_draw();
			r_set_in_front(FALSE);
			r_view_set_near(view.z_near);
			dos_models_pass(DOS_MODELS_ALL);
			dos_models_first_person(FALSE);
			time_phase(_time_first_person);
		}
		/* the first-person weapon's own (its muzzle flash), in front with it */
		r_view_set_near(rasterizer_globals.first_person_weapon_near_clip_distance);
		r_set_in_front(TRUE);
		dos_particles_draw(window->local_player_index, TRUE, view.y1 - view.y0);
		r_set_in_front(FALSE);
		r_view_set_near(view.z_near);
		time_phase(_time_first_person);
		r_view_end();
		/* the game's own HUD (dos_hud_game.c) */
		interface_draw_screen();
		time_phase(_time_hud);
	}
}

void render_frame_pregame(
	struct render_window const *window,
	struct bitmap_data *bitmap)
{
	(void)window;
	(void)bitmap;
	frame_number++;
	render.frame_index++;
	frame_begin();
}

/* testing (settings screenshot_frame=<n>, screenshot_every=<n>,
quit_frame=<n>): the picture of a frame as a BMP beside the program
(SHOTnnnn.BMP), and the end of the program at a frame; with the timings of
the frames between in the log */
static void write_screenshot_named(
	const struct system_frame *frame,
	const char *name)
{
	char path[300];
	FILE *file;
	unsigned char header[54];
	unsigned long row_bytes = (unsigned long)frame->width * 3, padded = (row_bytes + 3) & ~3UL;
	unsigned long size = 54 + padded * (unsigned long)frame->height;
	unsigned char *row = malloc(padded);
	const unsigned char *palette = r_palette();

	/* (the game's fopen takes the Xbox's paths: d:\ is the program's folder) */
	snprintf(path, sizeof(path), "d:\\%s", name);
	file = fopen(path, "wb");
	if (!file || !row)
	{
		if (file)
			fclose(file);
		free(row);
		return;
	}
	memset(header, 0, sizeof(header));
	header[0] = 'B';
	header[1] = 'M';
	*(unsigned long *)(header + 2) = size;
	*(unsigned long *)(header + 10) = 54;
	*(unsigned long *)(header + 14) = 40;
	*(long *)(header + 18) = frame->width;
	*(long *)(header + 22) = frame->height;
	*(unsigned short *)(header + 26) = 1;
	*(unsigned short *)(header + 28) = 24;
	*(unsigned long *)(header + 34) = padded * (unsigned long)frame->height;
	fwrite(header, 1, sizeof(header), file);
	memset(row, 0, padded);
	for (int y = frame->height - 1; y >= 0; y--)
	{
		const unsigned char *in = (const unsigned char *)frame->pixels + (size_t)y * frame->pitch;

		for (int x = 0; x < frame->width; x++)
		{
			unsigned long c = frame->bits == 8 ?
				((unsigned long)palette[in[x] * 3] << 16) | ((unsigned long)palette[in[x] * 3 + 1] << 8) |
				palette[in[x] * 3 + 2] : ((const unsigned long *)in)[x];

			row[x * 3 + 0] = (unsigned char)c;
			row[x * 3 + 1] = (unsigned char)(c >> 8);
			row[x * 3 + 2] = (unsigned char)(c >> 16);
		}
		fwrite(row, 1, padded, file);
	}
	fclose(file);
	free(row);
	system_log(name);
}

static void write_screenshot(
	const struct system_frame *frame)
{
	char name[16];

	snprintf(name, sizeof(name), "SHOT%04ld.BMP", frame_number % 10000);
	write_screenshot_named(frame, name);
}

static void testing_frame(
	const struct system_frame *frame)
{
	static long screenshot_frame = -1, screenshot_every, quit_frame;
	static unsigned long long last_time;
	static long timed_frames;
	unsigned long long now = system_microseconds();

	if (screenshot_frame < 0)
	{
		screenshot_frame = dos_setting_integer("screenshot_frame", 0);
		screenshot_every = dos_setting_integer("screenshot_every", 0);
		quit_frame = dos_setting_integer("quit_frame", 0);
		last_time = now;
	}
	timed_frames++;
	if (now - last_time >= 5000000)
	{
		const struct r_statistics *statistics = r_frame_statistics();
		char line[160];

		snprintf(line, sizeof(line), "frame %ld: %.1f ms a frame; last frame %ld triangles drawn, %ld culled, "
			"%ld pixels, %ld rows, %ld not finite, %ld affine", frame_number,
			(double)(now - last_time) / 1000.0 / (double)timed_frames, statistics->triangles_drawn,
			statistics->triangles_culled, statistics->pixels_drawn, statistics->rows_scanned,
			statistics->triangles_not_finite, statistics->triangles_affine);
		system_log(line);
		snprintf(line, sizeof(line), "  ms a frame: game %.1f, level %.1f, objects %.1f, first person %.1f, HUD %.1f, "
			"present %.1f", frame_times[_time_game] / 1000.0 / timed_frames,
			frame_times[_time_level] / 1000.0 / timed_frames, frame_times[_time_objects] / 1000.0 / timed_frames,
			frame_times[_time_first_person] / 1000.0 / timed_frames, frame_times[_time_hud] / 1000.0 / timed_frames,
			frame_times[_time_present] / 1000.0 / timed_frames);
		system_log(line);
		snprintf(line, sizeof(line), "  triangles a frame: level %ld, objects %ld, first person %ld, HUD %ld",
			phase_triangles[_time_level], phase_triangles[_time_objects], phase_triangles[_time_first_person],
			phase_triangles[_time_hud]);
		system_log(line);
		snprintf(line, sizeof(line), "  camera %.1f %.1f %.1f, yaw %.0f, pitch %.0f", (double)last_view_camera.position.x,
			(double)last_view_camera.position.y, (double)last_view_camera.position.z,
			(double)(atan2f(last_view_camera.forward.j, last_view_camera.forward.i) * 57.2957795f),
			(double)(asinf(PIN(last_view_camera.forward.k, -1.0f, 1.0f)) * 57.2957795f));
		system_log(line);
		memset(frame_times, 0, sizeof(frame_times));
		last_time = now;
		timed_frames = 0;
	}
	if ((screenshot_frame && frame_number == screenshot_frame) ||
		(screenshot_every && frame_number % screenshot_every == 0))
	{
		write_screenshot(frame);
	}
	if (quit_frame && frame_number >= quit_frame)
	{
		system_log("quit_frame reached");
		exit(0);
	}
}

void render_frame_present(
	const point2d *screenshot_index,
	struct bitmap_data *bitmap)
{
	struct system_frame frame;

	(void)screenshot_index;
	(void)bitmap;
	frame_begin();
	r_frame_end();
	frame.pixels = r_frame_pixels(&frame.width, &frame.height, &frame.pitch, &frame.bits);
	if (r_hardware_active())
	{
		/* (OpenGL or a Voodoo draws in the card: a screenshot's frame read
		back first) */
		long every = dos_setting_integer("screenshot_every", 0), at = dos_setting_integer("screenshot_frame", 0);

		if ((at && frame_number == at) || (every && frame_number % every == 0))
			r_hardware_read_back();
	}
	system_display_present(&frame);
	time_phase(_time_present);
	{
		/* (FRAME RATE 60 or 120: the frame's least time waited out) */
		extern unsigned long dos_frame_interval(void);
		static unsigned long long last;
		unsigned long interval = dos_frame_interval();
		unsigned long long now = system_microseconds();

		if (interval && last && now - last < interval)
		{
			extern void dsound_pump(void);

			dsound_pump();
			system_sleep((unsigned long)((interval - (now - last)) / 1000));
			now = system_microseconds();
		}
		last = now;
	}
	testing_frame(&frame);
	frame_begun = FALSE;
	if (vertical_blank_callback)
		vertical_blank_callback(0);
}

/* ---------- loading screens

Whenever a map loads (game_precache_new_map's hook: from the menu to a
game, back, a campaign level to the next, a network game's), a loading
screen: the map's picture, its name and what is played, drawn by the menu
(dos_menu_draw_loading) with the old map's fonts while they are still
there, kept as a picture, then shown with a bar over it as the load goes
(the map's reading, its decompression the first time, the level's
building) until the new map's first frame. The bar's shares: reading the
tag data 0-40%, a first game's decompression 0-60%, the level's building
50-95%; it only ever moves on. */

void dos_menu_draw_loading(const char *map_name, int width, int height);


static void loading_show(int forced)
{
	unsigned long long now = system_microseconds();
	struct system_frame frame;
	int width, height, pitch, bits, x0, x1, y0, y1, filled;
	float sweep;

	if (!loading.active || !rasterizer_globals.initialized || (!forced && now - loading.last_shown < 100000))
		return;
	loading.last_shown = now;
	/* (internet play's peers heard from during a long load, and the sound
	kept playing) */
	dos_p2p_poll();
	{
		extern void dsound_pump(void);

		dsound_pump();
	}
	r_frame_pixels(&width, &height, &pitch, &bits);
	r_frame_begin(0x000000);
	if (loading.picture && loading.width == width && loading.height == height)
		r_draw_picture(0, 0, loading.picture, width, height);
	/* the bar: a dark track, Halo's blue as far as it has come, a light
	sweeping along it (moving even where the load says nothing); under the
	centred 640 columns on a wide screen */
	x0 = (int)(dos_screen_centred_offset() + 120.0f * dos_screen_scale_x);
	x1 = (int)(dos_screen_centred_offset() + 520.0f * dos_screen_scale_x);
	y0 = height * 434 / 480;
	y1 = y0 + (height >= 400 ? 6 : 3);
	filled = x0 + (int)((long)(x1 - x0) * PIN(loading.permille, 0, 1000) / 1000);
	r_fill_rectangle(x0 - 1, y0 - 1, x1 + 1, y1 + 1, 0xFF203048);
	r_fill_rectangle(x0, y0, x1, y1, 0xFF040810);
	r_fill_rectangle(x0, y0, filled, y1, 0xFF2878E8);
	r_fill_rectangle(x0, y0, filled, y0 + 1, 0xFF8CC8FF);
	sweep = (float)((now - loading.started) % 1400000ULL) / 1400000.0f;
	{
		int sx = x0 + (int)((float)(x1 - x0 + width / 8) * sweep) - width / 8;
		int sx1 = sx + width / 8;

		r_fill_rectangle(sx < x0 ? x0 : sx, y0, sx1 > x1 ? x1 : sx1, y1, 0x50A0D8FF);
	}
	frame.pixels = r_frame_pixels(&frame.width, &frame.height, &frame.pitch, &frame.bits);
	/* (testing, setting screenshot_loading=1: each loading screen shown as
	LOADnnnn.BMP) */
	if (dos_setting_integer("screenshot_loading", 0))
	{
		static int shots;
		char name[16];

		if (r_hardware_active())
			r_hardware_read_back();
		snprintf(name, sizeof(name), "LOAD%04d.BMP", shots++ % 10000);
		write_screenshot_named(&frame, name);
	}
	system_display_present(&frame);
}

void dos_loading_begin(const char *map_name)
{
	int width, height, pitch, bits;
	const unsigned char *pixels;

	if (loading.active || !rasterizer_globals.initialized)
		return;
	/* the screen to show: drawn as a frame, then kept as a picture (a card's
	frame read back first) */
	pixels = r_frame_pixels(&width, &height, &pitch, &bits);
	r_frame_begin(0x000000);
	dos_menu_draw_loading(map_name, width, height);
	if (r_hardware_active())
		r_hardware_read_back();
	pixels = r_frame_pixels(&width, &height, &pitch, &bits);
	(free)(loading.picture);
	loading.picture = (malloc)((size_t)width * height * sizeof(unsigned long));
	if (loading.picture && pixels)
	{
		for (int y = 0; y < height; y++)
		{
			const unsigned char *row = pixels + (size_t)y * pitch;
			unsigned long *out = loading.picture + (size_t)y * width;

			for (int x = 0; x < width; x++)
			{
				if (bits == 8)
					out[x] = 0xFF000000UL | r_palette_color(row[x]);
				else
				{
					out[x] = 0xFF000000UL | (((const unsigned long *)row)[x] & 0x00FFFFFFUL);
				}
			}
		}
	}
	loading.width = width;
	loading.height = height;
	loading.active = TRUE;
	loading.permille = 0;
	loading.started = system_microseconds();
	loading.last_shown = 0;
	loading_show(TRUE);
}

static void loading_moved(long permille)
{
	if (permille > loading.permille)
		loading.permille = permille;
	loading_show(FALSE);
}

void dos_loading_read(unsigned long done, unsigned long total)
{
	loading_moved((long)((unsigned long long)done * 400 / (total ? total : 1)));
}

void dos_loading_level(long done, long total)
{
	loading_moved(500 + done * 450 / (total ? total : 1));
}

/* a long wait (a map's decompression, port/dos/xdk/xdk_kernel.c): the
words to the log once, the bar on (a load begun before the menu had a
screen to draw shows the bar alone) */
void dos_loading_progress(
	const char *what,
	long permille)
{
	static char last_what[64];

	if (strcmp(last_what, what))
	{
		snprintf(last_what, sizeof(last_what), "%s", what);
		system_log(what);
	}
	if (!loading.active && rasterizer_globals.initialized)
	{
		loading.active = TRUE;
		loading.picture = NULL;
		loading.permille = 0;
		loading.started = system_microseconds();
	}
	loading_moved(permille * 600 / 1000);
}

/* the new map's first frame: the loading screen gone */
static void loading_end(void)
{
	loading.active = FALSE;
	(free)(loading.picture);
	loading.picture = NULL;
}

boolean render_location_visible(
	struct location *location)
{
	(void)location;
	return TRUE;
}
