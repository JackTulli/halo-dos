/*
W9X_WINDOW.C

The game's window on Windows 98/ME: the display mode, the window and its
messages, the keyboard and the mouse (see w9x_backend.h).

Fullscreen changes the display mode (display.mode, 640x480 unless set
otherwise, at 32 bits a pixel where the card has it, else 24 or 16) before
the window and its OpenGL context exist, and covers the screen with a
borderless window. The drivers of the time did not all survive a mode change
under a live context, so once the context exists F11 only changes the
window's style. The desktop's mode comes back while another program is in
front (the game's window is minimised meanwhile, as games of the time did),
while a message box is up, at exit and after a crash.

Keys arrive as WM_KEYDOWN/WM_KEYUP: the scan code (with the extended bit)
gives SDL's scancode, the keyboard layout the character. The mouse, while
the game has it, is DirectInput's (w9x_input.c): raw counts, no pointer
acceleration, the cursor hidden. Without DirectInput the cursor is hidden,
kept in the window and put back in its centre every pump, which reads its
offset. Otherwise the cursor's moves and clicks go to the menus' pointer.
*/

#include "w9x_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef WM_MOUSEWHEEL
#define WM_MOUSEWHEEL 0x020A
#endif
#ifndef WHEEL_DELTA
#define WHEEL_DELTA 120
#endif
#ifndef SPI_GETWORKAREA
#define SPI_GETWORKAREA 48
#endif

/* 98/ME's DEVMODE ends before the panning fields of Windows 2000's, which
the SDK's headers include from NT 4 on */
#define DEVMODE_SIZE_9X FIELD_OFFSET(DEVMODEA, dmPanningWidth)

#define WINDOW_CLASS "Halo"
#define WINDOW_TITLE "Halo"

static HINSTANCE instance;
static HWND window;
static HDC window_dc;
static BOOL quit_requested;

/* the display mode: the one fullscreen wants (chosen once, tested with
CDS_TEST), and whether it is set now */
static BOOL mode_planned, mode_wanted;
static DEVMODEA mode;
static BOOL mode_changed;
static BOOL exit_hook_installed;

static BOOL fullscreen;
static BOOL hidden;
/* the window is in front (set when it opens: 98's foreground lock may leave
a new window behind the program the player went to meanwhile) */
static BOOL focused;
/* the desktop's mode is back while the game is away (another program in
front, a message box), to be changed again when it returns */
static BOOL minimised_for_desktop;
static RECT windowed_rect;
static DWORD windowed_style = WS_OVERLAPPEDWINDOW;
/* the client area's last size that was not zero (a minimised window has
none) */
static int client_width = 640, client_height = 480;
/* OpenGL has drawn a frame: the window is not erased from then on */
static BOOL drawing;
/* a message box is up (w9x_display_suspend) */
static BOOL suspended;

/* the mouse: whether the game wants it, has it, and how */
static BOOL capture_wanted, capture_active, capture_direct_input, cursor_hidden;
static unsigned char direct_input_buttons[4];

/* ---------- start-up and exit */

HWND w9x_window(void)
{
	return window;
}

HINSTANCE w9x_instance(void)
{
	if (!instance)
		instance = GetModuleHandleA(NULL);
	return instance;
}

static BOOL display_mode_set(const DEVMODEA *wanted)
{
	struct w9x_fpu fpu;
	LONG result;

	w9x_fpu_save(&fpu);
	result = ChangeDisplaySettingsA((DEVMODEA *)wanted, wanted ? CDS_FULLSCREEN : 0);
	w9x_fpu_check(&fpu, "ChangeDisplaySettingsA");
	return result == DISP_CHANGE_SUCCESSFUL;
}

void w9x_display_restore(void)
{
	if (!mode_changed)
		return;
	mode_changed = FALSE;
	display_mode_set(NULL);
}

static void exit_hook(void)
{
	/* the context goes before the mode does */
	w9x_gl_close();
	w9x_display_restore();
}

static void display_mode_apply(void)
{
	if (!mode_wanted || mode_changed)
		return;
	if (!exit_hook_installed)
	{
		atexit(exit_hook);
		exit_hook_installed = TRUE;
	}
	mode_changed = display_mode_set(&mode);
	if (!mode_changed)
		platform_log("display: could not change to %lux%lu at %lu bits a pixel", mode.dmPelsWidth, mode.dmPelsHeight,
			mode.dmBitsPerPel);
}

