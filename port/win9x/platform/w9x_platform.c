/*
W9X_PLATFORM.C

The window, OpenGL context and event loop behind the Windows 98/ME build:
port/linux/src/sdl_platform.c's interface (sdl_platform.h) without SDL3,
which needs Windows Vista. The 9x build compiles this in place of
sdl_platform.c.

This unit is XDK-facing, like sdl_platform.c: it sees platform.h and the
settings, and keeps the input state the controller emulation
(xinput_sdl.c) and the debug keyboard read. Everything Win32 is in the
w9x_*.c units behind w9x_backend.h, which call back w9x_on_* below as keys,
the mouse and focus change.

A little of sdl_platform.c is repeated here rather than shared, so that the
Linux, Windows and Android builds keep exactly their code: the input state
and its keys-pressed latch, the debug keyboard's queue and its virtual keys
(by the key's place, as there; the character is the keyboard layout's),
the menus' pointer, F11 and F12, debug.exit_after, the invite clipboard, and
the first start's offer to extract the maps folder.

The platform layer only makes the window and a legacy OpenGL context and
says what it got (platform_video_info, logged at start); which renderer
draws with it is the renderer's business. Until one that runs on these
drivers exists, a context without the renderer's functions leaves the
window open and black: the keyboard, the mouse and sound still work.
*/

#include "platform.h"
#include "sdl_platform.h"
#include "gl.h"
#include "port_config.h"
#include "p2p.h"
#include "xiso.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static BOOL platform_started = FALSE;
static BOOL platform_window = FALSE;
static BOOL platform_video_result_valid = FALSE;
static struct w9x_video_request video_request;
static BOOL video_request_ready = FALSE;
static struct w9x_video_result video_result;
static pthread_t platform_event_thread;

static struct platform_input_state input_state;
/* keys pressed since the last read, so a press and release between two
reads still counts as a press (a slow frame) */
static unsigned char keys_pressed[SDL_SCANCODE_COUNT];
/* the menus' pointer (platform_ui_pointer_set_active), under input_lock */
static struct platform_ui_pointer ui_pointer;
static float ui_pointer_wheel;
static pthread_mutex_t input_lock = PTHREAD_MUTEX_INITIALIZER;

/* debug keyboard queue */
#define KEYSTROKE_QUEUE_SIZE 64
static struct platform_keystroke keystroke_queue[KEYSTROKE_QUEUE_SIZE];
static unsigned long keystroke_head, keystroke_count;

BOOL platform_sdl_initialize(void)
{
	if (platform_started)
		return TRUE;
	/* a copy of the game started to open an invite link hands it to the
	one already running, and goes */
	if (p2p_hand_off_invite())
		exit(EXIT_SUCCESS);
	platform_started = TRUE;
	w9x_input_configure(config_string("input.gamepad_map"));
	/* found (or offered to the player, platform_offer_game_data) before the
	game's window opens */
	platform_data_root();
	return TRUE;
}

/* ---------- first start without game data (xbox_files.c) */

struct data_extraction
{
	pthread_mutex_t lock;
	char image[1024];
	char destination[1024];
	char file[256];
	unsigned long long done;
	unsigned long long total;
	BOOL finished;
	BOOL succeeded;
	char error[512];
};

static void data_extraction_progress(void *context, const char *file, unsigned long long done,
	unsigned long long total)
{
	struct data_extraction *extraction = context;

	pthread_mutex_lock(&extraction->lock);
	snprintf(extraction->file, sizeof(extraction->file), "%s", file);
	extraction->done = done;
	extraction->total = total;
	pthread_mutex_unlock(&extraction->lock);
}

static void *data_extraction_thread(void *context)
{
	struct data_extraction *extraction = context;
	BOOL succeeded = xiso_extract_maps(extraction->image, extraction->destination, data_extraction_progress,
		extraction, extraction->error, sizeof(extraction->error)) != 0;

	pthread_mutex_lock(&extraction->lock);
	extraction->succeeded = succeeded;
	extraction->finished = TRUE;
	pthread_mutex_unlock(&extraction->lock);
	return NULL;
}

