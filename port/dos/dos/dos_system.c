/*
DOS_SYSTEM.C

The system for the DOS build (halo_system.h): the log and messages, time,
the keyboard and the mouse, and the display.

The keyboard is the controller's own scan codes (dos_keyboard.s takes the
interrupt): the keys held, and the order they went down and up in, for the
console. The mouse is the driver's (int 33h): its counts since the last
read. The display is VGA's mode 13h for 320x200 in 256 colours, else a
VESA mode with a linear frame buffer (VBE 2.0): the one of the wanted
colours closest to the wanted size, the picture shown in it at the largest
whole multiple that fits, centred, converted to its pixels as it is copied.
*/

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dos_dpmi.h"
#include "dos_kernel32.h"
#include "halo_system.h"

void dos_keyboard_isr(void);
extern char dos_keyboard_isr_end;
extern unsigned short dos_keyboard_data_selector;
extern volatile unsigned long dos_keyboard_head;
extern volatile unsigned char dos_keyboard_ring[256];

int system_argc;
char **system_argv;

/* ---------- messages */

void system_log(const char *text)
{
	dos_log_text(text);
}

static char program_directory[260];

/* (its own file: the DOS build's picture is not the Windows 95 build's) */
const char *system_settings_file(void)
{
	return "HALODOS.CFG";
}

/* 320x200 in 256 colours, the frame rate shown, the skies a gradient: an
emulated or real Pentium's */
void system_default_settings(int *width, int *height, int *bits, int *fps_counter, int *sky_model)
{
	*width = 320;
	*height = 200;
	*bits = 8;
	*fps_counter = 1;
	*sky_model = 0;
}

const char *system_program_directory(void)
{
	if (!program_directory[0])
	{
		char *slash;

		strncpy(program_directory, dos_program_path(), sizeof(program_directory) - 1);
		slash = strrchr(program_directory, '\\');
		if (slash)
			*slash = 0;
		else
			strcpy(program_directory, ".");
	}
	return program_directory;
}

const char *system_description(void)
{
	static char text[64];
	struct dos_registers r;

	memset(&r, 0, sizeof(r));
	r.eax = 0x3000;
	dos_real_interrupt(0x21, &r);
	snprintf(text, sizeof(text), "MS-DOS %lu.%02lu, DOS/32A, %lu MHz", r.eax & 0xFF, (r.eax >> 8) & 0xFF,
		(unsigned long)(dos_tsc_frequency / 1000000));
	return text;
}

void system_error(const char *text)
{
	char line[600];

	/* (the video mode back first: the message is to be read) */
	snprintf(line, sizeof(line), "\r\nHalo: %s", text);
	dos_exit_with_message(line);
}

/* ---------- time */

unsigned long long system_microseconds(void)
{
	return dos_microseconds();
}

void system_sleep(unsigned long milliseconds)
{
	unsigned long long until = dos_microseconds() + (unsigned long long)milliseconds * 1000;

	while (dos_microseconds() < until)
		;
}

/* ---------- the keyboard */

static unsigned char keys[SYSTEM_KEY_COUNT];
static unsigned long keyboard_tail;
static int keyboard_extended;
static unsigned short old_keyboard_selector;
static unsigned long old_keyboard_offset;
static int keyboard_installed;

/* key events for the console, as they came */
#define EVENT_COUNT 64
static struct
{
	unsigned char key, down;
} events[EVENT_COUNT];
static int event_head, event_tail;

/* the US layout's characters, by scan code, without and with shift */
static const char ascii_plain[0x3A] =
	"\0\0331234567890-=\b\tqwertyuiop[]\r\0asdfghjkl;'`\0\\zxcvbnm,./\0*\0 ";
static const char ascii_shift[0x3A] =
	"\0\033!@#$%^&*()_+\b\tQWERTYUIOP{}\r\0ASDFGHJKL:\"~\0|ZXCVBNM<>?\0*\0 ";

static void keyboard_restore(void)
{
	if (keyboard_installed)
	{
		dos_set_interrupt_vector(9, old_keyboard_selector, old_keyboard_offset);
		keyboard_installed = FALSE;
	}
}

