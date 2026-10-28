/*
W9X_SHELL.C

The rest of the Windows 98/ME platform layer (see w9x_backend.h): message
boxes, the open file dialog and the progress window the first start uses
to extract the maps folder, the clipboard internet play's invites go
through, the executable's and the application data folders, the clock,
the processor's floating point state around driver calls, and the crash
report.

A GUI program on 9x has no console to print to, and the platform's log
(halo.log next to the executable, xbox_kernel.c) is only read afterwards:
the last lines of it are kept here, so that a crash can show them in a
message box.
*/

#include "w9x_internal.h"

#include <commdlg.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CSIDL_APPDATA
#define CSIDL_APPDATA 0x001a
#endif
/* the 4.0 structure's size, which 9x and NT 4 take (the SDK names it only
for Windows 2000 and later) */
#ifndef OPENFILENAME_SIZE_VERSION_400A
#define OPENFILENAME_SIZE_VERSION_400A 76
#endif

/* ---------- the floating point state */

static unsigned short read_control_word(void)
{
	unsigned short control;

	__asm__ volatile ("fnstcw %0" : "=m" (control));
	return control;
}

static unsigned long read_mxcsr(void)
{
	unsigned long mxcsr;

	__asm__ volatile ("stmxcsr %0" : "=m" (mxcsr));
	return mxcsr;
}

void w9x_fpu_save(struct w9x_fpu *state)
{
	state->control = read_control_word();
	state->mxcsr = read_mxcsr();
}

/* (call NULL: puts state in place without a word, for a new thread) */
void w9x_fpu_check(const struct w9x_fpu *state, const char *call)
{
	enum { MAXIMUM_REPORTED = 32 };
	static const char *reported[MAXIMUM_REPORTED];
	static volatile LONG reported_count;
	unsigned short control = read_control_word();
	unsigned long mxcsr = read_mxcsr();
	unsigned long restored = state->mxcsr;
	LONG index;

	/* (MXCSR's flags, bits 0-5, are what the call's own arithmetic left
	behind: no change of state) */
	if (control == state->control && !((mxcsr ^ state->mxcsr) & ~0x3Ful))
		return;
	__asm__ volatile ("fldcw %0" : : "m" (state->control));
	/* (never DAZ, bit 6: a Pentium III without it faults on the load, and
	tools/win9x_check.py wants every load shown to clear it, here by the
	instruction before) */
	__asm__ volatile ("andl $-0x41, %0; ldmxcsr %0" : "+m" (restored));
	if (!call)
		return;
	for (index = 0; index < reported_count && index < MAXIMUM_REPORTED; index++)
	{
		if (reported[index] == call)
			return;
	}
	index = InterlockedIncrement((LONG *)&reported_count) - 1;
	if (index < MAXIMUM_REPORTED)
		reported[index] = call;
	platform_log("floating point: %s changed the x87 control word %04x -> %04x, MXCSR %08lx -> %08lx; put back",
		call, state->control, control, state->mxcsr, mxcsr);
}

/* ---------- the clock */

unsigned long long w9x_ticks_ms(void)
{
	static CRITICAL_SECTION lock;
	static volatile LONG lock_ready;
	static DWORD last;
	static unsigned long long high;
	unsigned long long result;
	DWORD now;

	if (lock_ready != 2)
	{
		if (!InterlockedCompareExchange((LONG *)&lock_ready, 1, 0))
		{
			InitializeCriticalSection(&lock);
			InterlockedExchange((LONG *)&lock_ready, 2);
		}
		while (lock_ready != 2)
			Sleep(0);
	}
	EnterCriticalSection(&lock);
	now = timeGetTime();
	if (now < last)
		high += 1ULL << 32;
	last = now;
	result = high | now;
	LeaveCriticalSection(&lock);
	return result;
}

/* ---------- the log's last lines */

#define RECENT_LINES 12
#define RECENT_LINE_SIZE 160

static char recent_lines[RECENT_LINES][RECENT_LINE_SIZE];
static volatile LONG recent_next;

void w9x_log_remember(const char *line)
{
	LONG slot = InterlockedIncrement((LONG *)&recent_next) - 1;

	snprintf(recent_lines[(unsigned long)slot % RECENT_LINES], RECENT_LINE_SIZE, "%s", line);
}

/* ---------- folders */

int w9x_executable_directory(char *path, int size)
{
	char *slash;
	DWORD length;

	if (size <= 0)
		return FALSE;
	length = GetModuleFileNameA(NULL, path, (DWORD)size);
	if (!length || length >= (DWORD)size)
	{
		path[0] = 0;
		return FALSE;
	}
	slash = strrchr(path, '\\');
	if (!slash)
		slash = strrchr(path, '/');
	if (!slash)
	{
		path[0] = 0;
		return FALSE;
	}
	*slash = 0;
	return TRUE;
}