/* copies the maps, showing how far it has got; closing the window quits */
static BOOL data_extract(const char *image, const char *destination, char *error, int error_size)
{
	static struct data_extraction extraction;
	pthread_t thread;
	void *progress;
	BOOL finished = FALSE;

	memset(&extraction, 0, sizeof(extraction));
	pthread_mutex_init(&extraction.lock, NULL);
	snprintf(extraction.image, sizeof(extraction.image), "%s", image);
	snprintf(extraction.destination, sizeof(extraction.destination), "%s", destination);
	if (pthread_create(&thread, NULL, data_extraction_thread, &extraction) != 0)
	{
		snprintf(error, (size_t)error_size, "Could not start the extraction.");
		return FALSE;
	}
	/* (waited for through extraction.finished; the Windows port's threads
	cannot be joined) */
	pthread_detach(thread);
	progress = w9x_progress_open("Extracting the maps folder...");
	while (!finished)
	{
		char line[320];
		unsigned long long done, total;

		pthread_mutex_lock(&extraction.lock);
		finished = extraction.finished;
		snprintf(line, sizeof(line), "%s  (%lu of %lu MB)", extraction.file, (unsigned long)(extraction.done >> 20),
			(unsigned long)(extraction.total >> 20));
		done = extraction.done;
		total = extraction.total;
		pthread_mutex_unlock(&extraction.lock);
		if (!w9x_progress_update(progress, line, total ? (int)(done * 1000 / total) : 0))
		{
			platform_log("extraction cancelled");
			exit(EXIT_SUCCESS);
		}
		Sleep(50);
	}
	w9x_progress_close(progress);
	if (!extraction.succeeded)
		snprintf(error, (size_t)error_size, "%s", extraction.error);
	return extraction.succeeded;
}

BOOL platform_offer_game_data(const char *destination)
{
	char message[1400];

	/* not for runs nobody is watching */
	if (config_boolean("debug.hidden_window") || config_real("debug.exit_after") > 0.0)
		return FALSE;
	snprintf(message, sizeof(message),
		"Halo's game data (its maps folder) was not found.\n\n"
		"Extract the maps folder from an Xbox disc image (.iso) of Halo: Combat Evolved? "
		"It is copied to %s\\maps (about 2 GB).\n\n"
		"(Or put the maps folder there yourself, or set paths.data in config.toml.)",
		destination);
	for (;;)
	{
		char image[1024];
		char error[512];

		if (!w9x_message_yes_no(message))
		{
			platform_log("no game data: quitting");
			exit(EXIT_SUCCESS);
		}
		/* no image picked: ask again */
		if (!w9x_choose_file("Halo: choose the Xbox disc image", "Xbox disc images (*.iso, *.xiso)", "*.iso;*.xiso",
			image, sizeof(image)))
		{
			continue;
		}
		platform_log("extracting the maps folder from %s to %s", image, destination);
		if (data_extract(image, destination, error, sizeof(error)))
		{
			platform_log("extracted the maps folder");
			return TRUE;
		}
		platform_log("extraction failed: %s", error);
		w9x_message_error(error);
	}
}

int halo_interpolation_enabled(void)
{
	static int enabled = -1;

	if (enabled < 0)
		enabled = config_boolean("display.interpolation");
	return enabled;
}

/* ---------- the window */

/* display.mode: "640x480", "800x600x16", "desktop" (or empty) for no mode
change */
static void display_mode_setting(int *width, int *height, int *bits)
{
	const char *text = config_string("display.mode");
	int parsed_width = 0, parsed_height = 0, parsed_bits = 0;

	*width = 0;
	*height = 0;
	*bits = 0;
	if (!*text || !strcmp(text, "desktop"))
		return;
	if (sscanf(text, "%dx%dx%d", &parsed_width, &parsed_height, &parsed_bits) < 2 || parsed_width < 320 ||
		parsed_height < 240 || (parsed_bits && parsed_bits != 16 && parsed_bits != 24 && parsed_bits != 32))
	{
		platform_log("settings: display.mode \"%s\" is not WIDTHxHEIGHT or WIDTHxHEIGHTxBITS (16, 24 or 32); "
			"keeping the desktop's", text);
		return;
	}
	*width = parsed_width;
	*height = parsed_height;
	*bits = parsed_bits;
}

