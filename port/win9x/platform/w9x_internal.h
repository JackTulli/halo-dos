/*
W9X_INTERNAL.H

What the Win32-facing units of the Windows 98/ME platform layer (w9x_*.c
but w9x_platform.c) share among themselves, in Windows' own types; the
rest of the port sees only w9x_backend.h.

These units are built like port/windows/src/win32_*.c, against the Windows
SDK, at WINVER 0x0410 and _WIN32_WINDOWS 0x0410 so that structure sizes are
98's. DirectInput, DirectSound, OpenGL and shell32's newer functions are
never imported: they are looked up with GetProcAddress, and what they
create is reached through COM's vtables.
*/

#ifndef __HALO_WIN9X_INTERNAL_H
#define __HALO_WIN9X_INTERNAL_H

#include <windows.h>

#include "w9x_backend.h"

void platform_log(const char *format, ...);

/* ---------- the processor's floating point state (w9x_shell.c)

The game sets the x87 control word (53-bit precision, real_math.c) and the
compiler's SSE code runs under MXCSR. Display, sound and input drivers of
the time were known to change either (single precision for speed, or
unmasked exceptions), and a machine whose float results differ falls out of
step with the others in a network game. Each driver call that may load or
run driver code is bracketed: the state before it is saved, and if the call
changed it, it is put back (and that is logged, once for each call). */

struct w9x_fpu
{
	unsigned short control;
	unsigned long mxcsr;
};

void w9x_fpu_save(struct w9x_fpu *state);
/* call names what ran since the save, for the log; NULL puts state in
place without a word (a new thread taking its creator's) */
void w9x_fpu_check(const struct w9x_fpu *state, const char *call);

/* ---------- the window (w9x_window.c) */

HWND w9x_window(void);
HINSTANCE w9x_instance(void);
/* puts the desktop's display mode back while a message box is up (and
the game's window out of the way), and the game's mode afterwards */
void w9x_display_suspend(void);
void w9x_display_resume(void);

/* ---------- OpenGL (w9x_gl.c) */

/* opengl32.dll, the pixel format, the context; result's OpenGL fields */
int w9x_gl_open(HWND window, HDC dc, const struct w9x_video_request *request, struct w9x_video_result *result,
	char *error, int error_size);
void w9x_gl_swap(HDC dc);
/* the context is released and deleted (before the display mode goes back
at exit: some drivers of the time crashed otherwise) */
void w9x_gl_close(void);

/* ---------- DirectInput (w9x_input.c) */

/* the window DirectInput's devices belong to: the gamepads are looked for
from the next poll */
void w9x_input_window_opened(HWND window);
/* WM_DEVICECHANGE: look for gamepads again at the next poll */
void w9x_input_devices_changed(void);
/* once per w9x_pump: reads the gamepads */
void w9x_input_poll(void);
/* the system mouse through DirectInput, exclusive while the window is in
front: unaccelerated counts and a cursor hidden in place. FALSE without
DirectInput, or when it cannot be had */
int w9x_mouse_acquire(int acquire);
/* motion and wheel since the last read, and the first four buttons;
FALSE if it could not be read */
int w9x_mouse_read(long *dx, long *dy, long *dz, unsigned char buttons[4]);

/* ---------- DirectSound (w9x_audio.c) */

/* DirectSound's focus follows the window it is given: once the game's
exists, sound belongs to it */
void w9x_audio_window_opened(HWND window);

#endif
