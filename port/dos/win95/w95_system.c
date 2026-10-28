/*
W95_SYSTEM.C

The system for the Windows 95 build (halo_system.h), over Win32 alone:
nothing beyond Windows 95's own USER, GDI and KERNEL (no DirectX).

- the window: the picture scaled to it by GDI (StretchDIBits from a
  device-independent bitmap of the picture's own pixels: 8-bit with its
  colours, or 32-bit), at a whole multiple of the picture that fits the
  screen; or the whole screen, at the desktop's resolution or a display
  mode of its own (settings fullscreen and scaling: see the display's
  part); Alt+Enter switches between the window and the whole screen;
- the keys: the scan codes Windows passes with each key (lParam bits 16-24,
  the extended bit as the DOS build's 0x80), so both builds read the same
  codes;
- the mouse: while the window is in front the cursor is hidden and put
  back at the window's centre every read, its motion since the last read
  being what the player moved;
- the log: HALO.LOG beside the program, and the debugger's output.
*/

#include <windows.h>
#include <mmsystem.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "halo_system.h"
#include "r_gl.h"

int system_argc;
char **system_argv;

/* the settings (port/dos/xdk/dos_settings.c) */
int dos_setting_boolean(const char *name, int default_value);
long dos_setting_integer(const char *name, long default_value);
const char *dos_setting(const char *name, const char *default_value);
/* OpenGL (w95_gl.c) */
int w95_gl_open(HWND window);
void w95_gl_present(void);
void w95_gl_close(void);
static void gl_viewport(int frame_width, int frame_height);
#ifndef HALO_D3D
/* Direct3D 8 (w95_d3d.c; not in the separate HALOD3D.EXE, port/dos/DIRECT3D.md) */
#include "r_d3d.h"
int w95_d3d_open(HWND window);
void w95_d3d_present(void);
void w95_d3d_close(void);
static void d3d_viewport(int frame_width, int frame_height);
#endif

/* testing (setting hidden_window): the window is made but never shown, and
the mouse is left alone */
static int hidden;

/* ---------- messages */

static char program_directory[MAX_PATH];
static FILE *log_file;

const char *system_settings_file(void)
{
	return "HALO.CFG";
}

void system_default_settings(int *width, int *height, int *bits, int *fps_counter, int *sky_model)
{
	*width = 320;
	*height = 240;
	*bits = 16;
	*fps_counter = 0;
	*sky_model = 1;
}

const char *system_program_directory(void)
{
	if (!program_directory[0])
	{
		char *slash;

		GetModuleFileNameA(NULL, program_directory, sizeof(program_directory));
		slash = strrchr(program_directory, '\\');
		if (slash)
			*slash = 0;
		else
			strcpy(program_directory, ".");
	}
	return program_directory;
}

void system_log(const char *text)
{
	if (!log_file)
	{
		char path[MAX_PATH + 16];

		snprintf(path, sizeof(path), "%s\\HALO.LOG", system_program_directory());
		log_file = fopen(path, "w");
	}
	if (log_file)
	{
		fprintf(log_file, "%s\n", text);
		fflush(log_file);
	}
	OutputDebugStringA(text);
	OutputDebugStringA("\n");
}

const char *system_description(void)
{
	static char text[64];
	OSVERSIONINFOA version;

	version.dwOSVersionInfoSize = sizeof(version);
	if (GetVersionExA(&version))
	{
		snprintf(text, sizeof(text), "Windows %lu.%02lu (build %lu)", version.dwMajorVersion, version.dwMinorVersion,
			version.dwBuildNumber & 0xFFFF);
	}
	else
	{
		strcpy(text, "Windows");
	}
	return text;
}

static HWND window;

static void display_mode_restore(void);

void system_error(const char *text)
{
	char line[600];

	snprintf(line, sizeof(line), "Halo: %s", text);
	system_log(line);
	display_mode_restore();
	ClipCursor(NULL);
	ShowCursor(TRUE);
	MessageBoxA(window, text, "Halo", MB_OK | MB_ICONERROR);
	ExitProcess(1);
}

/* ---------- time */

static LARGE_INTEGER counter_frequency, counter_start;