/* what the window is to be, from the settings (read once): width by height
times display.window_scale when windowed */
static const struct w9x_video_request *video_request_get(unsigned long width, unsigned long height)
{
	static int scale;

	if (video_request_ready)
	{
		video_request.width = (int)width * scale;
		video_request.height = (int)height * scale;
		return &video_request;
	}
	video_request_ready = TRUE;
	scale = (int)config_integer("display.window_scale");
	if (scale < 1)
		scale = 1;
	memset(&video_request, 0, sizeof(video_request));
	video_request.width = (int)width * scale;
	video_request.height = (int)height * scale;
	video_request.hidden = config_boolean("debug.hidden_window");
	video_request.fullscreen = !video_request.hidden && config_boolean("display.fullscreen");
	display_mode_setting(&video_request.mode_width, &video_request.mode_height, &video_request.mode_bits);
	/* what the renderers would like best; less is taken when that is all
	there is, and video_result says what */
	video_request.color_bits = 32;
	video_request.alpha_bits = 8;
	video_request.depth_bits = 24;
	video_request.stencil_bits = 8;
	video_request.allow_software = config_boolean("display.allow_software_gl");
	return &video_request;
}

/* whether the game is, or is to be, fullscreen, and if so the size in
pixels of the display it fills (d3d8_gl.c draws at that resolution) */
BOOL platform_screen_mode(long *width, long *height)
{
	int screen_width, screen_height;

	if (!w9x_video_screen_mode(video_request_get(640, 480), &screen_width, &screen_height))
		return FALSE;
	*width = screen_width;
	*height = screen_height;
	return TRUE;
}

static void video_report(const struct w9x_video_result *result)
{
	static const char *const accelerations[] =
	{
		"Microsoft's software renderer",
		"a mini client driver (Microsoft's OpenGL with the card's help)",
		"the display driver's own (an installable client driver)",
	};

	platform_log("display: %s, %dx%d at %d bits a pixel%s", result->fullscreen ? "fullscreen" : "a window",
		result->display_width, result->display_height, result->display_bits,
		result->mode_changed ? " (the display mode was changed for it)" : "");
	if (!result->gl)
		return;
	platform_log("OpenGL %s on %s (%s): %s", result->version, result->renderer, result->vendor,
		accelerations[result->acceleration]);
	platform_log("OpenGL pixel format %d: %d-bit colour (%d %d %d, alpha %d), depth %d, stencil %d%s",
		result->pixel_format, result->color_bits, result->red_bits, result->green_bits, result->blue_bits,
		result->alpha_bits, result->depth_bits, result->stencil_bits,
		result->swap_control ? "; the swap interval can be set" : "; no swap interval control");
}

BOOL platform_video_initialize(unsigned long width, unsigned long height)
{
	char error[512];

	if (platform_window)
		return platform_video_result_valid;
	if (!platform_sdl_initialize())
		return FALSE;
	if (!w9x_video_open(video_request_get(width, height), &video_result, error, sizeof(error)))
	{
		platform_log("cannot open the window: %s", error);
		return FALSE;
	}
	platform_window = TRUE;
	platform_event_thread = pthread_self();
	video_report(&video_result);
	platform_mouse_capture(TRUE);
	if (!video_result.gl)
	{
		platform_log("no OpenGL: %s", error);
		/* (said to whoever watches; automated runs go on without) */
		if (!video_request.hidden && config_real("debug.exit_after") <= 0.0)
			w9x_message_error(error);
		return FALSE;
	}
	if (!gl_functions_load())
	{
		/* (the renderer's functions are logged one by one above) */
		platform_log("the renderer cannot run on OpenGL %d.%d: the window stays black", video_result.version_major,
			video_result.version_minor);
		return FALSE;
	}
	if (!w9x_gl_swap_interval(config_boolean("display.vsync") ? 1 : 0) && config_boolean("display.vsync"))
		platform_log("display.vsync: the driver cannot set the swap interval (its control panel may)");
	platform_video_result_valid = TRUE;
	return TRUE;
}