/* the desktop's mode, with GDI's answer where EnumDisplaySettings has none */
static void desktop_mode(int *width, int *height, int *bits)
{
	DEVMODEA current;
	HDC screen;

	memset(&current, 0, sizeof(current));
	current.dmSize = DEVMODE_SIZE_9X;
	if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &current) && current.dmPelsWidth)
	{
		*width = (int)current.dmPelsWidth;
		*height = (int)current.dmPelsHeight;
		*bits = (int)current.dmBitsPerPel;
		return;
	}
	*width = GetSystemMetrics(SM_CXSCREEN);
	*height = GetSystemMetrics(SM_CYSCREEN);
	screen = GetDC(NULL);
	*bits = GetDeviceCaps(screen, BITSPIXEL) * GetDeviceCaps(screen, PLANES);
	ReleaseDC(NULL, screen);
}

/* the mode fullscreen changes to, chosen once: the size asked for at the
deepest colour the card offers for it (a 16-bit desktop is common on these
machines; OpenGL's alpha and stencil come with 32 bits) */
static void display_mode_plan(const struct w9x_video_request *request)
{
	static const int depths[] = { 32, 24, 16 };
	int index;

	if (mode_planned)
		return;
	mode_planned = TRUE;
	mode_wanted = FALSE;
	if (!request || request->mode_width <= 0 || request->mode_height <= 0)
		return;
	for (index = 0; index < (int)(sizeof(depths) / sizeof(depths[0])); index++)
	{
		int bits = request->mode_bits > 0 ? request->mode_bits : depths[index];
		struct w9x_fpu fpu;
		LONG result;

		memset(&mode, 0, sizeof(mode));
		mode.dmSize = DEVMODE_SIZE_9X;
		mode.dmPelsWidth = (DWORD)request->mode_width;
		mode.dmPelsHeight = (DWORD)request->mode_height;
		mode.dmBitsPerPel = (DWORD)bits;
		mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
		/* DISP_CHANGE_RESTART (a colour depth Windows 95 could only change
		with a restart) is no good either */
		w9x_fpu_save(&fpu);
		result = ChangeDisplaySettingsA(&mode, CDS_TEST);
		w9x_fpu_check(&fpu, "ChangeDisplaySettingsA");
		if (result == DISP_CHANGE_SUCCESSFUL)
		{
			mode_wanted = TRUE;
			return;
		}
		if (request->mode_bits > 0)
			break;
	}
	platform_log("display: no %dx%d mode%s; fullscreen keeps the desktop's", request->mode_width, request->mode_height,
		request->mode_bits > 0 ? " at those bits a pixel" : "");
}

/* the work area (the screen less the taskbar) */
static void work_area(RECT *area)
{
	if (!SystemParametersInfoA(SPI_GETWORKAREA, 0, area, 0) || area->right <= area->left)
	{
		area->left = 0;
		area->top = 0;
		area->right = GetSystemMetrics(SM_CXSCREEN);
		area->bottom = GetSystemMetrics(SM_CYSCREEN);
	}
}

/* the window's place when windowed: the client area asked for, divided by
2, 3 and on until the window fits the work area (but not below 640x480),
centred in it */
static void windowed_rect_compute(int width, int height)
{
	RECT area, rect;
	int scale_down;

	work_area(&area);
	for (scale_down = 1; scale_down < 8; scale_down++)
	{
		rect.left = 0;
		rect.top = 0;
		rect.right = width / scale_down;
		rect.bottom = height / scale_down;
		AdjustWindowRect(&rect, windowed_style, FALSE);
		if (rect.right - rect.left <= area.right - area.left && rect.bottom - rect.top <= area.bottom - area.top)
			break;
		/* the Xbox's 640x480 at least, even if it overhangs */
		if (width / (scale_down + 1) < 640)
			break;
	}
	windowed_rect.left = area.left + ((area.right - area.left) - (rect.right - rect.left)) / 2;
	windowed_rect.top = area.top + ((area.bottom - area.top) - (rect.bottom - rect.top)) / 2;
	if (windowed_rect.left < area.left)
		windowed_rect.left = area.left;
	if (windowed_rect.top < area.top)
		windowed_rect.top = area.top;
	windowed_rect.right = windowed_rect.left + (rect.right - rect.left);
	windowed_rect.bottom = windowed_rect.top + (rect.bottom - rect.top);
}

