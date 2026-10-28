/*
W9X_BACKEND.H

The boundary of the Windows 98/ME platform layer, which takes SDL3's place
there (SDL3 needs Windows Vista).

Two kinds of unit meet here and cannot share any other header. The
XDK-facing ones see the Xbox's headers through platform.h: w9x_platform.c
(in place of port/linux/src/sdl_platform.c), and xinput_sdl.c,
dsound_sdl.c, gl_functions.c, port_config.c, xbox_files.c and xbox_kernel.c
in their HALO_WIN9X branches. The Win32-facing ones see only the Windows
SDK, as port/windows/src/win32_*.c do: w9x_window.c (the window, display
modes, the message loop, the mouse), w9x_gl.c (WGL), w9x_input.c
(DirectInput and winmm gamepads), w9x_audio.c (DirectSound and waveOut) and
w9x_shell.c (message boxes, the file dialog, the progress window, the
clipboard, folders, the crash report). Between them only plain C types
pass, as through port/linux/src/posix.h. One more Windows-facing unit,
w9x_launcher.c (the settings window shown at start), has a boundary of its
own, port/linux/src/launcher.h, which the native Windows build shares:
port_config.c shows it when the settings are first read, before any of
the above exists.

DirectInput, DirectSound, OpenGL and the shell's special folders are loaded
at run time, so the game starts on a 98 SE or ME without them (or with a
DirectX older than the newest it can use) and says what is missing. Every
call into a driver that may change the processor's floating point control
words is followed by a check that puts back the game's (w9x_fpu_*): systems
playing together must compute the same results.
*/

#ifndef __HALO_WIN9X_BACKEND_H
#define __HALO_WIN9X_BACKEND_H

/* ---------- the window and OpenGL (w9x_window.c, w9x_gl.c) */

/* what the game asks for when it opens its window (w9x_platform.c fills
it in from the settings) */
struct w9x_video_request
{
	/* the window's client area when windowed: 640x480 times
	display.window_scale, made smaller to fit the screen */
	int width, height;
	/* fullscreen from the start (display.fullscreen), and the display mode
	to change to for it (display.mode): 0 by 0 keeps the desktop's, bits 0
	takes the best of 32, 24 and 16 bits a pixel */
	int fullscreen;
	int mode_width, mode_height, mode_bits;
	/* debug.hidden_window: never shown, never fullscreen */
	int hidden;
	/* the pixel format the renderer would like; formats with less are
	taken when nothing better exists (16-bit colour, 16-bit depth, no
	stencil or alpha) */
	int color_bits, alpha_bits, depth_bits, stencil_bits;
	/* display.allow_software_gl: Microsoft's own OpenGL (the GDI Generic
	renderer, no 3D hardware at all) is refused unless this is set */
	int allow_software;
};

enum w9x_gl_acceleration
{
	/* Microsoft's GDI Generic renderer: software only */
	_w9x_gl_software,
	/* a mini client driver (PFD_GENERIC_ACCELERATED): Microsoft's OpenGL
	with the display driver drawing some of it */
	_w9x_gl_mcd,
	/* an installable client driver: the display driver's own OpenGL */
	_w9x_gl_icd,
};

/* what the window and the context turned out to be */
struct w9x_video_result
{
	/* the window exists (it may still have no OpenGL) */
	int window;
	/* fullscreen now, and the display mode in use (the desktop's when the
	mode was left alone) */
	int fullscreen;
	int display_width, display_height, display_bits;
	/* a display mode change is in effect (undone at exit, while another
	program is in front and while a message box is up) */
	int mode_changed;