/* the window and OpenGL as they turned out (zeroes before the window
opens) */
const struct w9x_video_result *platform_video_info(void)
{
	return &video_result;
}

void platform_video_drawable_size(int *width, int *height)
{
	w9x_video_size(width, height);
}

void platform_video_window_size(int *width, int *height)
{
	/* (no scaling for high pixel densities on 9x: one and the same) */
	w9x_video_size(width, height);
}

void platform_video_swap(void)
{
	w9x_video_swap();
}

void platform_mouse_capture(BOOL capture)
{
	if (platform_window)
		w9x_mouse_capture(capture ? 1 : 0);
}

/* ---------- keyboard translation */

/* Windows virtual key code for an SDL scancode (the Xbox debug keyboard
reports virtual keys); sdl_platform.c's table */
static BYTE virtual_key_from_scancode(SDL_Scancode scancode)
{
	if (scancode >= SDL_SCANCODE_A && scancode <= SDL_SCANCODE_Z)
		return (BYTE)('A' + (scancode - SDL_SCANCODE_A));
	if (scancode >= SDL_SCANCODE_1 && scancode <= SDL_SCANCODE_9)
		return (BYTE)('1' + (scancode - SDL_SCANCODE_1));
	if (scancode >= SDL_SCANCODE_F1 && scancode <= SDL_SCANCODE_F12)
		return (BYTE)(0x70 + (scancode - SDL_SCANCODE_F1));
	if (scancode >= SDL_SCANCODE_KP_1 && scancode <= SDL_SCANCODE_KP_9)
		return (BYTE)(0x61 + (scancode - SDL_SCANCODE_KP_1));
	switch (scancode)
	{
	case SDL_SCANCODE_0: return '0';
	case SDL_SCANCODE_KP_0: return 0x60;
	case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return 0x0d;
	case SDL_SCANCODE_ESCAPE: return 0x1b;
	case SDL_SCANCODE_BACKSPACE: return 0x08;
	case SDL_SCANCODE_TAB: return 0x09;
	case SDL_SCANCODE_SPACE: return 0x20;
	case SDL_SCANCODE_MINUS: return 0xbd;
	case SDL_SCANCODE_EQUALS: return 0xbb;
	case SDL_SCANCODE_LEFTBRACKET: return 0xdb;
	case SDL_SCANCODE_RIGHTBRACKET: return 0xdd;
	case SDL_SCANCODE_BACKSLASH: return 0xdc;
	case SDL_SCANCODE_SEMICOLON: return 0xba;
	case SDL_SCANCODE_APOSTROPHE: return 0xde;
	case SDL_SCANCODE_GRAVE: return 0xc0;
	case SDL_SCANCODE_COMMA: return 0xbc;
	case SDL_SCANCODE_PERIOD: return 0xbe;
	case SDL_SCANCODE_SLASH: return 0xbf;
	case SDL_SCANCODE_CAPSLOCK: return 0x14;
	case SDL_SCANCODE_PRINTSCREEN: return 0x2c;
	case SDL_SCANCODE_SCROLLLOCK: return 0x91;
	case SDL_SCANCODE_PAUSE: return 0x13;
	case SDL_SCANCODE_INSERT: return 0x2d;
	case SDL_SCANCODE_HOME: return 0x24;
	case SDL_SCANCODE_PAGEUP: return 0x21;
	case SDL_SCANCODE_DELETE: return 0x2e;
	case SDL_SCANCODE_END: return 0x23;
	case SDL_SCANCODE_PAGEDOWN: return 0x22;
	case SDL_SCANCODE_RIGHT: return 0x27;
	case SDL_SCANCODE_LEFT: return 0x25;
	case SDL_SCANCODE_DOWN: return 0x28;
	case SDL_SCANCODE_UP: return 0x26;
	case SDL_SCANCODE_NUMLOCKCLEAR: return 0x90;
	case SDL_SCANCODE_KP_DIVIDE: return 0x6f;
	case SDL_SCANCODE_KP_MULTIPLY: return 0x6a;
	case SDL_SCANCODE_KP_MINUS: return 0x6d;
	case SDL_SCANCODE_KP_PLUS: return 0x6b;
	case SDL_SCANCODE_KP_PERIOD: return 0x6e;
	case SDL_SCANCODE_LCTRL: return 0xa2;
	case SDL_SCANCODE_RCTRL: return 0xa3;
	case SDL_SCANCODE_LSHIFT: return 0xa0;
	case SDL_SCANCODE_RSHIFT: return 0xa1;
	case SDL_SCANCODE_LALT: return 0xa4;
	case SDL_SCANCODE_RALT: return 0xa5;
	default: return 0;
	}
}