unsigned long long system_microseconds(void)
{
	LARGE_INTEGER now;

	if (!counter_frequency.QuadPart)
	{
		/* (Windows 95 has the counter on any Pentium) */
		if (!QueryPerformanceFrequency(&counter_frequency) || !counter_frequency.QuadPart)
			counter_frequency.QuadPart = 1;
		QueryPerformanceCounter(&counter_start);
	}
	if (counter_frequency.QuadPart == 1)
		return (unsigned long long)timeGetTime() * 1000ULL;
	QueryPerformanceCounter(&now);
	return (unsigned long long)(now.QuadPart - counter_start.QuadPart) * 1000000ULL /
		(unsigned long long)counter_frequency.QuadPart;
}

#ifdef HALO_D3D
/* the Direct3D device's vertical blank, looked at from the game's waits
(port/dos/d3d/xbox/xd3d_device.c) */
void d3d_vertical_blank_poll(void);
#endif

void system_sleep(unsigned long milliseconds)
{
	Sleep(milliseconds);
#ifdef HALO_D3D
	d3d_vertical_blank_poll();
#endif
}

/* ---------- the keyboard and the mouse */

static unsigned char keys[SYSTEM_KEY_COUNT];
static int mouse_dx, mouse_dy, mouse_wheel;
static unsigned char mouse_buttons;
static int active, quit;

#define EVENT_COUNT 64
static struct
{
	unsigned char key, down;
	char character;
} events[EVENT_COUNT];
static int event_head, event_tail;

static void key_event(int key, int down, int character)
{
	if (down && keys[key] && !character)
		return;
	keys[key] = (unsigned char)down;
	if ((event_head + 1) % EVENT_COUNT != event_tail)
	{
		events[event_head].key = (unsigned char)key;
		events[event_head].down = (unsigned char)down;
		events[event_head].character = (char)character;
		event_head = (event_head + 1) % EVENT_COUNT;
	}
}

static int scan_code(LPARAM lparam)
{
	return (int)(((lparam >> 16) & 0x7F) | ((lparam & (1 << 24)) ? 0x80 : 0));
}

int system_next_keystroke(int *key, int *character, int *down)
{
	if (event_tail == event_head)
		return FALSE;
	*key = events[event_tail].key;
	*down = events[event_tail].down;
	*character = events[event_tail].character;
	event_tail = (event_tail + 1) % EVENT_COUNT;
	return TRUE;
}

/* ---------- the display

As the settings have it (and the SETTINGS page changes as the game runs:
system_display_settings_changed):

	fullscreen 0  a window, the picture at the largest whole multiple of
	              its size that fits the screen;
	           1  the whole screen, at the desktop's resolution;
	           2  the whole screen, at the display mode nearest above the
	              picture's size (the screen's resolution changed while Halo
	              is in front, put back while it is not and when it ends:
	              on an old machine GDI's stretch of the picture to a big
	              desktop is the cost); as 1 where no mode will do;
	scaling    0  sharp: the picture at whole multiples of its size;
	           1  fill: as large as fits, its shape kept;
	           2  stretch: all of the window or screen.

Alt+Enter switches between the window and the full screen (the setting's,
the desktop's when the setting is the window). */

static struct
{
	int open;
	/* what the display is now: 0 a window, 1 the screen, 2 a mode of its own */
	int fullscreen;
	int width, height;
	int scale;
	int scaling;
	/* each frame pixel shown twice across, down (DOUBLE WIDE, DOUBLE TALL:
	the frames that much smaller than the window's picture) */
	int double_x, double_y;
	/* the display mode changed (fullscreen 2): put back when Halo is not in
	front, and at its end */
	int mode_changed;
	BITMAPINFO *bitmap_info;
	RECT windowed;
	/* drawn by OpenGL (the setting renderer = opengl) */
	int gl;
	/* drawn by Direct3D 8 (the setting renderer = direct3d) */
	int d3d;
} screen;

static int setting_between(const char *name, long default_value, long last)
{
	long value = dos_setting_integer(name, default_value);

	return (int)(value < 0 ? 0 : value > last ? last : value);
}

/* the desktop's display mode back (also from the crash and error paths) */
static void display_mode_restore(void)
{
	if (screen.mode_changed)
	{
		ChangeDisplaySettingsA(NULL, 0);
		screen.mode_changed = FALSE;
	}
}