static void keyboard_install(void)
{
	dos_keyboard_data_selector = dos_data_selector();
	dos_dpmi_lock((const void *)dos_keyboard_isr, (unsigned long)(&dos_keyboard_isr_end - (char *)dos_keyboard_isr));
	dos_dpmi_lock((const void *)dos_keyboard_ring, sizeof(dos_keyboard_ring));
	dos_dpmi_lock((const void *)&dos_keyboard_head, sizeof(dos_keyboard_head));
	dos_get_interrupt_vector(9, &old_keyboard_selector, &old_keyboard_offset);
	dos_set_interrupt_vector(9, dos_code_selector(), (unsigned long)dos_keyboard_isr);
	keyboard_installed = TRUE;
	dos_at_exit(keyboard_restore);
}

/* the scan codes the interrupt has kept, into the keys and the events */
static void keyboard_read(void)
{
	while (keyboard_tail != dos_keyboard_head)
	{
		unsigned char code = dos_keyboard_ring[keyboard_tail];
		int key, down;

		keyboard_tail = (keyboard_tail + 1) & 255;
		if (code == 0xE0)
		{
			keyboard_extended = TRUE;
			continue;
		}
		if (code == 0xE1)
		{
			/* (pause: its five codes that follow are not keys) */
			continue;
		}
		key = (code & 0x7F) | (keyboard_extended ? 0x80 : 0);
		down = !(code & 0x80);
		keyboard_extended = FALSE;
		/* (the extended shifts that come with some keys) */
		if (key == (0x80 | 0x2A) || key == (0x80 | 0x36))
			continue;
		if (down && keys[key])
			continue;
		keys[key] = (unsigned char)down;
		if ((event_head + 1) % EVENT_COUNT != event_tail)
		{
			events[event_head].key = (unsigned char)key;
			events[event_head].down = (unsigned char)down;
			event_head = (event_head + 1) % EVENT_COUNT;
		}
	}
}

int system_next_keystroke(int *key, int *character, int *down)
{
	int shift;

	keyboard_read();
	if (event_tail == event_head)
		return FALSE;
	*key = events[event_tail].key;
	*down = events[event_tail].down;
	event_tail = (event_tail + 1) % EVENT_COUNT;
	shift = keys[SYSTEM_KEY_LEFT_SHIFT] || keys[SYSTEM_KEY_RIGHT_SHIFT];
	*character = *key < 0x3A ? (shift ? ascii_shift : ascii_plain)[*key] : 0;
	if (*key == SYSTEM_KEY_KEYPAD_ENTER)
		*character = '\r';
	return TRUE;
}

/* ---------- the mouse */

static int mouse_present = -1;

static void mouse_initialize(void)
{
	struct dos_registers r;

	memset(&r, 0, sizeof(r));
	r.eax = 0x0000;
	dos_real_interrupt(0x33, &r);
	mouse_present = (r.eax & 0xFFFF) == 0xFFFF;
	if (mouse_present)
	{
		/* (counts since the last read, from now) */
		memset(&r, 0, sizeof(r));
		r.eax = 0x000B;
		dos_real_interrupt(0x33, &r);
	}
}

/* ---------- the game port's joysticks (port 201h)

The port's four axes are one-shot timers: a write starts them, and each
axis's bit stays 1 for as long as its resistance says (the stick's place),
counted here in reads of the port (how many a millisecond is depends on the
machine: each axis's middle is its first count, its range grows with what
it has seen). Its buttons are its high bits, 0 while held. Joystick A is
axes 0 and 1 and buttons 4 and 5, joystick B the others (a Y cable's two
sticks), each its own controller (split screen's players); with the
setting game_port = 4axis they are one pad of four axes and four buttons
(a gamepad or a flight stick that uses all of the port). A stick of two
axes and two buttons plays as the old DOS games: forward and back, turning
left and right, fire and jump; four buttons add reload and the weapon's
switch. A joystick is there when its axes' bits fall within the count;
looked for every two seconds while none is (the setting controller = 0:
never). */

#define GAME_PORT 0x201
#define GAME_PORT_LIMIT 20000