static void queue_keystroke(int scancode, int ascii, int modifiers, BOOL down, BOOL repeat)
{
	struct platform_keystroke *keystroke;
	BYTE flags = (BYTE)(modifiers & (W9X_KEY_CTRL | W9X_KEY_SHIFT | W9X_KEY_ALT | W9X_KEY_CAPS_LOCK |
		W9X_KEY_NUM_LOCK));

	if (keystroke_count == KEYSTROKE_QUEUE_SIZE)
	{
		keystroke_head = (keystroke_head + 1) % KEYSTROKE_QUEUE_SIZE;
		keystroke_count--;
	}
	keystroke = &keystroke_queue[(keystroke_head + keystroke_count) % KEYSTROKE_QUEUE_SIZE];
	if (!down) flags |= 0x40;
	if (repeat) flags |= 0x80;
	keystroke->virtual_key = virtual_key_from_scancode((SDL_Scancode)scancode);
	/* ASCII only: the game looks the character up in a table of 128
	(input_xbox.c), and a layout's é, ß or £ is a negative CHAR */
	keystroke->ascii = down && ascii > 0 && ascii < 0x80 ? (CHAR)ascii : 0;
	keystroke->flags = flags;
	keystroke_count++;
}

BOOL platform_next_keystroke(struct platform_keystroke *keystroke)
{
	BOOL result = FALSE;

	pthread_mutex_lock(&input_lock);
	if (keystroke_count)
	{
		*keystroke = keystroke_queue[keystroke_head];
		keystroke_head = (keystroke_head + 1) % KEYSTROKE_QUEUE_SIZE;
		keystroke_count--;
		result = TRUE;
	}
	pthread_mutex_unlock(&input_lock);
	return result;
}

/* ---------- internet play's invite links (p2p.c) */

/* puts a new invite on the clipboard, and joins one found there when the
game comes to the front */
static void platform_invite_clipboard(BOOL look)
{
	/* the last clipboard text looked at, so each invite is joined once */
	static char seen[256];
	const char *invite = p2p_take_clipboard_text();

	if (invite)
	{
		w9x_clipboard_set(invite);
		snprintf(seen, sizeof(seen), "%s", invite);
		platform_log("Internet play: the invite link is on the clipboard");
	}
	if (look && config_boolean("network.join_from_clipboard"))
	{
		char text[sizeof(seen)];

		if (w9x_clipboard_get(text, sizeof(text)) && strcmp(text, seen))
		{
			snprintf(seen, sizeof(seen), "%s", text);
			p2p_join_invite(text);
		}
	}
}

/* ---------- events (w9x_backend.h's w9x_on_*, from w9x_pump) */