	/* an OpenGL context is current on the thread that opened the window */
	int gl;
	int pixel_format;
	int color_bits, red_bits, green_bits, blue_bits, alpha_bits;
	int depth_bits, stencil_bits;
	enum w9x_gl_acceleration acceleration;
	/* WGL_EXT_swap_control: display.vsync can be honoured */
	int swap_control;
	/* GL_VERSION as numbers (1.1 is 1, 1), and the strings, cut to fit */
	int version_major, version_minor;
	char vendor[64];
	char renderer[128];
	char version[64];
	/* GL_EXTENSIONS and the WGL extension string, as the driver keeps
	them (NULL when there is none); w9x_gl_extension asks them */
	const char *extensions;
	const char *wgl_extensions;
};

/* creates the window on this thread, changing the display mode first for
fullscreen, then the OpenGL context, which is made current here. FALSE if
not even the window could be made; with a window but no OpenGL, result->gl
is 0 and error says why */
int w9x_video_open(const struct w9x_video_request *request, struct w9x_video_result *result,
	char *error, int error_size);
/* the size fullscreen will have, or has: the planned display mode before
the window exists (request is then needed), the screen's afterwards; FALSE
when the game is (or is to be) in a window */
int w9x_video_screen_mode(const struct w9x_video_request *request, int *width, int *height);
/* the client area in pixels (its last size while minimised) */
void w9x_video_size(int *width, int *height);
void w9x_video_swap(void);
/* F11: between fullscreen and the window by the window's style alone (the
display mode stays as it is while the OpenGL context lives) */
void w9x_video_toggle_fullscreen(void);
/* any of OpenGL's entry points: wglGetProcAddress for what is above 1.1
and the extensions, opengl32.dll's own exports for 1.1 */
void *w9x_gl_proc_address(const char *name);
/* whether GL_EXTENSIONS or the WGL extensions list name, as a whole word */
int w9x_gl_extension(const char *name);
/* display.vsync, through WGL_EXT_swap_control; FALSE without it */
int w9x_gl_swap_interval(int interval);

/* ---------- events (w9x_window.c) */

/* runs the window's messages and reads the mouse and the gamepads; main
thread only. Calls the w9x_on_* functions below as things happen, and
returns W9X_PUMP_QUIT once the window has been closed */
#define W9X_PUMP_QUIT 1
int w9x_pump(void);
/* the game has the mouse: relative motion through DirectInput, else a
hidden cursor kept in the window's centre. Kept while the window is in
front, and taken again when it comes back */
void w9x_mouse_capture(int capture);
/* puts the cursor at a point of the client area */
void w9x_mouse_warp(int x, int y);

/* modifier bits of w9x_on_key: the debug keyboard's (XINPUT_DEBUG_KEYSTROKE
flags) */
#define W9X_KEY_CTRL 0x01
#define W9X_KEY_SHIFT 0x02
#define W9X_KEY_ALT 0x04
#define W9X_KEY_CAPS_LOCK 0x08
#define W9X_KEY_NUM_LOCK 0x10

/* Implemented by w9x_platform.c, called by w9x_pump (and by the window
procedure when Windows sends a message outside it). scancode is SDL's
(w9x_scancodes.h), 0 for keys it has none for; ascii is the character the
keyboard layout gives, 0 for none or for a key going up */
void w9x_on_key(int scancode, int ascii, int modifiers, int down, int repeat);
/* motion of the captured mouse, in its counts */
void w9x_on_mouse_motion(float dx, float dy);
/* the cursor over the client area, while it is not captured */
void w9x_on_pointer_motion(int x, int y);
/* an SDL_BUTTON_* going down or up, where the cursor is (in the client
area; the centre while captured) */
void w9x_on_mouse_button(int button, int down, int x, int y);
/* the wheel, in notches (forward is positive) */
void w9x_on_wheel(float notches);
/* the window came to the front, or went from it */
void w9x_on_focus(int focused);

/* ---------- gamepads (w9x_input.c) */