static struct
{
	int initialized, enabled, four_axes;
	/* which sticks answer (bit 0 A, bit 1 B) */
	int present;
	unsigned long next_look;
	/* each axis's middle, least and most count */
	long middle[4], low[4], high[4];
	int calibrated[4];
	char description[64];
} game_port;

/* each axis's count; the reads it took */
static long game_port_counts(long counts[4])
{
	long n = 0;

	counts[0] = counts[1] = counts[2] = counts[3] = 0;
	dos_outb(GAME_PORT, 0xFF);
	while (n < GAME_PORT_LIMIT)
	{
		unsigned char bits = (unsigned char)(dos_inb(GAME_PORT) & 0x0F);

		if (!bits)
			break;
		for (int k = 0; k < 4; k++)
		{
			if (bits & (1 << k))
				counts[k]++;
		}
		n++;
	}
	return n;
}

static short game_port_axis(int k, long count)
{
	long offset;

	if (!game_port.calibrated[k])
	{
		game_port.middle[k] = game_port.low[k] = game_port.high[k] = count;
		game_port.calibrated[k] = TRUE;
	}
	if (count < game_port.low[k])
		game_port.low[k] = count;
	if (count > game_port.high[k])
		game_port.high[k] = count;
	offset = count - game_port.middle[k];
	if (offset < 0 && game_port.middle[k] - game_port.low[k] > 4)
		return (short)(offset * 32767 / (game_port.middle[k] - game_port.low[k]));
	if (offset > 0 && game_port.high[k] - game_port.middle[k] > 4)
		return (short)(offset * 32767 / (game_port.high[k] - game_port.middle[k]));
	return 0;
}

static void game_port_read(struct system_input *input)
{
	unsigned long now = (unsigned long)(system_microseconds() / 1000ULL);
	long counts[4];
	unsigned char buttons;

	if (!game_port.initialized)
	{
		extern long dos_setting_integer(const char *name, long default_value);
		extern const char *dos_setting(const char *name, const char *default_value);

		game_port.initialized = TRUE;
		game_port.enabled = dos_setting_integer("controller", 1) != 0;
		game_port.four_axes = !_stricmp(dos_setting("game_port", "auto"), "4axis");
		strcpy(game_port.description, "NONE");
	}
	if (!game_port.enabled || (!game_port.present && (long)(now - game_port.next_look) < 0))
		return;
	game_port_counts(counts);
	buttons = (unsigned char)~dos_inb(GAME_PORT);
	{
		int present = 0;

		/* (a stick's two axes both timed out within the count) */
		if (counts[0] > 0 && counts[0] < GAME_PORT_LIMIT && counts[1] > 0 && counts[1] < GAME_PORT_LIMIT)
			present |= 1;
		if (counts[2] > 0 && counts[2] < GAME_PORT_LIMIT && counts[3] > 0 && counts[3] < GAME_PORT_LIMIT)
			present |= 2;
		if (present != game_port.present)
		{
			char line[96];

			game_port.present = present;
			snprintf(game_port.description, sizeof(game_port.description), "%s",
				!present ? "NONE" : game_port.four_axes ? "GAME PORT PAD" : present == 3 ? "JOYSTICKS A AND B" :
				present == 1 ? "JOYSTICK A" : "JOYSTICK B");
			snprintf(line, sizeof(line), "controller: %s on the game port", game_port.description);
			system_log(line);
		}
		if (!present)
		{
			game_port.next_look = now + 2000;
			return;
		}
	}
	if (game_port.four_axes)
	{
		struct system_pad *pad = &input->pads[input->pad_count++];

		memset(pad, 0, sizeof(*pad));
		pad->id = SYSTEM_PAD_GAME_PORT;
		pad->left_x = game_port_axis(0, counts[0]);
		pad->left_y = (short)-game_port_axis(1, counts[1]);
		pad->right_x = game_port_axis(2, counts[2]);
		pad->right_y = (short)-game_port_axis(3, counts[3]);
		if (buttons & 0x10) pad->right_trigger = 255;
		if (buttons & 0x20) pad->buttons |= SYSTEM_PAD_A;
		if (buttons & 0x40) pad->buttons |= SYSTEM_PAD_X;
		if (buttons & 0x80) pad->buttons |= SYSTEM_PAD_Y;
		return;
	}
	for (int stick = 0; stick < 2 && input->pad_count < SYSTEM_PADS; stick++)
	{
		struct system_pad *pad;
		unsigned char fire = (unsigned char)(stick ? 0x40 : 0x10), jump = (unsigned char)(stick ? 0x80 : 0x20);

		if (!(game_port.present & (1 << stick)))
			continue;
		pad = &input->pads[input->pad_count++];
		memset(pad, 0, sizeof(*pad));
		pad->id = (unsigned short)(SYSTEM_PAD_GAME_PORT | stick);
		/* (forward and back on the left stick, turning on the right) */
		pad->left_y = (short)-game_port_axis(stick * 2 + 1, counts[stick * 2 + 1]);
		pad->right_x = game_port_axis(stick * 2, counts[stick * 2]);
		if (buttons & fire) pad->right_trigger = 255;
		if (buttons & jump) pad->buttons |= SYSTEM_PAD_A;
		/* (a lone stick's other two buttons: reload and the weapon) */
		if (game_port.present == 1)
		{
			if (buttons & 0x40) pad->buttons |= SYSTEM_PAD_X;
			if (buttons & 0x80) pad->buttons |= SYSTEM_PAD_Y;
		}
	}
}