static BOOL focus_regained;

void w9x_on_key(int scancode, int ascii, int modifiers, int down, int repeat)
{
	BOOL toggle_mouse = FALSE, toggle_fullscreen = FALSE;

	pthread_mutex_lock(&input_lock);
	if (scancode > 0 && scancode < SDL_SCANCODE_COUNT)
	{
		input_state.keys[scancode] = down ? 1 : 0;
		if (down)
			keys_pressed[scancode] = 1;
	}
	queue_keystroke(scancode, ascii, modifiers, down != 0, repeat != 0);
	/* F12 releases or recaptures the mouse */
	if (down && !repeat && scancode == SDL_SCANCODE_F12)
	{
		input_state.mouse_released = !input_state.mouse_released;
		toggle_mouse = TRUE;
	}
	/* F11 switches between fullscreen and the window */
	if (down && !repeat && scancode == SDL_SCANCODE_F11)
		toggle_fullscreen = TRUE;
	pthread_mutex_unlock(&input_lock);
	if (toggle_mouse)
		platform_mouse_capture(!input_state.mouse_released && !input_state.ui_pointer);
	if (toggle_fullscreen)
		w9x_video_toggle_fullscreen();
}

void w9x_on_mouse_motion(float dx, float dy)
{
	pthread_mutex_lock(&input_lock);
	/* (only while captured, which the menus' pointer never is) */
	if (!input_state.ui_pointer)
	{
		input_state.mouse_dx += dx;
		input_state.mouse_dy += dy;
	}
	pthread_mutex_unlock(&input_lock);
}

void w9x_on_pointer_motion(int x, int y)
{
	pthread_mutex_lock(&input_lock);
	/* in the menus the mouse moves the pointer, not the view */
	if (input_state.ui_pointer)
	{
		ui_pointer.x = (float)x;
		ui_pointer.y = (float)y;
		ui_pointer.moved = TRUE;
	}
	pthread_mutex_unlock(&input_lock);
}

void w9x_on_mouse_button(int button, int down, int x, int y)
{
	pthread_mutex_lock(&input_lock);
	/* clicks in the menus go to the pointer; a button held down when the
	menu closes stays up until pressed again, so the click that resumes the
	game does not also fire */
	if (input_state.ui_pointer)
	{
		if (down && button == SDL_BUTTON_LEFT)
		{
			ui_pointer.left_clicks++;
			ui_pointer.click_x = (float)x;
			ui_pointer.click_y = (float)y;
		}
		else if (down && button == SDL_BUTTON_RIGHT)
		{
			ui_pointer.right_clicks++;
		}
	}
	else if (button >= 0 && button < PLATFORM_MOUSE_BUTTON_COUNT)
	{
		input_state.mouse_buttons[button] = down ? 1 : 0;
	}
	pthread_mutex_unlock(&input_lock);
}

void w9x_on_wheel(float notches)
{
	pthread_mutex_lock(&input_lock);
	if (input_state.ui_pointer)
	{
		/* whole notches */
		ui_pointer_wheel += notches;
		while (ui_pointer_wheel >= 1.0f)
		{
			ui_pointer.wheel_steps++;
			ui_pointer_wheel -= 1.0f;
		}
		while (ui_pointer_wheel <= -1.0f)
		{
			ui_pointer.wheel_steps--;
			ui_pointer_wheel += 1.0f;
		}
	}
	else
	{
		input_state.mouse_wheel += notches;
	}
	pthread_mutex_unlock(&input_lock);
}

void w9x_on_focus(int focused)
{
	BOOL capture = FALSE;

	pthread_mutex_lock(&input_lock);
	if (!focused)
	{
		memset(input_state.keys, 0, sizeof(input_state.keys));
		memset(input_state.mouse_buttons, 0, sizeof(input_state.mouse_buttons));
		input_state.focused = FALSE;
	}
	else
	{
		input_state.focused = TRUE;
		focus_regained = TRUE;
		capture = !input_state.mouse_released && !input_state.ui_pointer;
	}
	pthread_mutex_unlock(&input_lock);
	if (capture)
		platform_mouse_capture(TRUE);
}