static void fullscreen_place(void)
{
	SetWindowLongA(window, GWL_STYLE, (LONG)(WS_POPUP | WS_VISIBLE));
	SetWindowPos(window, HWND_TOP, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
		SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}

static void windowed_place(void)
{
	SetWindowLongA(window, GWL_STYLE, (LONG)(windowed_style | WS_VISIBLE));
	SetWindowPos(window, HWND_NOTOPMOST, windowed_rect.left, windowed_rect.top,
		windowed_rect.right - windowed_rect.left, windowed_rect.bottom - windowed_rect.top,
		SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}

/* ---------- the mouse */

static void cursor_show(BOOL show)
{
	if (show && cursor_hidden)
	{
		ShowCursor(TRUE);
		cursor_hidden = FALSE;
	}
	else if (!show && !cursor_hidden)
	{
		ShowCursor(FALSE);
		cursor_hidden = TRUE;
	}
}

static void window_centre(POINT *centre)
{
	RECT client;

	GetClientRect(window, &client);
	centre->x = (client.right - client.left) / 2;
	centre->y = (client.bottom - client.top) / 2;
	ClientToScreen(window, centre);
}

/* the cursor kept inside the client area (the mouse without DirectInput) */
static void cursor_clip(void)
{
	RECT clip;

	GetClientRect(window, &clip);
	ClientToScreen(window, (POINT *)&clip.left);
	ClientToScreen(window, (POINT *)&clip.right);
	ClipCursor(&clip);
}

/* takes or gives back the mouse to match what the game wants and whether
the window can have it */
static void capture_apply(void)
{
	BOOL active = capture_wanted && focused && !suspended && window && !hidden && IsWindowVisible(window) &&
		!IsIconic(window);

	if (active == capture_active)
		return;
	capture_active = active;
	if (active)
	{
		capture_direct_input = w9x_mouse_acquire(TRUE);
		memset(direct_input_buttons, 0, sizeof(direct_input_buttons));
		if (!capture_direct_input)
		{
			POINT centre;

			cursor_clip();
			window_centre(&centre);
			SetCursorPos(centre.x, centre.y);
		}
		cursor_show(FALSE);
	}
	else
	{
		int index;

		if (capture_direct_input)
		{
			static const int buttons[4] = { 1, 3, 2, 4 };

			w9x_mouse_acquire(FALSE);
			/* buttons held when the mouse goes are let go */
			for (index = 0; index < 4; index++)
			{
				if (direct_input_buttons[index])
					w9x_on_mouse_button(buttons[index], 0, client_width / 2, client_height / 2);
			}
		}
		else
		{
			ClipCursor(NULL);
		}
		capture_direct_input = FALSE;
		cursor_show(TRUE);
	}
}

void w9x_mouse_capture(int capture)
{
	capture_wanted = capture != 0;
	if (window)
		capture_apply();
}

void w9x_mouse_warp(int x, int y)
{
	POINT point;

	if (!window)
		return;
	point.x = x;
	point.y = y;
	ClientToScreen(window, &point);
	SetCursorPos(point.x, point.y);
}

/* the captured mouse's motion and buttons since the last pump */
static void mouse_read(void)
{
	if (!capture_active)
		return;
	if (capture_direct_input)
	{
		/* DirectInput's buttons 0-3: left, right, middle, the first side
		button */
		static const int buttons[4] = { 1, 3, 2, 4 };
		long dx, dy, dz;
		unsigned char state[4];
		int index;

		if (!w9x_mouse_read(&dx, &dy, &dz, state))
			return;
		if (dx || dy)
			w9x_on_mouse_motion((float)dx, (float)dy);
		if (dz)
			w9x_on_wheel((float)dz / WHEEL_DELTA);
		for (index = 0; index < 4; index++)
		{
			if (state[index] != direct_input_buttons[index])
			{
				direct_input_buttons[index] = state[index];
				w9x_on_mouse_button(buttons[index], state[index], client_width / 2, client_height / 2);
			}
		}
	}
	else
	{
		POINT now, centre;

		GetCursorPos(&now);
		window_centre(&centre);
		if (now.x != centre.x || now.y != centre.y)
		{
			w9x_on_mouse_motion((float)(now.x - centre.x), (float)(now.y - centre.y));
			SetCursorPos(centre.x, centre.y);
		}
	}
}

/* ---------- the keyboard */

/* SDL's scancode for a set 1 scan code without the extended bit */
static const unsigned char scancodes[0x59] =
{
	/* 00 */ 0, 41, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 45, 46, 42, 43,
	/* 10 */ 20, 26, 8, 21, 23, 28, 24, 12, 18, 19, 47, 48, 40, 224, 4, 22,
	/* 20 */ 7, 9, 10, 11, 13, 14, 15, 51, 52, 53, 225, 49, 29, 27, 6, 25,
	/* 30 */ 5, 17, 16, 54, 55, 56, 229, 85, 226, 44, 57, 58, 59, 60, 61, 62,
	/* 40 */ 63, 64, 65, 66, 67, 83, 71, 95, 96, 97, 86, 92, 93, 94, 87, 89,
	/* 50 */ 90, 91, 98, 99, 70, 0, 100, 68, 69
};

static int scancode_from_message(WPARAM virtual_key, LPARAM lparam)
{
	int code = (int)((lparam >> 16) & 0xff);

	/* Pause sends Num Lock's scan code with Ctrl's prefix */
	if (virtual_key == VK_PAUSE)
		return 72;
	if (lparam & (1L << 24))
	{
		switch (code)
		{
		case 0x1c: return 88;  /* keypad enter */
		case 0x1d: return 228; /* right ctrl */
		case 0x35: return 84;  /* keypad / */
		case 0x37: return 70;  /* print screen */
		case 0x38: return 230; /* right alt */
		case 0x45: return 83;  /* num lock */
		case 0x47: return 74;  /* home */
		case 0x48: return 82;  /* up */
		case 0x49: return 75;  /* page up */
		case 0x4b: return 80;  /* left */
		case 0x4d: return 79;  /* right */
		case 0x4f: return 77;  /* end */
		case 0x50: return 81;  /* down */
		case 0x51: return 78;  /* page down */
		case 0x52: return 73;  /* insert */
		case 0x53: return 76;  /* delete */
		case 0x5b: return 227; /* left Windows key */
		case 0x5c: return 231; /* right Windows key */
		case 0x5d: return 101; /* menu */
		default: return 0;
		}
	}
	/* Num Lock without the extended bit on some keyboards */
	if (virtual_key == VK_NUMLOCK)
		return 83;
	return code < (int)sizeof(scancodes) ? scancodes[code] : 0;
}

static void key_message(UINT message, WPARAM wparam, LPARAM lparam)
{
	BOOL down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
	BOOL repeat = down && (lparam & (1L << 30)) != 0;
	int modifiers = 0;
	int ascii = 0;

	if (GetKeyState(VK_CONTROL) & 0x8000) modifiers |= W9X_KEY_CTRL;
	if (GetKeyState(VK_SHIFT) & 0x8000) modifiers |= W9X_KEY_SHIFT;
	if (GetKeyState(VK_MENU) & 0x8000) modifiers |= W9X_KEY_ALT;
	if (GetKeyState(VK_CAPITAL) & 0x0001) modifiers |= W9X_KEY_CAPS_LOCK;
	if (GetKeyState(VK_NUMLOCK) & 0x0001) modifiers |= W9X_KEY_NUM_LOCK;
	if (down)
	{
		BYTE state[256];
		WORD characters = 0;

		/* the layout's character (dead keys give none) */
		if (GetKeyboardState(state) &&
			ToAscii((UINT)wparam, (UINT)((lparam >> 16) & 0xff), state, &characters, 0) == 1 &&
			(characters & 0xff) != 0)
		{
			ascii = (int)(characters & 0xff);
		}
	}
	w9x_on_key(scancode_from_message(wparam, lparam), ascii, modifiers, down, repeat);
}

/* ---------- the window's messages */

/* the desktop's mode back while the game is away: also after F11 has made
the game a window on the changed mode, which would otherwise leave the
other programs on 640x480 */
static void display_mode_leave(void)
{
	if (!mode_changed || !window)
		return;
	minimised_for_desktop = TRUE;
	if (fullscreen)
		ShowWindow(window, SW_MINIMIZE);
	w9x_display_restore();
}

static void display_mode_return(void)
{
	if (!minimised_for_desktop || !window)
		return;
	minimised_for_desktop = FALSE;
	display_mode_apply();
	if (IsIconic(window))
		ShowWindow(window, SW_RESTORE);
	if (fullscreen)
		fullscreen_place();
}

static void focus_changed(BOOL now_focused)
{
	focused = now_focused;
	if (!now_focused)
	{
		capture_apply();
		/* the desktop back while another program is in front */
		display_mode_leave();
	}
	else
	{
		display_mode_return();
	}
	w9x_on_focus(now_focused);
	capture_apply();
}

static void mouse_button_message(int button, BOOL down, LPARAM lparam)
{
	/* while DirectInput has the mouse it reports the buttons (and these
	messages do not come) */
	if (capture_active && capture_direct_input)
		return;
	w9x_on_mouse_button(button, down, (int)(short)LOWORD(lparam), (int)(short)HIWORD(lparam));
}

static LRESULT CALLBACK window_procedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
	switch (message)
	{
	case WM_CLOSE:
		quit_requested = TRUE;
		return 0;
	case WM_ACTIVATEAPP:
		focus_changed(wparam != 0);
		return 0;
	case WM_SIZE:
		if (wparam != SIZE_MINIMIZED && LOWORD(lparam) && HIWORD(lparam))
		{
			client_width = LOWORD(lparam);
			client_height = HIWORD(lparam);
		}
		break;
	case WM_SYSCOMMAND:
		/* no screen saver or monitor power-down during play, and Alt alone
		or F10 does not go to the window menu */
		if ((wparam & 0xfff0) == SC_SCREENSAVE || (wparam & 0xfff0) == SC_MONITORPOWER ||
			(wparam & 0xfff0) == SC_KEYMENU)
		{
			return 0;
		}
		break;
	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
	case WM_KEYUP:
	case WM_SYSKEYUP:
		key_message(message, wparam, lparam);
		/* Alt+F4 still closes the window */
		if (message == WM_SYSKEYDOWN && wparam == VK_F4)
			break;
		return 0;
	case WM_CHAR:
	case WM_SYSCHAR:
		/* (the characters came with the keys; WM_SYSCHAR would beep) */
		return 0;
	case WM_MOUSEMOVE:
		if (!capture_active)
			w9x_on_pointer_motion((int)(short)LOWORD(lparam), (int)(short)HIWORD(lparam));
		return 0;
	case WM_LBUTTONDOWN:
	case WM_LBUTTONUP:
		mouse_button_message(1, message == WM_LBUTTONDOWN, lparam);
		return 0;
	case WM_MBUTTONDOWN:
	case WM_MBUTTONUP:
		mouse_button_message(2, message == WM_MBUTTONDOWN, lparam);
		return 0;
	case WM_RBUTTONDOWN:
	case WM_RBUTTONUP:
		mouse_button_message(3, message == WM_RBUTTONDOWN, lparam);
		return 0;
	case WM_MOUSEWHEEL:
		if (!(capture_active && capture_direct_input))
			w9x_on_wheel((float)(short)HIWORD(wparam) / WHEEL_DELTA);
		return 0;
	case WM_DEVICECHANGE:
		/* 98 and ME report a USB pad plugged in or out with
		DBT_DEVNODES_CHANGED */
		w9x_input_devices_changed();
		return TRUE;
	case WM_ERASEBKGND:
		/* OpenGL draws all of it; until it does, the class's black */
		if (drawing)
			return 1;
		break;
	case WM_PAINT:
		if (drawing)
		{
			ValidateRect(hwnd, NULL);
			return 0;
		}
		break;
	default:
		break;
	}
	return DefWindowProcA(hwnd, message, wparam, lparam);
}

/* ---------- public code */

int w9x_video_screen_mode(const struct w9x_video_request *request, int *width, int *height)
{
	int bits;

	if (window)
	{
		if (!fullscreen)
			return FALSE;
		*width = GetSystemMetrics(SM_CXSCREEN);
		*height = GetSystemMetrics(SM_CYSCREEN);
		return *width > 0 && *height > 0;
	}
	if (!request || !request->fullscreen || request->hidden)
		return FALSE;
	display_mode_plan(request);
	if (mode_wanted)
	{
		*width = (int)mode.dmPelsWidth;
		*height = (int)mode.dmPelsHeight;
		return TRUE;
	}
	desktop_mode(width, height, &bits);
	return *width > 0 && *height > 0;
}

int w9x_video_open(const struct w9x_video_request *request, struct w9x_video_result *result,
	char *error, int error_size)
{
	WNDCLASSA class_information;
	int bits;

	memset(result, 0, sizeof(*result));
	if (error_size > 0)
		error[0] = 0;
	if (window)
	{
		snprintf(error, (size_t)error_size, "The window is already open.");
		return FALSE;
	}
	hidden = request->hidden != 0;
	fullscreen = request->fullscreen && !hidden;
	if (fullscreen)
	{
		display_mode_plan(request);
		display_mode_apply();
	}

	memset(&class_information, 0, sizeof(class_information));
	/* CS_OWNDC: OpenGL's pixel format belongs to one device context for
	the window's life */
	class_information.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
	class_information.lpfnWndProc = window_procedure;
	class_information.hInstance = w9x_instance();
	/* the executable's icon (resource 1) where it has one */
	class_information.hIcon = LoadIconA(w9x_instance(), MAKEINTRESOURCEA(1));
	if (!class_information.hIcon)
		class_information.hIcon = LoadIconA(NULL, (LPCSTR)IDI_APPLICATION);
	class_information.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
	class_information.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
	class_information.lpszClassName = WINDOW_CLASS;
	if (!RegisterClassA(&class_information))
	{
		snprintf(error, (size_t)error_size, "Could not register the window class (error %lu).", GetLastError());
		w9x_display_restore();
		return FALSE;
	}
	windowed_rect_compute(request->width > 0 ? request->width : 640, request->height > 0 ? request->height : 480);
	if (fullscreen)
	{
		window = CreateWindowExA(0, WINDOW_CLASS, WINDOW_TITLE, WS_POPUP, 0, 0, GetSystemMetrics(SM_CXSCREEN),
			GetSystemMetrics(SM_CYSCREEN), NULL, NULL, w9x_instance(), NULL);
	}
	else
	{
		window = CreateWindowExA(0, WINDOW_CLASS, WINDOW_TITLE, windowed_style, windowed_rect.left, windowed_rect.top,
			windowed_rect.right - windowed_rect.left, windowed_rect.bottom - windowed_rect.top, NULL, NULL,
			w9x_instance(), NULL);
	}
	if (!window)
	{
		snprintf(error, (size_t)error_size, "Could not create the window (error %lu).", GetLastError());
		w9x_display_restore();
		return FALSE;
	}
	if (!hidden)
	{
		ShowWindow(window, SW_SHOWNORMAL);
		UpdateWindow(window);
		SetForegroundWindow(window);
	}
	/* (WM_ACTIVATEAPP has come by now if the window got the front; if 98's
	foreground lock kept it behind, the mouse is not taken until the player
	comes to it) */
	focused = !hidden && GetForegroundWindow() == window;
	{
		RECT client;

		GetClientRect(window, &client);
		if (client.right > 0 && client.bottom > 0)
		{
			client_width = client.right;
			client_height = client.bottom;
		}
	}
	window_dc = GetDC(window);
	w9x_input_window_opened(window);
	w9x_audio_window_opened(window);

	result->window = TRUE;
	result->fullscreen = fullscreen;
	result->mode_changed = mode_changed;
	desktop_mode(&result->display_width, &result->display_height, &bits);
	result->display_bits = bits;
	/* (without OpenGL the window stays, black, for the keyboard and the
	mouse) */
	w9x_gl_open(window, window_dc, request, result, error, error_size);
	return TRUE;
}

void w9x_video_size(int *width, int *height)
{
	RECT client;

	if (window && GetClientRect(window, &client) && client.right > 0 && client.bottom > 0)
	{
		client_width = client.right;
		client_height = client.bottom;
	}
	*width = client_width;
	*height = client_height;
}

void w9x_video_swap(void)
{
	if (!window_dc)
		return;
	drawing = TRUE;
	w9x_gl_swap(window_dc);
}

void w9x_video_toggle_fullscreen(void)
{
	if (!window || hidden)
		return;
	if (fullscreen)
	{
		fullscreen = FALSE;
		windowed_place();
	}
	else
	{
		/* where the window comes back to */
		if (!IsIconic(window))
			GetWindowRect(window, &windowed_rect);
		fullscreen = TRUE;
		fullscreen_place();
	}
	if (capture_active && !capture_direct_input)
		cursor_clip();
}

void w9x_display_suspend(void)
{
	suspended = TRUE;
	if (window)
		capture_apply();
	display_mode_leave();
	w9x_display_restore();
}

void w9x_display_resume(void)
{
	suspended = FALSE;
	display_mode_return();
	if (window)
		capture_apply();
}

int w9x_pump(void)
{
	MSG message;

	while (PeekMessageA(&message, NULL, 0, 0, PM_REMOVE))
	{
		TranslateMessage(&message);
		DispatchMessageA(&message);
	}
	if (window)
	{
		mouse_read();
		w9x_input_poll();
	}
	return quit_requested ? W9X_PUMP_QUIT : 0;
}