void system_pad_rumble(unsigned short id, unsigned short left, unsigned short right)
{
	(void)id;
	(void)left;
	(void)right;
}

const char *system_pad_name(unsigned short id)
{
	if (id == SYSTEM_PAD_GAME_PORT && game_port.four_axes)
		return "GAME PORT PAD";
	if (id == SYSTEM_PAD_GAME_PORT)
		return "JOYSTICK A";
	if (id == (SYSTEM_PAD_GAME_PORT | 1))
		return "JOYSTICK B";
	return "NONE";
}

const char *system_pad_description(void)
{
	if (game_port.initialized && !game_port.enabled)
		return "OFF";
	return game_port.initialized ? game_port.description : "NONE";
}

void system_input_read(struct system_input *input)
{
	struct dos_registers r;

	keyboard_read();
	memcpy(input->keys, keys, sizeof(input->keys));
	input->mouse_dx = input->mouse_dy = input->mouse_wheel = 0;
	input->mouse_buttons = 0;
	input->quit = 0;
	if (mouse_present < 0)
		mouse_initialize();
	input->mouse_present = (unsigned char)mouse_present;
	if (mouse_present)
	{
		memset(&r, 0, sizeof(r));
		r.eax = 0x000B;
		dos_real_interrupt(0x33, &r);
		input->mouse_dx = (short)(r.ecx & 0xFFFF);
		input->mouse_dy = (short)(r.edx & 0xFFFF);
		memset(&r, 0, sizeof(r));
		r.eax = 0x0003;
		dos_real_interrupt(0x33, &r);
		input->mouse_buttons = (unsigned char)(r.ebx & 7);
		/* (CuteMouse's wheel: the count in BH) */
		input->mouse_wheel = -(signed char)((r.ebx >> 8) & 0xFF);
	}
	input->pad_count = 0;
	memset(input->pads, 0, sizeof(input->pads));
	game_port_read(input);
}

/* ---------- the display */

#pragma pack(push, 1)
struct vbe_controller
{
	char signature[4];
	unsigned short version;
	unsigned long oem_string;
	unsigned long capabilities;
	unsigned long mode_list;
	unsigned short total_memory;
	unsigned char rest[492];
};

struct vbe_mode
{
	unsigned short attributes;
	unsigned char window_a, window_b;
	unsigned short granularity, window_size, segment_a, segment_b;
	unsigned long window_function;
	unsigned short bytes_per_line;
	unsigned short width, height;
	unsigned char character_width, character_height, planes, bits, banks, memory_model, bank_size, image_pages;
	unsigned char reserved1;
	unsigned char red_size, red_position, green_size, green_position, blue_size, blue_position, reserved_size,
		reserved_position, direct_color_attributes;
	unsigned long physical_base;
	unsigned long reserved2;
	unsigned short reserved3;
	unsigned char rest[206];
};
#pragma pack(pop)