void platform_pump_events(void)
{
	/* debug.exit_after (seconds) ends the game that long after the window
	opens, as closing it does (tools/pgo_train.py) */
	static unsigned long long exit_ticks = (unsigned long long)-1;
	static BOOL looked_at_clipboard;
	BOOL look_at_clipboard = !looked_at_clipboard;

	if (!platform_window || !pthread_equal(pthread_self(), platform_event_thread))
		return;
	if (exit_ticks == (unsigned long long)-1)
	{
		double seconds = config_real("debug.exit_after");

		exit_ticks = seconds > 0.0 ? w9x_ticks_ms() + (unsigned long long)(seconds * 1000.0) : 0;
	}
	if (exit_ticks && w9x_ticks_ms() >= exit_ticks)
	{
		platform_log("exiting after debug.exit_after");
		exit(EXIT_SUCCESS);
	}
	if (w9x_pump() & W9X_PUMP_QUIT)
	{
		platform_log("window closed");
		exit(EXIT_SUCCESS);
	}
	pthread_mutex_lock(&input_lock);
	if (focus_regained)
	{
		focus_regained = FALSE;
		look_at_clipboard = TRUE;
	}
	pthread_mutex_unlock(&input_lock);
	looked_at_clipboard = TRUE;
	platform_invite_clipboard(look_at_clipboard);
}

/* ---------- the menus' pointer */

/* While a menu is up the mouse is released, its pointer shows (centered when
the menu opens) and its motion, clicks and wheel go to the menus
(halo_ui_pointer_update, d3d8_gl.c) instead of the controller and the aim. */
void platform_ui_pointer_set_active(BOOL active)
{
	if (!platform_window || (active != FALSE) == (input_state.ui_pointer != FALSE))
		return;
	pthread_mutex_lock(&input_lock);
	input_state.ui_pointer = active;
	memset(&ui_pointer, 0, sizeof(ui_pointer));
	ui_pointer_wheel = 0.0f;
	input_state.mouse_dx = 0.0f;
	input_state.mouse_dy = 0.0f;
	input_state.mouse_wheel = 0.0f;
	memset(input_state.mouse_buttons, 0, sizeof(input_state.mouse_buttons));
	pthread_mutex_unlock(&input_lock);
	platform_mouse_capture(!active && !input_state.mouse_released);
	if (active)
	{
		int width, height;

		w9x_video_size(&width, &height);
		w9x_mouse_warp(width / 2, height / 2);
		pthread_mutex_lock(&input_lock);
		ui_pointer.x = width * 0.5f;
		ui_pointer.y = height * 0.5f;
		pthread_mutex_unlock(&input_lock);
	}
}

/* what the pointer did since the last call; FALSE when it is not active */
BOOL platform_ui_pointer_read(struct platform_ui_pointer *pointer)
{
	BOOL active;

	pthread_mutex_lock(&input_lock);
	active = input_state.ui_pointer;
	*pointer = ui_pointer;
	ui_pointer.moved = FALSE;
	ui_pointer.left_clicks = 0;
	ui_pointer.right_clicks = 0;
	ui_pointer.wheel_steps = 0;
	pthread_mutex_unlock(&input_lock);
	return active;
}

void platform_input_read(struct platform_input_state *state, BOOL consume_motion)
{
	pthread_mutex_lock(&input_lock);
	*state = input_state;
	if (consume_motion)
	{
		int scancode;

		for (scancode = 0; scancode < SDL_SCANCODE_COUNT; scancode++)
		{
			state->keys[scancode] |= keys_pressed[scancode];
			keys_pressed[scancode] = 0;
		}
		input_state.mouse_dx = 0;
		input_state.mouse_dy = 0;
		input_state.mouse_wheel = 0;
	}
	pthread_mutex_unlock(&input_lock);
}