/* the display mode for the picture: the smallest at least its size, of the
desktop's colours where it has one (Windows 95 changes a resolution at
once, but its colours only after a restart), else of the most colours */
static int choose_mode(DEVMODEA *chosen)
{
	HDC desktop = GetDC(NULL);
	int desktop_bits = desktop ? GetDeviceCaps(desktop, BITSPIXEL) * GetDeviceCaps(desktop, PLANES) : 0;
	int found = FALSE;
	DEVMODEA mode;

	if (desktop)
		ReleaseDC(NULL, desktop);
	for (DWORD i = 0; ; i++)
	{
		unsigned long area, best_area;

		memset(&mode, 0, sizeof(mode));
		mode.dmSize = sizeof(mode);
		if (!EnumDisplaySettingsA(NULL, i, &mode))
			break;
		/* (not 256 colours: GDI would draw the picture with the system's
		twenty colours) */
		if (mode.dmBitsPerPel < 15 || (int)mode.dmPelsWidth < screen.width || (int)mode.dmPelsHeight < screen.height)
			continue;
		area = mode.dmPelsWidth * mode.dmPelsHeight;
		best_area = found ? chosen->dmPelsWidth * chosen->dmPelsHeight : 0;
		if (!found || area < best_area ||
			(area == best_area && ((int)mode.dmBitsPerPel == desktop_bits ||
			((int)chosen->dmBitsPerPel != desktop_bits && mode.dmBitsPerPel > chosen->dmBitsPerPel))))
		{
			*chosen = mode;
			found = TRUE;
		}
	}
	return found;
}

static int change_mode(void)
{
	DEVMODEA mode;
	char line[96];
	LONG result;

	if (screen.mode_changed)
		return TRUE;
	if (!choose_mode(&mode))
	{
		system_log("display: no display mode for the picture: the whole screen at the desktop's");
		return FALSE;
	}
	mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
	result = ChangeDisplaySettingsA(&mode, CDS_FULLSCREEN);
	snprintf(line, sizeof(line), "display: %lux%lu in %lu bits: %s", mode.dmPelsWidth, mode.dmPelsHeight,
		mode.dmBitsPerPel, result == DISP_CHANGE_SUCCESSFUL ? "set" : "refused (the desktop's instead)");
	system_log(line);
	screen.mode_changed = result == DISP_CHANGE_SUCCESSFUL;
	return screen.mode_changed;
}

static void centre_cursor(void)
{
	RECT client;
	POINT centre;

	GetClientRect(window, &client);
	centre.x = (client.right - client.left) / 2;
	centre.y = (client.bottom - client.top) / 2;
	ClientToScreen(window, &centre);
	SetCursorPos(centre.x, centre.y);
}

static void capture_mouse(int capture)
{
	static int captured;

	if (hidden)
		return;
	if (capture == captured)
		return;
	captured = capture;
	if (capture)
	{
		RECT client;
		POINT corner = { 0, 0 };

		GetClientRect(window, &client);
		ClientToScreen(window, &corner);
		client.left += corner.x;
		client.right += corner.x;
		client.top += corner.y;
		client.bottom += corner.y;
		ClipCursor(&client);
		ShowCursor(FALSE);
		centre_cursor();
	}
	else
	{
		ClipCursor(NULL);
		ShowCursor(TRUE);
	}
}