/* the Xbox controller's buttons, as input.gamepad_map assigns them */
#define W9X_PAD_A 0x0001
#define W9X_PAD_B 0x0002
#define W9X_PAD_X 0x0004
#define W9X_PAD_Y 0x0008
#define W9X_PAD_WHITE 0x0010
#define W9X_PAD_BLACK 0x0020
#define W9X_PAD_BACK 0x0040
#define W9X_PAD_START 0x0080
#define W9X_PAD_LEFT_THUMB 0x0100
#define W9X_PAD_RIGHT_THUMB 0x0200
#define W9X_PAD_DPAD_UP 0x0400
#define W9X_PAD_DPAD_DOWN 0x0800
#define W9X_PAD_DPAD_LEFT 0x1000
#define W9X_PAD_DPAD_RIGHT 0x2000

struct w9x_pad_state
{
	unsigned long buttons;
	/* -32768 to 32767, down and right positive (SDL's sense) */
	short left_x, left_y, right_x, right_y;
	/* 0 to 255 */
	unsigned char left_trigger, right_trigger;
};

/* input.gamepad_map: SDL's mapping syntax ("a:b0,b:b1,leftx:a0,
dpup:h0.1,lefttrigger:b6,..."), so a Windows line of SDL_GameControllerDB
can be pasted; empty for the dual-analog layout most pads of the time
have. Called before the window opens */
void w9x_input_configure(const char *mapping);
/* the attached gamepads, at most four (looked for when the window opens and
when Windows reports a device change, not every frame: DirectInput's
enumeration takes tens of milliseconds on 9x) */
int w9x_pad_count(void);
/* the state read at the last w9x_pump; FALSE if there is no such pad */
int w9x_pad_read(int index, struct w9x_pad_state *state);
/* the two motors, 0 to 65535 (force feedback pads, DirectX 5 or later;
others ignore it) */
void w9x_pad_rumble(int index, unsigned short left, unsigned short right);

/* ---------- sound (w9x_audio.c) */

/* opens the output: backend "dsound", "waveout" or "auto" (DirectSound,
else the wave mapper); rate 0 takes the first of 44100, 48000 and 22050
the device plays without resampling. *rate is what was opened: the
mixer's output rate. 16-bit stereo */
int w9x_audio_open(const char *backend, unsigned long rate, unsigned long *opened_rate);
/* starts the thread that keeps the device fed with fill's frames */
void w9x_audio_run(void (*fill)(short *samples, unsigned long frames));

/* ---------- the shell (w9x_shell.c) */

/* a question with Yes and No, or an error; any display mode change is
undone while the box is up */
int w9x_message_yes_no(const char *text);
void w9x_message_error(const char *text);
/* the open file dialog; FALSE if nothing was chosen */
int w9x_choose_file(const char *title, const char *filter_name, const char *patterns, char *path, int size);
/* a small window showing a line of text over a progress bar, for work
done before the game's window exists. Update runs its messages and returns
FALSE once the player has closed it */
void *w9x_progress_open(const char *title);
int w9x_progress_update(void *progress, const char *line, int permille);
void w9x_progress_close(void *progress);
/* CF_TEXT; get returns FALSE when there is no text that fits */
int w9x_clipboard_set(const char *text);
int w9x_clipboard_get(char *text, int size);
/* the executable's folder (backslashes, no separator at the end) */
int w9x_executable_directory(char *path, int size);
/* the per-user application data folder (shell32's CSIDL_APPDATA, through
SHGetSpecialFolderPathA where the shell has it: 98, ME, or 95 with
Internet Explorer 4's desktop) */
int w9x_application_data_directory(char *path, int size);
/* milliseconds since the system started, without timeGetTime's wrap after
49.7 days */
unsigned long long w9x_ticks_ms(void);

/* platform_log keeps its last lines here for the crash report */
void w9x_log_remember(const char *line);
/* undoes a display mode change (the crash path, and at exit) */
void w9x_display_restore(void);
/* the crash path (port/windows/src/win32_memory_watch.c): the desktop's
display mode back, then a message box with what happened and the last
lines of the log */
void w9x_crash_report(const char *what);

#endif