typedef char vbe_mode_size_assert[sizeof(struct vbe_mode) == 256 ? 1 : -1];

static struct
{
	int open;
	int vga;
	unsigned short mode;
	unsigned char *frame_buffer;
	int width, height, pitch, bytes;
	enum system_pixel_format format;
	/* the picture shown at a whole multiple, centred; its pixels each twice
	more across or down (DOUBLE WIDE, DOUBLE TALL: system_display_doubling) */
	int scale, left, top;
	int scale_x, scale_y;
	unsigned char *row;
	unsigned long palette[256];
} screen;

static int vbe_mode_information(unsigned short mode, struct vbe_mode *information)
{
	struct dos_registers r;

	memset(dos_transfer, 0, 256);
	memset(&r, 0, sizeof(r));
	r.eax = 0x4F01;
	r.ecx = mode;
	r.es = dos_transfer_segment;
	r.edi = 0;
	if (!dos_real_interrupt(0x10, &r) || (r.eax & 0xFFFF) != 0x004F)
		return FALSE;
	memcpy(information, dos_transfer, sizeof(*information));
	return TRUE;
}

static enum system_pixel_format format_of(const struct vbe_mode *mode)
{
	switch (mode->bits)
	{
	case 8: return _system_pixels_8;
	case 15: return _system_pixels_15;
	case 16: return mode->green_size == 5 ? _system_pixels_15 : _system_pixels_16;
	case 24: return _system_pixels_24;
	default: return _system_pixels_32;
	}
}

/* the VESA modes with a linear frame buffer: their numbers and information */
#define MAXIMUM_MODES 128
static unsigned short mode_numbers[MAXIMUM_MODES];
static struct vbe_mode mode_information[MAXIMUM_MODES];
static int mode_count = -1;

static void list_modes(void)
{
	struct dos_registers r;
	struct vbe_controller controller;
	unsigned short numbers[MAXIMUM_MODES];
	int count = 0;

	mode_count = 0;
	memset(dos_transfer, 0, 512);
	memcpy(dos_transfer, "VBE2", 4);
	memset(&r, 0, sizeof(r));
	r.eax = 0x4F00;
	r.es = dos_transfer_segment;
	r.edi = 0;
	if (!dos_real_interrupt(0x10, &r) || (r.eax & 0xFFFF) != 0x004F)
		return;
	memcpy(&controller, dos_transfer, sizeof(controller));
	if (memcmp(controller.signature, "VESA", 4))
		return;
	{
		const unsigned short *list = dos_real_pointer((unsigned short)(controller.mode_list >> 16),
			(unsigned short)(controller.mode_list & 0xFFFF));

		while (count < MAXIMUM_MODES && list[count] != 0xFFFF)
		{
			numbers[count] = list[count];
			count++;
		}
	}
	for (int i = 0; i < count; i++)
	{
		struct vbe_mode information;

		/* supported, graphics, with a linear frame buffer, packed or direct */
		if (!vbe_mode_information(numbers[i], &information) || (information.attributes & 0x91) != 0x91 ||
			(information.memory_model != 4 && information.memory_model != 6) || !information.physical_base)
			continue;
		if (information.bits != 8 && information.bits != 15 && information.bits != 16 && information.bits != 24 &&
			information.bits != 32)
			continue;
		mode_numbers[mode_count] = numbers[i];
		mode_information[mode_count] = information;
		mode_count++;
	}
}

static int bits_match(int wanted, enum system_pixel_format format)
{
	switch (wanted)
	{
	case 8: return format == _system_pixels_8;
	case 16: return format == _system_pixels_16 || format == _system_pixels_15;
	default: return format == _system_pixels_32 || format == _system_pixels_24;
	}
}

int system_display_modes(int bits, int *widths, int *heights, int maximum)
{
	int found = 0;

	if (mode_count < 0)
		list_modes();
	for (int i = 0; i < mode_count && found < maximum; i++)
	{
		if (bits_match(bits, format_of(&mode_information[i])))
		{
			widths[found] = mode_information[i].width;
			heights[found] = mode_information[i].height;
			found++;
		}
	}
	return found;
}