/* the display made what = 0 a window, 1 the screen, 2 a mode of its own */
static void set_display(int what)
{
	if (!window || hidden)
	{
		DEVMODEA mode;
		char line[96];

		/* (testing, hidden: the display mode it would set, logged) */
		if (hidden && what == 2 && choose_mode(&mode))
		{
			snprintf(line, sizeof(line), "display: would set %lux%lu in %lu bits (hidden)", mode.dmPelsWidth,
				mode.dmPelsHeight, mode.dmBitsPerPel);
			system_log(line);
		}
		return;
	}
	if (screen.fullscreen == 0 && what != 0)
		GetWindowRect(window, &screen.windowed);
	if (what != 2)
		display_mode_restore();
	else if (!change_mode())
		what = 1;
	/* (the mouse let go, held again in the new place by the next read) */
	capture_mouse(FALSE);
	if (what == 0)
	{
		SetWindowLongA(window, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
		SetWindowPos(window, HWND_TOP, screen.windowed.left, screen.windowed.top,
			screen.windowed.right - screen.windowed.left, screen.windowed.bottom - screen.windowed.top,
			SWP_FRAMECHANGED);
	}
	else
	{
		SetWindowLongA(window, GWL_STYLE, WS_POPUP | WS_VISIBLE);
		SetWindowPos(window, HWND_TOP, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
			SWP_FRAMECHANGED);
	}
	screen.fullscreen = what;
}

/* the SETTINGS page's (halo_system.h): the display as the settings now
have it */
void system_display_settings_changed(void)
{
	if (!screen.open)
		return;
	screen.scaling = setting_between("scaling", 0, 2);
	if (setting_between("fullscreen", 0, 2) != screen.fullscreen)
		set_display(setting_between("fullscreen", 0, 2));
}

static void toggle_fullscreen(void)
{
	int full = setting_between("fullscreen", 0, 2);

	set_display(screen.fullscreen ? 0 : full ? full : 1);
}

static LRESULT CALLBACK window_procedure(HWND handle, UINT message, WPARAM wparam, LPARAM lparam)
{
	switch (message)
	{
	case WM_CLOSE:
		quit = TRUE;
		return 0;
	case WM_ACTIVATE:
		active = LOWORD(wparam) != WA_INACTIVE;
		if (!active)
		{
			memset(keys, 0, sizeof(keys));
			capture_mouse(FALSE);
		}
		/* (a display mode of its own: the desktop's back while Halo is not
		in front, the window out of the way; Halo's again when it is) */
		if (screen.fullscreen == 2)
		{
			if (!active && screen.mode_changed)
			{
				display_mode_restore();
				ShowWindow(handle, SW_MINIMIZE);
			}
			else if (active && !screen.mode_changed)
			{
				if (IsIconic(handle))
					ShowWindow(handle, SW_RESTORE);
				if (change_mode())
				{
					SetWindowPos(handle, HWND_TOP, 0, 0, GetSystemMetrics(SM_CXSCREEN),
						GetSystemMetrics(SM_CYSCREEN), 0);
				}
			}
		}
		return 0;
	case WM_SYSKEYDOWN:
		if (wparam == VK_RETURN)
		{
			toggle_fullscreen();
			return 0;
		}
		/* (fall through: Alt and F10 are keys like the others) */
	case WM_KEYDOWN:
		key_event(scan_code(lparam), TRUE, 0);
		return 0;
	case WM_SYSKEYUP:
	case WM_KEYUP:
		key_event(scan_code(lparam), FALSE, 0);
		return 0;
	case WM_CHAR:
		/* (the character a key types, for the console: an event of its own,
		after the key's) */
		if (event_head != event_tail || TRUE)
		{
			int last = (event_head + EVENT_COUNT - 1) % EVENT_COUNT;

			if (event_head != event_tail && events[last].down && !events[last].character)
				events[last].character = (char)wparam;
		}
		return 0;
	case WM_LBUTTONDOWN: mouse_buttons |= SYSTEM_MOUSE_LEFT; capture_mouse(TRUE); return 0;
	case WM_LBUTTONUP: mouse_buttons &= ~SYSTEM_MOUSE_LEFT; return 0;
	case WM_RBUTTONDOWN: mouse_buttons |= SYSTEM_MOUSE_RIGHT; return 0;
	case WM_RBUTTONUP: mouse_buttons &= ~SYSTEM_MOUSE_RIGHT; return 0;
	case WM_MBUTTONDOWN: mouse_buttons |= SYSTEM_MOUSE_MIDDLE; return 0;
	case WM_MBUTTONUP: mouse_buttons &= ~SYSTEM_MOUSE_MIDDLE; return 0;
	case 0x020A: /* WM_MOUSEWHEEL (Windows 98; 95 with IntelliPoint) */
		mouse_wheel += (short)HIWORD(wparam) / 120;
		return 0;
	case WM_SETCURSOR:
		if (LOWORD(lparam) == HTCLIENT && active)
		{
			SetCursor(NULL);
			return TRUE;
		}
		break;
	case WM_ERASEBKGND:
		return 1;
	}
	return DefWindowProcA(handle, message, wparam, lparam);
}

static void pump(void)
{
	MSG message;

	while (PeekMessageA(&message, NULL, 0, 0, PM_REMOVE))
	{
		TranslateMessage(&message);
		DispatchMessageA(&message);
	}
}

void system_input_read(struct system_input *input)
{
	pump();
	if (active && window)
	{
		RECT client;
		POINT cursor, centre;

		capture_mouse(TRUE);
		GetClientRect(window, &client);
		centre.x = (client.right - client.left) / 2;
		centre.y = (client.bottom - client.top) / 2;
		ClientToScreen(window, &centre);
		GetCursorPos(&cursor);
		mouse_dx += cursor.x - centre.x;
		mouse_dy += cursor.y - centre.y;
		centre_cursor();
	}
	memcpy(input->keys, keys, sizeof(input->keys));
	input->mouse_dx = mouse_dx;
	input->mouse_dy = mouse_dy;
	input->mouse_wheel = mouse_wheel;
	input->mouse_buttons = mouse_buttons;
	input->mouse_present = TRUE;
	input->quit = (unsigned char)quit;
	{
		/* (a game controller: w95_pad.c; only while Halo is in front, as
		the keys) */
		void w95_pad_read(struct system_input *input, int in_front);

		w95_pad_read(input, active && window);
	}
	mouse_dx = mouse_dy = mouse_wheel = 0;
	if (quit)
	{
		system_log("the window was closed");
		exit(0);
	}
}

/* the picture's sizes to choose from (the SETTINGS page's RESOLUTION): the
usual ones up to the desktop's, and the desktop's own. The picture is drawn
at its size and scaled to the window or screen: the smaller, the faster */
int system_display_modes(int bits, int *widths, int *heights, int maximum)
{
	/* (4:3 and the old ones, and the wide screens' 16:9, 16:10 and 21:9:
	the game lays itself out on a wide one, ASPECT AUTO) */
	static const int sizes[][2] = { { 320, 200 }, { 320, 240 }, { 400, 300 }, { 512, 384 }, { 640, 360 },
		{ 640, 400 }, { 640, 480 }, { 800, 600 }, { 854, 480 }, { 1024, 576 }, { 1024, 768 }, { 1152, 864 },
		{ 1280, 720 }, { 1280, 800 }, { 1280, 960 }, { 1280, 1024 }, { 1366, 768 }, { 1440, 900 }, { 1600, 900 },
		{ 1600, 1200 }, { 1680, 1050 }, { 1920, 1080 }, { 1920, 1200 }, { 2560, 1080 }, { 2560, 1440 } };
	int desktop_width = GetSystemMetrics(SM_CXSCREEN), desktop_height = GetSystemMetrics(SM_CYSCREEN);
	int count = 0;

	(void)bits;
	for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]) && count < maximum; i++)
	{
		if (sizes[i][0] > desktop_width || sizes[i][1] > desktop_height ||
			(sizes[i][0] == desktop_width && sizes[i][1] == desktop_height))
		{
			continue;
		}
		widths[count] = sizes[i][0];
		heights[count] = sizes[i][1];
		count++;
	}
	if (count < maximum)
	{
		widths[count] = desktop_width;
		heights[count] = desktop_height;
		count++;
	}
	return count;
}

