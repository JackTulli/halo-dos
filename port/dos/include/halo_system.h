/*
HALO_SYSTEM.H

What each system gives the rest of the DOS and Windows 95 builds: time,
input, the display, the program's folder, messages. port/dos/dos
implements it over DOS and the hardware (VESA, the keyboard controller, the
mouse driver), port/dos/win95 over Win32. Only plain C types cross it, so
that every side can include it: the game's units and the Xbox-facing
platform layer (which see the Xbox SDK's headers), the renderer, and each
system's units (which see the Windows SDK's).
*/

#ifndef __HALO_SYSTEM_H
#define __HALO_SYSTEM_H

/* ---------- start-up and messages */

/* the system's part of the start-up, before the game's main runs anything
(the DOS build's clock and heap are ready earlier, in its start-up code) */
void system_initialize(void);
/* a line for the log (HALO.LOG beside the program) */
void system_log(const char *text);
/* tells the player something went wrong: a message box on Windows, the
text screen on DOS (the video mode is put back first) */
void system_error(const char *text);
/* the folder the program is in ("C:\GAMES\HALO"), no separator at the end */
const char *system_program_directory(void);
/* the settings' file beside the program (dos_settings.c), and the defaults
of its first version: the picture's size and colours, the frame rate */
const char *system_settings_file(void);
void system_default_settings(int *width, int *height, int *bits, int *fps_counter, int *sky_model);
/* the system's name and version for the log ("MS-DOS 7.10", "Windows 4.10") */
const char *system_description(void);
/* the program started again with its command line (the menu's APPLY AND
RESTART, the display closed first), this one ended: Windows starts the new
one itself; DOS ends with SYSTEM_RESTART_CODE, which PLAY.BAT (and the
DOSBox-X launcher) start it again on. Does not return */
#define SYSTEM_RESTART_CODE 42
void system_restart(void);

/* ---------- time */

unsigned long long system_microseconds(void);
/* waits about that long, giving the time to the system where there is one */
void system_sleep(unsigned long milliseconds);
/* the heap's bytes in all and not yet handed out (DOS: its DPMI block;
Windows: what the system says) */
unsigned long system_memory_total(void);
unsigned long system_memory_free(void);
/* testing (setting profile=1): a game frame is being drawn; the profilers
sample from the first, so loading stays out of their numbers */
void system_profile_frame(void);

/* ---------- input */

/* keys are the PC keyboard's scan codes (set 1); the extended keys
(arrows, the right control and alt, the keypad's enter...) are their code
plus 0x80 */
#define SYSTEM_KEY_COUNT 256