static void text_mode(void)
{
	struct dos_registers r;

	memset(&r, 0, sizeof(r));
	r.eax = 0x0003;
	dos_real_interrupt(0x10, &r);
}

/* the 3dfx Voodoo (dos_voodoo.c) */
int dos_voodoo_open(void);
int dos_voodoo_active(void);
void dos_voodoo_present(void);
void dos_voodoo_close(void);

/* the menus' pointer (halo_system.h): the driver's mickeys, its default
8 across and 16 down for 8 pixels */
void system_mouse_menu_scale(float *x, float *y)
{
	*x = 1.0f;
	*y = 0.5f;
}

/* the menu's APPLY AND RESTART: DOS cannot start a program from one about
to end, so this one ends with SYSTEM_RESTART_CODE and PLAY.BAT (or the
DOSBox-X launcher) starts it again */
void system_restart(void)
{
	system_log("restart: ending with the code PLAY.BAT starts Halo again on");
	printf("Halo starts again with the new settings (PLAY.BAT does it; else type HALO).\n");
	exit(SYSTEM_RESTART_CODE);
}

void system_display_close(void)
{
	if (dos_voodoo_active())
	{
		dos_voodoo_close();
		screen.open = FALSE;
		dos_console_set_visible(TRUE);
		return;
	}
	if (screen.open)
	{
		text_mode();
		free(screen.row);
		screen.row = NULL;
		screen.open = FALSE;
		dos_console_set_visible(TRUE);
	}
}

/* (DOS's picture is always the whole screen: its settings change nothing
while the game runs) */
void system_display_settings_changed(void)
{
}

int system_display_open(int width, int height, int bits, struct system_display *display)
{
	struct dos_registers r;
	int best = -1;
	long best_score = 0x7FFFFFFF;

	system_display_close();
	memset(&screen, 0, sizeof(screen));
	{
		/* (renderer = voodoo: the 3dfx card draws and shows everything, in
		millions of colours whatever bits says; else the VGA or VESA mode) */
		const char *dos_setting(const char *name, const char *default_value);
		const char *renderer = dos_setting("renderer", "software");

		if (renderer && (!_stricmp(renderer, "voodoo") || !_stricmp(renderer, "glide") || !_stricmp(renderer, "3dfx")))
		{
			if (dos_voodoo_open())
			{
				dos_console_set_visible(FALSE);
				display->width = width;
				display->height = height;
				display->pixel_aspect = (4.0f / 3.0f) / ((float)width / (float)height);
				display->format = _system_pixels_32;
				display->vertical_blank = FALSE;
				snprintf(display->description, sizeof(display->description), "3dfx Voodoo Graphics, 640x480");
				return TRUE;
			}
			system_log("voodoo: none to draw with: the software renderer");
		}
	}
	if (bits == 8 && width == 320 && height == 200)
	{
		/* VGA's own 256 colours */
		memset(&r, 0, sizeof(r));
		r.eax = 0x0013;
		dos_real_interrupt(0x10, &r);
		screen.vga = TRUE;
		screen.frame_buffer = dos_pointer(0xA0000);
		screen.width = 320;
		screen.height = 200;
		screen.pitch = 320;
		screen.bytes = 1;
		screen.format = _system_pixels_8;
	}
	else
	{
		if (mode_count < 0)
			list_modes();
		/* the mode of these colours that fits the picture the most times with
		the least left over, or else the largest */
		for (int i = 0; i < mode_count; i++)
		{
			const struct vbe_mode *mode = &mode_information[i];
			int scale_x = mode->width / width, scale_y = mode->height / height;
			int scale = scale_x < scale_y ? scale_x : scale_y;
			long score;

			if (!bits_match(bits, format_of(mode)))
				continue;
			if (scale < 1)
				score = 1000000L + (long)width * height - (long)mode->width * mode->height;
			else
				score = (long)mode->width * mode->height - (long)width * scale * height * scale +
					(scale > 1 ? 1000 : 0);
			/* (16 bits rather than 15, 32 rather than 24) */
			if (format_of(mode) == _system_pixels_15 || format_of(mode) == _system_pixels_24)
				score += 10;
			if (score < best_score)
			{
				best_score = score;
				best = i;
			}
		}
		if (best < 0)
			return FALSE;
		{
			const struct vbe_mode *mode = &mode_information[best];
			unsigned long size = (unsigned long)mode->bytes_per_line * mode->height;
			unsigned long linear = dos_dpmi_map_physical(mode->physical_base, size);

			if (!linear)
				return FALSE;
			memset(&r, 0, sizeof(r));
			r.eax = 0x4F02;
			r.ebx = mode_numbers[best] | 0x4000;
			if (!dos_real_interrupt(0x10, &r) || (r.eax & 0xFFFF) != 0x004F)
				return FALSE;
			screen.mode = mode_numbers[best];
			screen.frame_buffer = dos_pointer(linear);
			screen.width = mode->width;
			screen.height = mode->height;
			screen.pitch = mode->bytes_per_line;
			screen.format = format_of(mode);
			screen.bytes = (mode->bits + 7) / 8;
		}
	}
	screen.scale = screen.width / width < screen.height / height ? screen.width / width : screen.height / height;
	if (screen.scale < 1)
		screen.scale = 1;
	screen.scale_x = screen.scale_y = screen.scale;
	screen.left = (screen.width - width * screen.scale) / 2;
	screen.top = (screen.height - height * screen.scale) / 2;
	if (screen.left < 0)
		screen.left = 0;
	if (screen.top < 0)
		screen.top = 0;
	screen.row = malloc((size_t)screen.width * 4);
	if (!screen.row)
		return FALSE;
	screen.open = TRUE;
	dos_at_exit(system_display_close);
	dos_console_set_visible(FALSE);
	for (int y = 0; y < screen.height; y++)
		memset(screen.frame_buffer + (size_t)y * screen.pitch, 0, (size_t)screen.width * screen.bytes);
	display->width = screen.width;
	display->height = screen.height;
	display->pixel_aspect = (4.0f / 3.0f) / ((float)screen.width / (float)screen.height);
	display->format = screen.format;
	display->vertical_blank = TRUE;
	snprintf(display->description, sizeof(display->description), screen.vga ? "VGA mode 13h" :
		"VESA mode %03Xh, %d bits, picture x%d", screen.mode, screen.bytes * 8, screen.scale);
	return TRUE;
}