/* Windows' text services (MSCTF, and on Windows 8 and later its text input
framework) off for Halo's threads before its window is made: Halo types no
text through an IME, and on Windows 11 that framework crashed inside Halo's
message loop as a controller was switched off (its touch keyboard follows
controllers). IMM32's calls are looked up, not imported: Windows 95 may have
no IMM32, and ImmDisableTextFrameService is XP's */
static void text_services_off(void)
{
	typedef BOOL (WINAPI *disable_function)(DWORD thread);
	HMODULE imm = LoadLibraryA("IMM32.DLL");
	disable_function disable;

	if (!imm)
		return;
	disable = (disable_function)GetProcAddress(imm, "ImmDisableTextFrameService");
	if (disable)
		disable((DWORD)-1);
	disable = (disable_function)GetProcAddress(imm, "ImmDisableIME");
	if (disable)
		disable((DWORD)-1);
}

int system_display_open(int width, int height, int bits, struct system_display *display)
{
	WNDCLASSA window_class;
	RECT rectangle;
	int screen_width = GetSystemMetrics(SM_CXSCREEN), screen_height = GetSystemMetrics(SM_CYSCREEN);

	memset(&screen, 0, sizeof(screen));
	screen.width = width;
	screen.height = height;
	/* the largest whole multiple that leaves room for the window's frame */
	screen.scale = 1;
	while (width * (screen.scale + 1) <= screen_width - 32 && height * (screen.scale + 1) <= screen_height - 64)
		screen.scale++;
	text_services_off();
	memset(&window_class, 0, sizeof(window_class));
	window_class.lpfnWndProc = window_procedure;
	window_class.hInstance = GetModuleHandleA(NULL);
	window_class.hCursor = LoadCursorA(NULL, IDC_ARROW);
	window_class.lpszClassName = "Halo95";
	RegisterClassA(&window_class);
	rectangle.left = 0;
	rectangle.top = 0;
	rectangle.right = width * screen.scale;
	rectangle.bottom = height * screen.scale;
	AdjustWindowRect(&rectangle, WS_OVERLAPPEDWINDOW, FALSE);
	hidden = dos_setting_boolean("hidden_window", 0);
	window = CreateWindowExA(0, "Halo95", "Halo", WS_OVERLAPPEDWINDOW | (hidden ? 0 : WS_VISIBLE), CW_USEDEFAULT,
		CW_USEDEFAULT, rectangle.right - rectangle.left, rectangle.bottom - rectangle.top, NULL, NULL,
		GetModuleHandleA(NULL), NULL);
	if (!window)
		return FALSE;
	if (!hidden)
	{
		ShowWindow(window, SW_SHOW);
		UpdateWindow(window);
		SetForegroundWindow(window);
		active = TRUE;
	}
	screen.bitmap_info = calloc(1, sizeof(BITMAPINFOHEADER) + 256 * sizeof(RGBQUAD));
	if (!screen.bitmap_info)
		return FALSE;
	screen.bitmap_info->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	screen.bitmap_info->bmiHeader.biWidth = width;
	/* (top down) */
	screen.bitmap_info->bmiHeader.biHeight = -height;
	screen.bitmap_info->bmiHeader.biPlanes = 1;
	screen.bitmap_info->bmiHeader.biBitCount = (WORD)(bits == 8 ? 8 : 32);
	screen.bitmap_info->bmiHeader.biCompression = BI_RGB;
	screen.open = TRUE;
	screen.scaling = setting_between("scaling", 0, 2);
	{
		/* (OpenGL draws thousands or millions of colours: 256 are the
		software renderer's) */
		const char *renderer = dos_setting("renderer", "software");

		if (renderer && (!_stricmp(renderer, "opengl") || !_stricmp(renderer, "1")))
		{
			screen.gl = w95_gl_open(window);
			if (screen.gl)
				gl_viewport(width, height);
		}
#ifndef HALO_D3D
		/* (Direct3D 8 the same: w95_d3d.c) */
		if (renderer && (!_stricmp(renderer, "direct3d") || !_stricmp(renderer, "d3d") || !_stricmp(renderer, "d3d8")))
		{
			screen.d3d = w95_d3d_open(window);
			if (screen.d3d)
				d3d_viewport(width, height);
		}
#endif
	}
	GetWindowRect(window, &screen.windowed);
	/* (the desktop's display mode back however Halo ends) */
	atexit(display_mode_restore);
	set_display(setting_between("fullscreen", 0, 2));
	display->width = width * screen.scale;
	display->height = height * screen.scale;
	display->pixel_aspect = 1.0f;
	display->format = bits == 8 && !screen.gl && !screen.d3d ? _system_pixels_8 : _system_pixels_32;
	display->vertical_blank = FALSE;
	snprintf(display->description, sizeof(display->description), "%s, %s, picture x%d",
		screen.fullscreen == 2 ? "the screen in a mode of its own" : screen.fullscreen ? "the whole screen" : "a window",
		screen.gl ? "OpenGL" : screen.d3d ? "Direct3D 8" : "GDI", screen.scale);
	pump();
	return TRUE;
}