#define SYSTEM_KEY_ESCAPE 0x01
#define SYSTEM_KEY_1 0x02
#define SYSTEM_KEY_0 0x0B
#define SYSTEM_KEY_MINUS 0x0C
#define SYSTEM_KEY_EQUALS 0x0D
#define SYSTEM_KEY_BACKSPACE 0x0E
#define SYSTEM_KEY_TAB 0x0F
#define SYSTEM_KEY_Q 0x10
#define SYSTEM_KEY_W 0x11
#define SYSTEM_KEY_E 0x12
#define SYSTEM_KEY_R 0x13
#define SYSTEM_KEY_T 0x14
#define SYSTEM_KEY_Y 0x15
#define SYSTEM_KEY_U 0x16
#define SYSTEM_KEY_I 0x17
#define SYSTEM_KEY_O 0x18
#define SYSTEM_KEY_P 0x19
#define SYSTEM_KEY_ENTER 0x1C
#define SYSTEM_KEY_LEFT_CONTROL 0x1D
#define SYSTEM_KEY_A 0x1E
#define SYSTEM_KEY_S 0x1F
#define SYSTEM_KEY_D 0x20
#define SYSTEM_KEY_F 0x21
#define SYSTEM_KEY_G 0x22
#define SYSTEM_KEY_H 0x23
#define SYSTEM_KEY_J 0x24
#define SYSTEM_KEY_K 0x25
#define SYSTEM_KEY_L 0x26
#define SYSTEM_KEY_BACKQUOTE 0x29
#define SYSTEM_KEY_LEFT_SHIFT 0x2A
#define SYSTEM_KEY_Z 0x2C
#define SYSTEM_KEY_X 0x2D
#define SYSTEM_KEY_C 0x2E
#define SYSTEM_KEY_V 0x2F
#define SYSTEM_KEY_B 0x30
#define SYSTEM_KEY_N 0x31
#define SYSTEM_KEY_M 0x32
#define SYSTEM_KEY_RIGHT_SHIFT 0x36
#define SYSTEM_KEY_LEFT_ALT 0x38
#define SYSTEM_KEY_SPACE 0x39
#define SYSTEM_KEY_F1 0x3B
#define SYSTEM_KEY_F2 0x3C
#define SYSTEM_KEY_F10 0x44
#define SYSTEM_KEY_F11 0x57
#define SYSTEM_KEY_F12 0x58
#define SYSTEM_KEY_KEYPAD_ENTER (0x80 | 0x1C)
#define SYSTEM_KEY_RIGHT_CONTROL (0x80 | 0x1D)
#define SYSTEM_KEY_RIGHT_ALT (0x80 | 0x38)
#define SYSTEM_KEY_UP (0x80 | 0x48)
#define SYSTEM_KEY_LEFT (0x80 | 0x4B)
#define SYSTEM_KEY_RIGHT (0x80 | 0x4D)
#define SYSTEM_KEY_DOWN (0x80 | 0x50)
#define SYSTEM_KEY_DELETE (0x80 | 0x53)

#define SYSTEM_MOUSE_LEFT 0x01
#define SYSTEM_MOUSE_RIGHT 0x02
#define SYSTEM_MOUSE_MIDDLE 0x04

struct system_input
{
	/* nonzero while held */
	unsigned char keys[SYSTEM_KEY_COUNT];
	/* the mouse's motion since the last read, in its counts (mickeys);
	down and right positive */
	int mouse_dx, mouse_dy;
	/* the wheel's notches since the last read, forward positive */
	int mouse_wheel;
	unsigned char mouse_buttons;
	/* a mouse is there */
	unsigned char mouse_present;
	/* the player closed the window (Windows) */
	unsigned char quit;
	/* the game controllers there (Windows: XInput's, then the joysticks
	Windows knows; DOS: the game port's), in that order */
	int pad_count;
	struct system_pad
	{
		/* which controller: the same while it stays plugged in (its kind
		and number: system_pad_name says what it is) */
		unsigned short id;
		/* its buttons (SYSTEM_PAD_*), its sticks (-32768 to 32767, up and
		right positive, no dead zone taken off) and triggers (0 to 255) */
		unsigned short buttons;
		short left_x, left_y, right_x, right_y;
		unsigned char left_trigger, right_trigger;
	} pads[4];
};

#define SYSTEM_PADS 4
/* a controller's id's kind (its high byte; the low one its number) */
#define SYSTEM_PAD_XINPUT 0x100
#define SYSTEM_PAD_JOYSTICK 0x200
#define SYSTEM_PAD_GAME_PORT 0x300

/* a controller's buttons, as XInput numbers them (the bumpers are the
Xbox's white and black buttons) */
#define SYSTEM_PAD_UP 0x0001
#define SYSTEM_PAD_DOWN 0x0002
#define SYSTEM_PAD_LEFT 0x0004
#define SYSTEM_PAD_RIGHT 0x0008
#define SYSTEM_PAD_START 0x0010
#define SYSTEM_PAD_BACK 0x0020
#define SYSTEM_PAD_LEFT_THUMB 0x0040
#define SYSTEM_PAD_RIGHT_THUMB 0x0080
#define SYSTEM_PAD_LEFT_BUMPER 0x0100
#define SYSTEM_PAD_RIGHT_BUMPER 0x0200
#define SYSTEM_PAD_A 0x1000
#define SYSTEM_PAD_B 0x2000
#define SYSTEM_PAD_X 0x4000
#define SYSTEM_PAD_Y 0x8000