int system_display_doubling(int wide, int tall)
{
	if (dos_voodoo_active() || !screen.open)
		return FALSE;
	screen.scale_x = screen.scale * (wide ? 2 : 1);
	screen.scale_y = screen.scale * (tall ? 2 : 1);
	return TRUE;
}

void system_display_palette(const unsigned char *rgb)
{
	/* (the DAC takes 6 bits a colour) */
	dos_outb(0x3C8, 0);
	for (int i = 0; i < 256; i++)
	{
		dos_outb(0x3C9, (unsigned char)(rgb[i * 3 + 0] >> 2));
		dos_outb(0x3C9, (unsigned char)(rgb[i * 3 + 1] >> 2));
		dos_outb(0x3C9, (unsigned char)(rgb[i * 3 + 2] >> 2));
		screen.palette[i] = ((unsigned long)rgb[i * 3] << 16) | ((unsigned long)rgb[i * 3 + 1] << 8) | rgb[i * 3 + 2];
	}
}

/* a row's first columns of the picture in the display's pixels, each scale
times across */
static void convert_row(const struct system_frame *frame, int y, int columns, unsigned char *out)
{
	const unsigned char *in = (const unsigned char *)frame->pixels + (size_t)y * frame->pitch;
	int scale = screen.scale_x;

	switch (screen.format)
	{
	case _system_pixels_8:
		for (int x = 0; x < columns; x++)
		{
			for (int k = 0; k < scale; k++)
				*out++ = in[x];
		}
		break;
	case _system_pixels_15:
	case _system_pixels_16:
	{
		unsigned short *words = (unsigned short *)out;
		const unsigned long *pixels = (const unsigned long *)in;

		for (int x = 0; x < columns; x++)
		{
			unsigned long c = frame->bits == 8 ? screen.palette[in[x]] : pixels[x];
			unsigned short packed = screen.format == _system_pixels_16 ?
				(unsigned short)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F)) :
				(unsigned short)(((c >> 9) & 0x7C00) | ((c >> 6) & 0x03E0) | ((c >> 3) & 0x001F));

			for (int k = 0; k < scale; k++)
				*words++ = packed;
		}
		break;
	}
	case _system_pixels_24:
	{
		const unsigned long *pixels = (const unsigned long *)in;

		for (int x = 0; x < columns; x++)
		{
			unsigned long c = frame->bits == 8 ? screen.palette[in[x]] : pixels[x];

			for (int k = 0; k < scale; k++)
			{
				*out++ = (unsigned char)c;
				*out++ = (unsigned char)(c >> 8);
				*out++ = (unsigned char)(c >> 16);
			}
		}
		break;
	}
	default:
	{
		unsigned long *longs = (unsigned long *)out;
		const unsigned long *pixels = (const unsigned long *)in;

		for (int x = 0; x < columns; x++)
		{
			unsigned long c = frame->bits == 8 ? screen.palette[in[x]] : pixels[x];

			for (int k = 0; k < scale; k++)
				*longs++ = c;
		}
		break;
	}
	}
}