int system_display_doubling(int wide, int tall)
{
	if (!screen.open || screen.gl || screen.d3d)
		return FALSE;
	screen.double_x = wide != 0;
	screen.double_y = tall != 0;
	return TRUE;
}

/* the window, for the Direct3D build's device (port/dos/d3d) */
void *system_display_window(void)
{
	return window;
}

/* the menus' pointer (halo_system.h): the motion is the window's pixels,
the menus span the window */
void system_mouse_menu_scale(float *x, float *y)
{
	RECT client;

	*x = *y = 1.0f;
	if (window && GetClientRect(window, &client) && client.right > 0 && client.bottom > 0)
	{
		*x = 640.0f / (float)client.right;
		*y = 480.0f / (float)client.bottom;
	}
}

/* the menu's APPLY AND RESTART: this program again with its command line,
from its folder, then this one ended */
void system_restart(void)
{
	char path[MAX_PATH];
	STARTUPINFOA startup;
	PROCESS_INFORMATION process;

	GetModuleFileNameA(NULL, path, sizeof(path));
	memset(&startup, 0, sizeof(startup));
	startup.cb = sizeof(startup);
	if (CreateProcessA(path, GetCommandLineA(), NULL, NULL, FALSE, 0, NULL, system_program_directory(), &startup,
		&process))
	{
		CloseHandle(process.hThread);
		CloseHandle(process.hProcess);
	}
	else
		system_log("restart: Halo could not be started again");
	exit(0);
}