int w9x_application_data_directory(char *path, int size)
{
	typedef BOOL (WINAPI *get_folder_function)(HWND owner, LPSTR path, int folder, BOOL create);
	static get_folder_function get_folder;
	static BOOL looked;
	char folder[MAX_PATH];

	if (!looked)
	{
		HMODULE shell = LoadLibraryA("shell32.dll");

		looked = TRUE;
		get_folder = shell ? (get_folder_function)GetProcAddress(shell, "SHGetSpecialFolderPathA") : NULL;
	}
	if (!get_folder || !get_folder(NULL, folder, CSIDL_APPDATA, TRUE) || !folder[0])
		return FALSE;
	if ((int)strlen(folder) + 1 > size)
		return FALSE;
	strcpy(path, folder);
	return TRUE;
}

/* ---------- message boxes and the file dialog */

int w9x_message_yes_no(const char *text)
{
	int answer;

	w9x_display_suspend();
	answer = MessageBoxA(w9x_window(), text, "Halo", MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND);
	w9x_display_resume();
	return answer == IDYES;
}

void w9x_message_error(const char *text)
{
	w9x_display_suspend();
	MessageBoxA(w9x_window(), text, "Halo", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
	w9x_display_resume();
}

int w9x_choose_file(const char *title, const char *filter_name, const char *patterns, char *path, int size)
{
	OPENFILENAMEA dialog;
	char filter[256];
	struct w9x_fpu fpu;
	int length;
	BOOL chosen;

	if (size <= 0)
		return FALSE;
	/* "name\0patterns\0All files\0*.*\0\0" */
	length = snprintf(filter, sizeof(filter) - 1, "%s%c%s%cAll files (*.*)%c*.*%c", filter_name, 0, patterns, 0, 0, 0);
	if (length < 0 || length >= (int)sizeof(filter) - 1)
		return FALSE;
	filter[length] = 0;
	path[0] = 0;
	memset(&dialog, 0, sizeof(dialog));
	/* Windows 9x and NT 4 take only the 4.0 structure */
	dialog.lStructSize = OPENFILENAME_SIZE_VERSION_400A;
	dialog.hwndOwner = w9x_window();
	dialog.lpstrFilter = filter;
	dialog.lpstrFile = path;
	dialog.nMaxFile = (DWORD)size;
	dialog.lpstrTitle = title;
	dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
	w9x_display_suspend();
	/* (the dialog loads shell extensions) */
	w9x_fpu_save(&fpu);
	chosen = GetOpenFileNameA(&dialog);
	w9x_fpu_check(&fpu, "GetOpenFileNameA");
	w9x_display_resume();
	return chosen && path[0];
}

/* ---------- the progress window */

#define PROGRESS_CLASS "HaloProgress"
#define PROGRESS_WIDTH 640
#define PROGRESS_HEIGHT 150

struct progress
{
	HWND window;
	char title[128];
	char line[320];
	int permille;
	BOOL closed;
};

static LRESULT CALLBACK progress_procedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
	struct progress *progress = (struct progress *)GetWindowLongA(hwnd, GWL_USERDATA);

	switch (message)
	{
	case WM_PAINT:
		if (progress)
		{
			PAINTSTRUCT paint;
			HDC dc = BeginPaint(hwnd, &paint);
			HBRUSH background = CreateSolidBrush(RGB(12, 16, 20));
			HBRUSH track = CreateSolidBrush(RGB(60, 66, 72));
			HBRUSH fill = CreateSolidBrush(RGB(90, 160, 90));
			RECT client, bar;

			GetClientRect(hwnd, &client);
			FillRect(dc, &client, background);
			SetBkMode(dc, TRANSPARENT);
			SetTextColor(dc, RGB(230, 230, 230));
			TextOutA(dc, 20, 16, progress->title, (int)strlen(progress->title));
			TextOutA(dc, 20, 64, progress->line, (int)strlen(progress->line));
			SetRect(&bar, 20, 100, client.right - 20, 124);
			FillRect(dc, &bar, track);
			bar.right = bar.left + (bar.right - bar.left) * progress->permille / 1000;
			FillRect(dc, &bar, fill);
			DeleteObject(background);
			DeleteObject(track);
			DeleteObject(fill);
			EndPaint(hwnd, &paint);
			return 0;
		}
		break;
	case WM_ERASEBKGND:
		return 1;
	case WM_CLOSE:
		if (progress)
			progress->closed = TRUE;
		return 0;
	default:
		break;
	}
	return DefWindowProcA(hwnd, message, wparam, lparam);
}