/* a controller's motors (its id), 0 to 65535 each (nothing where it has
none) */
void system_pad_rumble(unsigned short id, unsigned short left, unsigned short right);
/* what a controller is (its id), in capitals: "XBOX CONTROLLER 2", a
joystick's own name, "JOYSTICK A" */
const char *system_pad_name(unsigned short id);
/* the controllers for the menu: their names, "NONE", or "OFF" (the
setting controller = 0) */
const char *system_pad_description(void);

/* reads the keyboard and the mouse (on Windows, runs the window's messages
first) */
void system_input_read(struct system_input *input);
/* the menus' pointer: how far one count of the mouse's motion moves it on
their 640x480 screen, across and down (Windows: the window's pixels to the
menu's; DOS: the driver's mickeys, twice as many down as across) */
void system_mouse_menu_scale(float *x, float *y);

/* the next key going down or up, in the order they were pressed, with the
character it types (0 if none): for the console and the menus. FALSE when
there is none */
int system_next_keystroke(int *key, int *character, int *down);

/* ---------- the display */

enum system_pixel_format
{
	/* palette indices (system_display_palette) */
	_system_pixels_8,
	/* 0RRRRRGGGGGBBBBB */
	_system_pixels_15,
	/* RRRRRGGGGGGBBBBB */
	_system_pixels_16,
	/* B, G, R bytes */
	_system_pixels_24,
	/* B, G, R, unused bytes */
	_system_pixels_32,
};

/* the picture the renderer makes, to be shown */
struct system_frame
{
	/* 8-bit palette indices, or 32-bit pixels (0x00RRGGBB) */
	const void *pixels;
	int width, height;
	/* bytes from one row to the next */
	int pitch;
	int bits;
};

struct system_display
{
	int width, height;
	/* a pixel's width over its height as it is seen: a DOS mode fills a 4:3
	screen (320x200's are taller than wide, 5/6), a window's are square */
	float pixel_aspect;
	enum system_pixel_format format;
	/* the display's own refresh can be waited for */
	int vertical_blank;
	char description[64];
};

/* the modes the display can show at bits (8 or more), best first, up to
maximum; returns how many */
int system_display_modes(int bits, int *widths, int *heights, int maximum);
/* changes to a mode as close as there is to width x height at bits a pixel
(8, 16 or 32: 16 takes 15 where there is no 16, 32 takes 24); FALSE if none
works. The frame then shows scaled to fill it (whole multiples, centred) */
int system_display_open(int width, int height, int bits, struct system_display *display);
/* the frames from now each half as wide (wide) or as tall (tall) as the
display opened for, each pixel shown twice that way (DOUBLE WIDE and DOUBLE
TALL: a slow machine's half or quarter of the pixels); FALSE where a
graphics card draws (OpenGL, Direct3D, a Voodoo: at its own resolution)
and nothing changes */
int system_display_doubling(int wide, int tall);
void system_display_close(void);
/* the display's settings changed (the SETTINGS page's): Windows 95's
window or full screen, and its scaling, as they now say; nothing on DOS */
void system_display_settings_changed(void);
/* the colours of an 8-bit display: 256 red, green, blue triples, 0-255 */
void system_display_palette(const unsigned char *rgb);
/* shows a frame (converting its pixels to the display's) */
void system_display_present(const struct system_frame *frame);

/* ---------- sound */

/* opens the sound device for 16-bit stereo at about rate frames a second
(DOS: the Sound Blaster the BLASTER variable names; Windows 95: the wave
mapper): FALSE without one; the rate it took in *opened_rate. The setting
sound_record=<file> writes the mix to a .WAV file instead (testing) */
int system_audio_open(unsigned long rate, unsigned long *opened_rate);
/* keeps lead frames queued in the device ahead of what it plays (as many
as it holds at most), fill writing each run of frames (16-bit stereo,
interleaved) */
void system_audio_update(void (*fill)(short *samples, unsigned long frames), unsigned long lead);
/* stops it (at exit) */
void system_audio_close(void);

#endif /* __HALO_SYSTEM_H */