void system_display_close(void)
{
	display_mode_restore();
	w95_gl_close();
#ifndef HALO_D3D
	w95_d3d_close();
#endif
	if (window)
	{
		capture_mouse(FALSE);
		DestroyWindow(window);
		window = NULL;
	}
	free(screen.bitmap_info);
	memset(&screen, 0, sizeof(screen));
}

void system_display_palette(const unsigned char *rgb)
{
	for (int i = 0; i < 256 && screen.bitmap_info; i++)
	{
		screen.bitmap_info->bmiColors[i].rgbRed = rgb[i * 3];
		screen.bitmap_info->bmiColors[i].rgbGreen = rgb[i * 3 + 1];
		screen.bitmap_info->bmiColors[i].rgbBlue = rgb[i * 3 + 2];
		screen.bitmap_info->bmiColors[i].rgbReserved = 0;
	}
}

/* where a picture of frame_width x frame_height goes in the client area,
as the scaling says */
static void present_rectangle(const RECT *client_rectangle, int frame_width, int frame_height, int *x_out, int *y_out,
	int *width_out, int *height_out)
{
	RECT client = *client_rectangle;
	int width, height;

	if (screen.scaling == 2)
	{
		/* (stretch: all of it) */
		width = client.right;
		height = client.bottom;
	}
	else if (screen.scaling == 1)
	{
		/* (fill: as large as fits, the picture's shape kept) */
		width = client.right;
		height = (int)((long)frame_height * client.right / frame_width);
		if (height > client.bottom)
		{
			height = client.bottom;
			width = (int)((long)frame_width * client.bottom / frame_height);
		}
	}
	else
	{
		/* (sharp: the largest whole multiple that fits the client area,
		centred, or the area itself when the picture is larger) */
		int scale = 1;

		while (frame_width * (scale + 1) <= client.right && frame_height * (scale + 1) <= client.bottom)
			scale++;
		width = frame_width * scale;
		height = frame_height * scale;
		if (width > client.right || height > client.bottom)
		{
			width = client.right;
			height = client.bottom;
		}
	}
	*x_out = (client.right - width) / 2;
	*y_out = (client.bottom - height) / 2;
	*width_out = width;
	*height_out = height;
}

/* OpenGL's viewport for the next frame: the picture's rectangle (sharp
scaling fills the window all the same: OpenGL draws at the window's own
resolution, there is nothing to keep square) */
static void gl_viewport(int frame_width, int frame_height)
{
	RECT client;
	int x, y, width, height;
	int scaling = screen.scaling;

	GetClientRect(window, &client);
	if (client.right <= 0 || client.bottom <= 0)
		return;
	if (scaling == 0)
		screen.scaling = 1;
	present_rectangle(&client, frame_width, frame_height, &x, &y, &width, &height);
	screen.scaling = scaling;
	r_gl_viewport(x, client.bottom - (y + height), width, height, client.right, client.bottom);
}

#ifndef HALO_D3D
/* Direct3D's the same, from the top left (its back buffer is the client
area's size) */
static void d3d_viewport(int frame_width, int frame_height)
{
	RECT client;
	int x, y, width, height;
	int scaling = screen.scaling;

	GetClientRect(window, &client);
	if (client.right <= 0 || client.bottom <= 0)
		return;
	if (scaling == 0)
		screen.scaling = 1;
	present_rectangle(&client, frame_width, frame_height, &x, &y, &width, &height);
	screen.scaling = scaling;
	r_d3d_viewport(x, y, width, height, client.right, client.bottom);
}
#endif