void system_display_present(const struct system_frame *frame)
{
	int rows = frame->height, columns = frame->width, row_bytes;

	if (dos_voodoo_active())
	{
		dos_voodoo_present();
		return;
	}
	if (!screen.open)
		return;
	if (screen.top + rows * screen.scale_y > screen.height)
		rows = (screen.height - screen.top) / screen.scale_y;
	/* (a picture larger than the largest mode of its colours: its top left,
	as much as the screen and the converted row hold; converted whole, a
	32-bit picture 1600 wide overran the row of a 1280 mode) */
	if (screen.left + columns * screen.scale_x > screen.width)
		columns = (screen.width - screen.left) / screen.scale_x;
	row_bytes = columns * screen.scale_x * screen.bytes;
	if (screen.left * screen.bytes + row_bytes > screen.pitch)
		row_bytes = screen.pitch - screen.left * screen.bytes;
	if (screen.format == _system_pixels_8 && screen.scale_x <= 2 && frame->bits == 8)
	{
		/* (the picture as the screen holds it: each row straight into the
		frame buffer, a word at a time, once for each line it takes; a
		doubled row's pixels paired first, four at a time into two longs) */
		for (int y = 0; y < rows; y++)
		{
			const unsigned char *in = (const unsigned char *)frame->pixels + (size_t)y * frame->pitch;
			const unsigned char *source = in;

			if (screen.scale_x == 2)
			{
				unsigned long *out = (unsigned long *)screen.row;
				int x = 0;

				for (; x + 4 <= columns; x += 4)
				{
					unsigned long a = in[x], b = in[x + 1], c = in[x + 2], d = in[x + 3];

					out[0] = a | a << 8 | b << 16 | b << 24;
					out[1] = c | c << 8 | d << 16 | d << 24;
					out += 2;
				}
				for (; x < columns; x++)
					screen.row[2 * x] = screen.row[2 * x + 1] = in[x];
				source = screen.row;
			}
			for (int k = 0; k < screen.scale_y; k++)
			{
				memcpy(screen.frame_buffer + (size_t)(screen.top + y * screen.scale_y + k) * screen.pitch +
					(size_t)screen.left, source, (size_t)row_bytes);
			}
		}
		return;
	}
	for (int y = 0; y < rows; y++)
	{
		convert_row(frame, y, columns, screen.row);
		for (int k = 0; k < screen.scale_y; k++)
		{
			memcpy(screen.frame_buffer + (size_t)(screen.top + y * screen.scale_y + k) * screen.pitch +
				(size_t)screen.left * screen.bytes, screen.row, (size_t)row_bytes);
		}
	}
}

/* ---------- start-up */

void dos_profile_start(void);

void system_initialize(void)
{
	char line[128];

	keyboard_install();
	snprintf(line, sizeof(line), "Halo for DOS: %s", system_description());
	system_log(line);
	dos_profile_start();
}