void *w9x_progress_open(const char *title)
{
	static BOOL registered;
	struct progress *progress = calloc(1, sizeof(*progress));
	RECT rect = { 0, 0, PROGRESS_WIDTH, PROGRESS_HEIGHT };
	const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;

	if (!progress)
		return NULL;
	if (!registered)
	{
		WNDCLASSA class_information;

		memset(&class_information, 0, sizeof(class_information));
		class_information.lpfnWndProc = progress_procedure;
		class_information.hInstance = w9x_instance();
		class_information.hIcon = LoadIconA(w9x_instance(), MAKEINTRESOURCEA(1));
		if (!class_information.hIcon)
			class_information.hIcon = LoadIconA(NULL, (LPCSTR)IDI_APPLICATION);
		class_information.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
		class_information.lpszClassName = PROGRESS_CLASS;
		registered = RegisterClassA(&class_information) != 0;
	}
	snprintf(progress->title, sizeof(progress->title), "%s", title);
	AdjustWindowRect(&rect, style, FALSE);
	progress->window = CreateWindowExA(0, PROGRESS_CLASS, "Halo", style, CW_USEDEFAULT, CW_USEDEFAULT,
		rect.right - rect.left, rect.bottom - rect.top, NULL, NULL, w9x_instance(), NULL);
	if (!progress->window)
	{
		free(progress);
		return NULL;
	}
	SetWindowLongA(progress->window, GWL_USERDATA, (LONG)progress);
	ShowWindow(progress->window, SW_SHOWNORMAL);
	UpdateWindow(progress->window);
	return progress;
}

int w9x_progress_update(void *handle, const char *line, int permille)
{
	struct progress *progress = handle;
	MSG message;

	if (!progress)
		return TRUE;
	if (permille < 0)
		permille = 0;
	if (permille > 1000)
		permille = 1000;
	if (strcmp(progress->line, line) || progress->permille != permille)
	{
		snprintf(progress->line, sizeof(progress->line), "%s", line);
		progress->permille = permille;
		InvalidateRect(progress->window, NULL, FALSE);
	}
	while (PeekMessageA(&message, NULL, 0, 0, PM_REMOVE))
	{
		TranslateMessage(&message);
		DispatchMessageA(&message);
	}
	return !progress->closed;
}

void w9x_progress_close(void *handle)
{
	struct progress *progress = handle;

	if (!progress)
		return;
	DestroyWindow(progress->window);
	free(progress);
}

/* ---------- the clipboard */

int w9x_clipboard_set(const char *text)
{
	size_t length = strlen(text);
	HGLOBAL memory;
	char *copy;
	BOOL set;

	/* the window owns what it puts there (SetClipboardData fails for an
	empty owner on 9x) */
	if (!w9x_window())
		return FALSE;
	memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, length + 1);
	if (!memory)
		return FALSE;
	copy = GlobalLock(memory);
	if (!copy)
	{
		GlobalFree(memory);
		return FALSE;
	}
	memcpy(copy, text, length + 1);
	GlobalUnlock(memory);
	if (!OpenClipboard(w9x_window()))
	{
		GlobalFree(memory);
		return FALSE;
	}
	EmptyClipboard();
	set = SetClipboardData(CF_TEXT, memory) != NULL;
	CloseClipboard();
	if (!set)
		GlobalFree(memory);
	return set;
}

int w9x_clipboard_get(char *text, int size)
{
	HANDLE memory;
	BOOL found = FALSE;

	if (size <= 0 || !OpenClipboard(w9x_window()))
		return FALSE;
	memory = GetClipboardData(CF_TEXT);
	if (memory)
	{
		const char *locked = GlobalLock(memory);

		if (locked)
		{
			if ((int)strlen(locked) < size)
			{
				strcpy(text, locked);
				found = TRUE;
			}
			GlobalUnlock(memory);
		}
	}
	CloseClipboard();
	return found;
}

/* ---------- the crash report */

void w9x_crash_report(const char *what)
{
	static volatile LONG reporting;
	static volatile DWORD reporting_thread;
	char text[RECENT_LINES * (RECENT_LINE_SIZE + 2) + 1024];
	char log_path[MAX_PATH];
	size_t used;
	LONG next = recent_next;
	LONG index;

	if (InterlockedExchange((LONG *)&reporting, 1))
	{
		/* a second fault in the reporting thread just ends. One in another
		thread (the state that broke the first often breaks a second) waits:
		its filter's return would end the process, and the box with it,
		before the player has read it */
		if (reporting_thread != GetCurrentThreadId())
			Sleep(INFINITE);
		return;
	}
	reporting_thread = GetCurrentThreadId();
	w9x_display_restore();
	ClipCursor(NULL);
	while (ShowCursor(TRUE) < 0)
		;
	if (!w9x_executable_directory(log_path, (int)sizeof(log_path) - 10))
		log_path[0] = 0;
	strcat(log_path, log_path[0] ? "\\halo.log" : "halo.log");
	used = (size_t)snprintf(text, sizeof(text), "Halo has stopped: %s\n\nThe last lines of its log (%s):\n\n",
		what, log_path);
	for (index = next > RECENT_LINES ? next - RECENT_LINES : 0; index < next && used < sizeof(text); index++)
	{
		int written = snprintf(text + used, sizeof(text) - used, "%s\n",
			recent_lines[(unsigned long)index % RECENT_LINES]);

		if (written < 0)
			break;
		used += (size_t)written;
	}
	MessageBoxA(NULL, text, "Halo", MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
}