void system_display_present(const struct system_frame *frame)
{
	HDC device;
	RECT client;
	int width, height, x, y;

	if (!screen.open || !window)
		return;
	if (screen.gl)
	{
		w95_gl_present();
		gl_viewport(frame->width, frame->height);
		return;
	}
#ifndef HALO_D3D
	if (screen.d3d)
	{
		w95_d3d_present();
		d3d_viewport(frame->width, frame->height);
		return;
	}
#endif
	GetClientRect(window, &client);
	if (client.right <= 0 || client.bottom <= 0)
		return;
	/* (a doubled frame's pixels each two across or down: the picture is
	placed at the size it stands for, and stretched into it) */
	present_rectangle(&client, frame->width << screen.double_x, frame->height << screen.double_y, &x, &y, &width,
		&height);
	screen.bitmap_info->bmiHeader.biWidth = frame->pitch / (frame->bits / 8);
	screen.bitmap_info->bmiHeader.biHeight = -frame->height;
	device = GetDC(window);
	if (x > 0 || y > 0)
	{
		HBRUSH black = GetStockObject(BLACK_BRUSH);
		RECT bar;

		SetRect(&bar, 0, 0, client.right, y);
		FillRect(device, &bar, black);
		SetRect(&bar, 0, y + height, client.right, client.bottom);
		FillRect(device, &bar, black);
		SetRect(&bar, 0, y, x, y + height);
		FillRect(device, &bar, black);
		SetRect(&bar, x + width, y, client.right, y + height);
		FillRect(device, &bar, black);
	}
	StretchDIBits(device, x, y, width, height, 0, 0, frame->width, frame->height, frame->pixels,
		screen.bitmap_info, DIB_RGB_COLORS, SRCCOPY);
	ReleaseDC(window, device);
}

/* ---------- crashes: what and where in the log (HALO95.MAP names the
addresses), the return addresses up the frame pointers' chain, then
Windows' own ending */

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *pointers)
{
	char line[160];
	const CONTEXT *context = pointers->ContextRecord;
	unsigned long frame = context->Ebp;

	display_mode_restore();
	snprintf(line, sizeof(line), "Halo crashed: exception %08lX at %08lX (address %08lX)",
		pointers->ExceptionRecord->ExceptionCode, (unsigned long)pointers->ExceptionRecord->ExceptionAddress,
		pointers->ExceptionRecord->NumberParameters > 1 ?
			(unsigned long)pointers->ExceptionRecord->ExceptionInformation[1] : 0UL);
	system_log(line);
	snprintf(line, sizeof(line), "EAX %08lX EBX %08lX ECX %08lX EDX %08lX ESI %08lX EDI %08lX EBP %08lX ESP %08lX",
		context->Eax, context->Ebx, context->Ecx, context->Edx, context->Esi, context->Edi, context->Ebp,
		context->Esp);
	system_log(line);
	for (int depth = 0; depth < 24 && frame && !(frame & 3) && !IsBadReadPtr((void *)frame, 8); depth++)
	{
		const unsigned long *words = (const unsigned long *)frame;

		snprintf(line, sizeof(line), "  called from %08lX", words[1]);
		system_log(line);
		if (words[0] <= frame)
			break;
		frame = words[0];
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

/* an abort (an assertion of the C runtime's, a failed allocation's): the
return addresses up the chain as for a crash */
static void abort_handler(int number)
{
	char line[64];
	unsigned long frame = (unsigned long)__builtin_frame_address(0);

	(void)number;
	display_mode_restore();
	system_log("Halo aborted");
	for (int depth = 0; depth < 24 && frame && !(frame & 3) && !IsBadReadPtr((void *)frame, 8); depth++)
	{
		const unsigned long *words = (const unsigned long *)frame;

		snprintf(line, sizeof(line), "  called from %08lX", words[1]);
		system_log(line);
		if (words[0] <= frame)
			break;
		frame = words[0];
	}
	ExitProcess(3);
}

/* ---------- start-up */

void w95_profile_start(void);

void system_initialize(void)
{
	char line[128];

	SetUnhandledExceptionFilter(crash_filter);
	signal(SIGABRT, abort_handler);
	/* the game waits for its next frame with short sleeps: at Windows'
	default timer (15.6 ms on NT) each rounds up to a whole tick */
	timeBeginPeriod(1);
	snprintf(line, sizeof(line), "Halo for Windows 95: %s", system_description());
	system_log(line);
	/* the addresses Halo Custom Edition's maps are linked to (0x40440000, 28
	MB) kept free: the game sees those maps' tags there, in blocks of its
	own (port/dos/game/dos_ce.c), and must not have any other memory there */
	if (!VirtualAlloc((void *)0x40440000UL, 0x01C00000UL, MEM_RESERVE, PAGE_NOACCESS))
		system_log("Custom Edition: the addresses 40440000 to 42040000 could not be kept free");
	w95_profile_start();
}

/* ---------- the heap (halo_system.h): the system's own business on Windows */

unsigned long system_memory_total(void)
{
	return 0;
}

unsigned long system_memory_free(void)
{
	return 0;
}
