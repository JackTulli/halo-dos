/*
DOS_KERNEL32.C

The Win32 calls of the C runtime and the platform layer, answered over DOS.

The DOS build links the Windows 95 build's C runtime (port/win9x: nxdk's
PDCLib over KERNEL32) and the platform layer's Win32 half unchanged; they
are compiled with _KERNEL32_ and _USER32_ (tools/dos_build.py), so these
functions are called directly instead of through import tables. Each
keeps the SDK's prototype, and so its stdcall name.

One thread: critical sections and events do nothing that waits, and
CreateThread fails. Files are DOS handles (dos_files.c); time is the
processor's time stamp counter and DOS's date (the local time, which DOS
holds; the coordinated time is that less the zone's offset: GetSystemTime).
*/

#include <windows.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dos_dpmi.h"
#include "dos_files.h"
#include "dos_kernel32.h"

/* ---------- errors */

static DWORD last_error;

DWORD WINAPI GetLastError(void)
{
	return last_error;
}

void WINAPI SetLastError(DWORD error)
{
	last_error = error;
}

/* a DOS error as the call's answer: FALSE, the error kept */
static BOOL failed(int dos_error)
{
	last_error = (DWORD)dos_error;
	return FALSE;
}

/* ---------- the log and the console */

static unsigned short log_handle;
static int log_open;
static int console_visible = 1;

void dos_console_set_visible(int visible)
{
	console_visible = visible;
}

static void console_write(const char *text, unsigned long length)
{
	unsigned long done;

	if (console_visible)
		dos_write(1, text, length, &done);
}

void dos_log_text(const char *text)
{
	unsigned long length = (unsigned long)strlen(text);
	unsigned long done;

	if (!log_open)
	{
		/* the log beside the program, where the player finds it */
		char path[260];
		char *slash;

		strncpy(path, dos_program_path(), sizeof(path) - 16);
		path[sizeof(path) - 16] = 0;
		slash = strrchr(path, '\\');
		strcpy(slash ? slash + 1 : path, "HALO.LOG");
		log_open = dos_open(path, DOS_OPEN_WRITE, DOS_IF_EXISTS_TRUNCATE | DOS_IF_MISSING_CREATE, &log_handle) == 0 ?
			1 : -1;
	}
	if (log_open > 0)
	{
		dos_write(log_handle, text, length, &done);
		dos_write(log_handle, "\r\n", 2, &done);
		/* (a crash must not lose the last lines) */
		dos_commit(log_handle);
	}
	console_write(text, length);
	console_write("\r\n", 2);
}

void WINAPI OutputDebugStringA(LPCSTR text)
{
	char line[512];
	size_t length;

	strncpy(line, text, sizeof(line) - 1);
	line[sizeof(line) - 1] = 0;
	length = strlen(line);
	while (length && (line[length - 1] == '\n' || line[length - 1] == '\r'))
		line[--length] = 0;
	dos_log_text(line);
}

int WINAPI MessageBoxA(HWND window, LPCSTR text, LPCSTR caption, UINT type)
{
	(void)window;
	(void)type;
	dos_console_set_visible(1);
	if (caption)
	{
		dos_log_text(caption);
	}
	dos_log_text(text);
	return IDOK;
}

/* ---------- handles */

enum
{
	_handle_free,
	_handle_file,
	_handle_event,
	_handle_find,
};

#define MAXIMUM_HANDLES 64
#define HANDLE_BASE 0x1000

struct handle
{
	int type;
	unsigned short dos;
	int signaled;
	int manual_reset;
	struct dos_find *find;
};

static struct handle handles[MAXIMUM_HANDLES];

static HANDLE handle_new(int type, struct handle **entry)
{
	for (int i = 3; i < MAXIMUM_HANDLES; i++)
	{
		if (handles[i].type == _handle_free)
		{
			memset(&handles[i], 0, sizeof(handles[i]));
			handles[i].type = type;
			*entry = &handles[i];
			return (HANDLE)(ULONG_PTR)(HANDLE_BASE + i);
		}
	}
	last_error = ERROR_TOO_MANY_OPEN_FILES;
	return INVALID_HANDLE_VALUE;
}

static struct handle *handle_get(HANDLE value)
{
	ULONG_PTR index = (ULONG_PTR)value - HANDLE_BASE;

	if (index >= MAXIMUM_HANDLES || handles[index].type == _handle_free)
	{
		last_error = ERROR_INVALID_HANDLE;
		return NULL;
	}
	return &handles[index];
}

HANDLE WINAPI GetStdHandle(DWORD which)
{
	int index;

	switch (which)
	{
	case STD_INPUT_HANDLE: index = 0; break;
	case STD_OUTPUT_HANDLE: index = 1; break;
	case STD_ERROR_HANDLE: index = 2; break;
	default: last_error = ERROR_INVALID_PARAMETER; return INVALID_HANDLE_VALUE;
	}
	handles[index].type = _handle_file;
	handles[index].dos = (unsigned short)index;
	return (HANDLE)(ULONG_PTR)(HANDLE_BASE + index);
}

BOOL WINAPI CloseHandle(HANDLE object)
{
	struct handle *handle = handle_get(object);

	if (!handle)
		return FALSE;
	if (handle->type == _handle_file && handle->dos > 2)
		dos_close(handle->dos);
	if (handle->type == _handle_find && handle->find)
	{
		dos_find_close(handle->find);
		free(handle->find);
	}
	if ((ULONG_PTR)object - HANDLE_BASE > 2)
		handle->type = _handle_free;
	return TRUE;
}

/* ---------- files */

HANDLE WINAPI CreateFileA(LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
	DWORD disposition, DWORD flags, HANDLE template_file)
{
	unsigned short mode, action;
	unsigned short dos;
	struct handle *entry;
	HANDLE result;
	int error;

	(void)share;
	(void)security;
	(void)flags;
	(void)template_file;
	if ((access & (GENERIC_READ | GENERIC_WRITE)) == (GENERIC_READ | GENERIC_WRITE))
		mode = DOS_OPEN_READ_WRITE;
	else if (access & GENERIC_WRITE)
		mode = DOS_OPEN_WRITE;
	else
		mode = DOS_OPEN_READ;
	switch (disposition)
	{
	case CREATE_NEW: action = DOS_IF_EXISTS_FAIL | DOS_IF_MISSING_CREATE; break;
	case CREATE_ALWAYS: action = DOS_IF_EXISTS_TRUNCATE | DOS_IF_MISSING_CREATE; break;
	case OPEN_ALWAYS: action = DOS_IF_EXISTS_OPEN | DOS_IF_MISSING_CREATE; break;
	case TRUNCATE_EXISTING: action = DOS_IF_EXISTS_TRUNCATE | DOS_IF_MISSING_FAIL; break;
	default: action = DOS_IF_EXISTS_OPEN | DOS_IF_MISSING_FAIL; break;
	}
	/* "CON" is the console */
	if (!_stricmp(path, "CON") || !_stricmp(path, "CONOUT$"))
		return GetStdHandle(STD_OUTPUT_HANDLE);
	error = dos_open(path, mode, action, &dos);
	if (error)
	{
		failed(error);
		return INVALID_HANDLE_VALUE;
	}
	result = handle_new(_handle_file, &entry);
	if (result == INVALID_HANDLE_VALUE)
	{
		dos_close(dos);
		return result;
	}
	entry->dos = dos;
	last_error = 0;
	return result;
}

BOOL WINAPI ReadFile(HANDLE file, LPVOID buffer, DWORD size, LPDWORD read, LPOVERLAPPED overlapped)
{
	struct handle *handle = handle_get(file);
	unsigned long done = 0;
	int error;

	(void)overlapped;
	if (read)
		*read = 0;
	if (!handle || handle->type != _handle_file)
		return FALSE;
	error = dos_read(handle->dos, buffer, size, &done);
	if (read)
		*read = done;
	return error ? failed(error) : TRUE;
}

BOOL WINAPI WriteFile(HANDLE file, LPCVOID buffer, DWORD size, LPDWORD written, LPOVERLAPPED overlapped)
{
	struct handle *handle = handle_get(file);
	unsigned long done = 0;
	int error;

	(void)overlapped;
	if (written)
		*written = 0;
	if (!handle || handle->type != _handle_file)
		return FALSE;
	if (handle->dos <= 2)
	{
		/* the console's lines go to the log too (stdout and stderr) */
		if (handle->dos >= 1)
		{
			char line[512];
			DWORD length = size < sizeof(line) - 1 ? size : sizeof(line) - 1;

			memcpy(line, buffer, length);
			while (length && (line[length - 1] == '\n' || line[length - 1] == '\r'))
				length--;
			line[length] = 0;
			if (length)
				dos_log_text(line);
		}
		if (written)
			*written = size;
		return TRUE;
	}
	if (!size)
		return TRUE;
	error = dos_write(handle->dos, buffer, size, &done);
	if (written)
		*written = done;
	return error ? failed(error) : TRUE;
}

DWORD WINAPI SetFilePointer(HANDLE file, LONG distance, PLONG distance_high, DWORD method)
{
	struct handle *handle = handle_get(file);
	unsigned long position;
	int error;

	if (!handle || handle->type != _handle_file)
		return INVALID_SET_FILE_POINTER;
	if (distance_high && *distance_high != 0 && *distance_high != -1)
	{
		last_error = ERROR_INVALID_PARAMETER;
		return INVALID_SET_FILE_POINTER;
	}
	error = dos_seek(handle->dos, distance, method == FILE_BEGIN ? 0 : method == FILE_CURRENT ? 1 : 2, &position);
	if (error)
	{
		failed(error);
		return INVALID_SET_FILE_POINTER;
	}
	if (distance_high)
		*distance_high = 0;
	last_error = 0;
	return position;
}

BOOL WINAPI SetFilePointerEx(HANDLE file, LARGE_INTEGER distance, PLARGE_INTEGER position, DWORD method)
{
	LONG high = distance.HighPart;
	DWORD low = SetFilePointer(file, (LONG)distance.LowPart, &high, method);

	if (low == INVALID_SET_FILE_POINTER && last_error)
		return FALSE;
	if (position)
	{
		position->LowPart = low;
		position->HighPart = 0;
	}
	return TRUE;
}

DWORD WINAPI GetFileSize(HANDLE file, LPDWORD size_high)
{
	struct handle *handle = handle_get(file);
	unsigned long here, end;

	if (size_high)
		*size_high = 0;
	if (!handle || handle->type != _handle_file)
		return INVALID_FILE_SIZE;
	if (dos_seek(handle->dos, 0, 1, &here) || dos_seek(handle->dos, 0, 2, &end))
		return INVALID_FILE_SIZE;
	dos_seek(handle->dos, (long)here, 0, &here);
	return end;
}

BOOL WINAPI GetFileSizeEx(HANDLE file, PLARGE_INTEGER size)
{
	DWORD low = GetFileSize(file, NULL);

	if (low == INVALID_FILE_SIZE)
		return FALSE;
	size->QuadPart = low;
	return TRUE;
}

BOOL WINAPI FlushFileBuffers(HANDLE file)
{
	struct handle *handle = handle_get(file);

	if (!handle || handle->type != _handle_file)
		return FALSE;
	if (handle->dos > 2)
		dos_commit(handle->dos);
	return TRUE;
}

BOOL WINAPI SetEndOfFile(HANDLE file)
{
	struct handle *handle = handle_get(file);
	unsigned long done;
	int error;

	if (!handle || handle->type != _handle_file)
		return FALSE;
	/* a write of no bytes ends the file at the position */
	error = dos_write(handle->dos, "", 0, &done);
	return error ? failed(error) : TRUE;
}

DWORD WINAPI GetFileType(HANDLE file)
{
	struct handle *handle = handle_get(file);

	if (!handle || handle->type != _handle_file)
		return FILE_TYPE_UNKNOWN;
	return dos_is_device(handle->dos) ? FILE_TYPE_CHAR : FILE_TYPE_DISK;
}

/* DOS's date and time (local) as a FILETIME */
static void file_time_from_dos(unsigned long date_time, FILETIME *file_time)
{
	SYSTEMTIME system;

	memset(&system, 0, sizeof(system));
	system.wYear = (WORD)(1980 + (date_time >> 25));
	system.wMonth = (WORD)((date_time >> 21) & 15);
	system.wDay = (WORD)((date_time >> 16) & 31);
	system.wHour = (WORD)((date_time >> 11) & 31);
	system.wMinute = (WORD)((date_time >> 5) & 63);
	system.wSecond = (WORD)((date_time & 31) * 2);
	if (!system.wMonth)
		system.wMonth = 1;
	if (!system.wDay)
		system.wDay = 1;
	SystemTimeToFileTime(&system, file_time);
}

BOOL WINAPI GetFileInformationByHandle(HANDLE file, LPBY_HANDLE_FILE_INFORMATION information)
{
	struct handle *handle = handle_get(file);
	unsigned long date_time = 0;

	if (!handle || handle->type != _handle_file)
		return FALSE;
	memset(information, 0, sizeof(*information));
	information->dwFileAttributes = FILE_ATTRIBUTE_ARCHIVE;
	information->nFileSizeLow = GetFileSize(file, &information->nFileSizeHigh);
	information->nNumberOfLinks = 1;
	if (handle->dos > 2 && !dos_file_time(handle->dos, &date_time))
	{
		file_time_from_dos(date_time, &information->ftLastWriteTime);
		information->ftCreationTime = information->ftLastWriteTime;
		information->ftLastAccessTime = information->ftLastWriteTime;
	}
	return TRUE;
}

BOOL WINAPI DeleteFileA(LPCSTR path)
{
	int error = dos_delete(path);

	return error ? failed(error) : TRUE;
}

BOOL WINAPI MoveFileA(LPCSTR from, LPCSTR to)
{
	int error = dos_rename(from, to);

	return error ? failed(error) : TRUE;
}

DWORD WINAPI GetFileAttributesA(LPCSTR path)
{
	unsigned short attributes;
	int error = dos_get_attributes(path, &attributes);

	if (error)
	{
		failed(error);
		return INVALID_FILE_ATTRIBUTES;
	}
	return attributes ? attributes : FILE_ATTRIBUTE_NORMAL;
}

BOOL WINAPI GetFileAttributesExA(LPCSTR path, GET_FILEEX_INFO_LEVELS level, LPVOID result)
{
	WIN32_FILE_ATTRIBUTE_DATA *data = result;
	struct dos_find find;
	int error;

	(void)level;
	memset(data, 0, sizeof(*data));
	error = dos_find_first(path, &find);
	if (error)
	{
		/* (a root or a directory a search cannot name) */
		DWORD attributes = GetFileAttributesA(path);

		if (attributes == INVALID_FILE_ATTRIBUTES)
			return FALSE;
		data->dwFileAttributes = attributes;
		return TRUE;
	}
	dos_find_close(&find);
	data->dwFileAttributes = find.attributes ? find.attributes : FILE_ATTRIBUTE_NORMAL;
	data->nFileSizeLow = find.size;
	file_time_from_dos(find.date_time, &data->ftLastWriteTime);
	data->ftCreationTime = data->ftLastWriteTime;
	data->ftLastAccessTime = data->ftLastWriteTime;
	return TRUE;
}

BOOL WINAPI SetFileAttributesA(LPCSTR path, DWORD attributes)
{
	int error = dos_set_attributes(path, (unsigned short)(attributes & 0x27));

	return error ? failed(error) : TRUE;
}

BOOL WINAPI CreateDirectoryA(LPCSTR path, LPSECURITY_ATTRIBUTES security)
{
	int error;

	(void)security;
	error = dos_make_directory(path);
	return error ? failed(error == DOS_ERROR_ACCESS_DENIED ? ERROR_ALREADY_EXISTS : error) : TRUE;
}

BOOL WINAPI RemoveDirectoryA(LPCSTR path)
{
	int error = dos_remove_directory(path);

	return error ? failed(error) : TRUE;
}

DWORD WINAPI GetCurrentDirectoryA(DWORD size, LPSTR buffer)
{
	char path[300];
	DWORD length;

	if (dos_current_directory(path, sizeof(path)))
		return 0;
	length = (DWORD)strlen(path);
	/* (no separator at the end but a root's) */
	if (length > 3 && path[length - 1] == '\\')
		path[--length] = 0;
	if (length + 1 > size)
		return length + 1;
	memcpy(buffer, path, length + 1);
	return length;
}

BOOL WINAPI SetCurrentDirectoryA(LPCSTR path)
{
	int error = dos_change_directory(path);

	return error ? failed(error) : TRUE;
}

DWORD WINAPI GetFullPathNameA(LPCSTR path, DWORD size, LPSTR buffer, LPSTR *file_part)
{
	char full[300];
	DWORD length;

	if (path[0] && path[1] == ':' && (path[2] == '\\' || path[2] == '/'))
		snprintf(full, sizeof(full), "%s", path);
	else if (path[0] == '\\' || path[0] == '/')
	{
		char current[300];

		if (dos_current_directory(current, sizeof(current)))
			return 0;
		snprintf(full, sizeof(full), "%c:%s", current[0], path);
	}
	else
	{
		char current[300];
		size_t used;

		if (dos_current_directory(current, sizeof(current)))
			return 0;
		used = strlen(current);
		snprintf(full, sizeof(full), "%s%s%s", current, used && current[used - 1] == '\\' ? "" : "\\", path);
	}
	for (char *c = full; *c; c++)
	{
		if (*c == '/')
			*c = '\\';
	}
	length = (DWORD)strlen(full);
	if (length + 1 > size)
		return length + 1;
	memcpy(buffer, full, length + 1);
	if (file_part)
	{
		char *slash = strrchr(buffer, '\\');

		*file_part = slash ? slash + 1 : buffer;
	}
	return length;
}

DWORD WINAPI GetModuleFileNameA(HMODULE module, LPSTR buffer, DWORD size)
{
	DWORD length = (DWORD)strlen(dos_program_path());

	(void)module;
	if (!size)
		return 0;
	if (length >= size)
		length = size - 1;
	memcpy(buffer, dos_program_path(), length);
	buffer[length] = 0;
	return length;
}

DWORD WINAPI GetTempPathA(DWORD size, LPSTR buffer)
{
	char path[260];
	DWORD length;

	if (!GetEnvironmentVariableA("TEMP", path, sizeof(path)) && !GetEnvironmentVariableA("TMP", path, sizeof(path)))
	{
		if (!GetCurrentDirectoryA(sizeof(path), path))
			strcpy(path, "C:\\");
	}
	length = (DWORD)strlen(path);
	if (length && path[length - 1] != '\\' && length + 1 < sizeof(path))
	{
		path[length++] = '\\';
		path[length] = 0;
	}
	if (length + 1 > size)
		return length + 1;
	memcpy(buffer, path, length + 1);
	return length;
}

UINT WINAPI GetTempFileNameA(LPCSTR directory, LPCSTR prefix, UINT unique, LPSTR buffer)
{
	static UINT counter;

	for (int attempt = 0; attempt < 1000; attempt++)
	{
		UINT number = unique ? unique : (UINT)((dos_rdtsc() >> 8) + ++counter) & 0xFFFF;
		unsigned short handle;

		snprintf(buffer, MAX_PATH, "%s%s%.3s%04X.TMP", directory,
			directory[0] && directory[strlen(directory) - 1] != '\\' ? "\\" : "", prefix, number);
		if (unique)
			return unique;
		if (!dos_open(buffer, DOS_OPEN_WRITE, DOS_IF_EXISTS_FAIL | DOS_IF_MISSING_CREATE, &handle))
		{
			dos_close(handle);
			return number;
		}
	}
	return 0;
}

BOOL WINAPI GetDiskFreeSpaceExA(LPCSTR directory, PULARGE_INTEGER available, PULARGE_INTEGER total,
	PULARGE_INTEGER free_bytes)
{
	unsigned long long free_space, total_space;
	int drive = 0;
	int error;

	if (directory && directory[0] && directory[1] == ':')
		drive = (directory[0] | 0x20) - 'a' + 1;
	error = dos_disk_space(drive, &free_space, &total_space);
	if (error)
		return failed(error);
	if (available)
		available->QuadPart = free_space;
	if (total)
		total->QuadPart = total_space;
	if (free_bytes)
		free_bytes->QuadPart = free_space;
	return TRUE;
}

/* ---------- searches */

static void find_data(const struct dos_find *find, WIN32_FIND_DATAA *data)
{
	memset(data, 0, sizeof(*data));
	data->dwFileAttributes = find->attributes ? find->attributes : FILE_ATTRIBUTE_NORMAL;
	data->nFileSizeLow = find->size;
	file_time_from_dos(find->date_time, &data->ftLastWriteTime);
	data->ftCreationTime = data->ftLastWriteTime;
	data->ftLastAccessTime = data->ftLastWriteTime;
	strncpy(data->cFileName, find->name, sizeof(data->cFileName) - 1);
}

HANDLE WINAPI FindFirstFileA(LPCSTR pattern, LPWIN32_FIND_DATAA data)
{
	struct dos_find *find = malloc(sizeof(*find));
	struct handle *entry;
	HANDLE result;
	int error;

	if (!find)
	{
		last_error = ERROR_NOT_ENOUGH_MEMORY;
		return INVALID_HANDLE_VALUE;
	}
	error = dos_find_first(pattern, find);
	if (error)
	{
		free(find);
		failed(error == DOS_ERROR_NO_MORE_FILES ? ERROR_FILE_NOT_FOUND : error);
		return INVALID_HANDLE_VALUE;
	}
	result = handle_new(_handle_find, &entry);
	if (result == INVALID_HANDLE_VALUE)
	{
		dos_find_close(find);
		free(find);
		return result;
	}
	entry->find = find;
	find_data(find, data);
	return result;
}

BOOL WINAPI FindNextFileA(HANDLE search, LPWIN32_FIND_DATAA data)
{
	struct handle *handle = handle_get(search);
	int error;

	if (!handle || handle->type != _handle_find)
		return FALSE;
	error = dos_find_next(handle->find);
	if (error)
		return failed(ERROR_NO_MORE_FILES);
	find_data(handle->find, data);
	return TRUE;
}

BOOL WINAPI FindClose(HANDLE search)
{
	return CloseHandle(search);
}

/* ---------- the process */

void WINAPI ExitProcess(UINT code)
{
	dos_exit((int)code);
}

LPSTR WINAPI GetCommandLineA(void)
{
	return (LPSTR)dos_command_line();
}

DWORD WINAPI GetEnvironmentVariableA(LPCSTR name, LPSTR buffer, DWORD size)
{
	const char *entry = dos_environment();
	size_t name_length = strlen(name);

	for (; entry && *entry; entry += strlen(entry) + 1)
	{
		if (!_strnicmp(entry, name, name_length) && entry[name_length] == '=')
		{
			const char *value = entry + name_length + 1;
			DWORD length = (DWORD)strlen(value);

			if (length + 1 > size)
				return length + 1;
			memcpy(buffer, value, length + 1);
			return length;
		}
	}
	last_error = ERROR_ENVVAR_NOT_FOUND;
	return 0;
}

BOOL WINAPI GetVersionExA(LPOSVERSIONINFOA version)
{
	struct dos_registers r;

	memset(&r, 0, sizeof(r));
	r.eax = 0x3000;
	dos_real_interrupt(0x21, &r);
	version->dwMajorVersion = r.eax & 0xFF;
	version->dwMinorVersion = (r.eax >> 8) & 0xFF;
	version->dwBuildNumber = 0;
	/* (neither 9x nor NT: the C runtime's code for either does not apply) */
	version->dwPlatformId = VER_PLATFORM_WIN32s;
	strcpy(version->szCSDVersion, "MS-DOS");
	return TRUE;
}

DWORD WINAPI GetCurrentThreadId(void)
{
	return 1;
}

DWORD WINAPI GetCurrentProcessId(void)
{
	return 1;
}

HMODULE WINAPI GetModuleHandleA(LPCSTR name)
{
	(void)name;
	return (HMODULE)0x400000;
}

FARPROC WINAPI GetProcAddress(HMODULE module, LPCSTR name)
{
	(void)module;
	(void)name;
	last_error = ERROR_PROC_NOT_FOUND;
	return NULL;
}

HMODULE WINAPI LoadLibraryA(LPCSTR name)
{
	(void)name;
	last_error = ERROR_MOD_NOT_FOUND;
	return NULL;
}

/* ---------- time */

DWORD WINAPI GetTickCount(void)
{
	return (DWORD)(dos_microseconds() / 1000);
}

BOOL WINAPI QueryPerformanceCounter(LARGE_INTEGER *counter)
{
	counter->QuadPart = (LONGLONG)dos_microseconds();
	return TRUE;
}

BOOL WINAPI QueryPerformanceFrequency(LARGE_INTEGER *frequency)
{
	frequency->QuadPart = 1000000;
	return TRUE;
}

void WINAPI Sleep(DWORD milliseconds)
{
	unsigned long long until = dos_microseconds() + (unsigned long long)milliseconds * 1000;

	while (dos_microseconds() < until)
		;
}

DWORD WINAPI SleepEx(DWORD milliseconds, BOOL alertable)
{
	(void)alertable;
	Sleep(milliseconds);
	return 0;
}

static int is_leap(int year)
{
	return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

/* days since 1 January 1601 */
static long long days_from_date(int year, int month, int day)
{
	static const int before_month[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
	long long years = year - 1601;
	long long days = years * 365 + years / 4 - years / 100 + years / 400;

	days += before_month[(month - 1) % 12] + day - 1;
	if (month > 2 && is_leap(year))
		days++;
	return days;
}

BOOL WINAPI SystemTimeToFileTime(const SYSTEMTIME *system, LPFILETIME file_time)
{
	long long days = days_from_date(system->wYear, system->wMonth, system->wDay);
	unsigned long long ticks = ((unsigned long long)days * 86400 + system->wHour * 3600 + system->wMinute * 60 +
		system->wSecond) * 10000000ULL + system->wMilliseconds * 10000ULL;

	file_time->dwLowDateTime = (DWORD)ticks;
	file_time->dwHighDateTime = (DWORD)(ticks >> 32);
	return TRUE;
}

BOOL WINAPI FileTimeToSystemTime(const FILETIME *file_time, LPSYSTEMTIME system)
{
	unsigned long long ticks = ((unsigned long long)file_time->dwHighDateTime << 32) | file_time->dwLowDateTime;
	unsigned long long seconds = ticks / 10000000ULL;
	long long days = (long long)(seconds / 86400);
	unsigned long rest = (unsigned long)(seconds % 86400);
	int year = 1601;

	memset(system, 0, sizeof(*system));
	system->wMilliseconds = (WORD)(ticks / 10000 % 1000);
	system->wHour = (WORD)(rest / 3600);
	system->wMinute = (WORD)(rest / 60 % 60);
	system->wSecond = (WORD)(rest % 60);
	/* 1 January 1601 was a Monday */
	system->wDayOfWeek = (WORD)((days + 1) % 7);
	for (;;)
	{
		int length = is_leap(year) ? 366 : 365;

		if (days < length)
			break;
		days -= length;
		year++;
	}
	system->wYear = (WORD)year;
	for (int month = 1; month <= 12; month++)
	{
		static const int lengths[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
		int length = lengths[month - 1] + (month == 2 && is_leap(year));

		if (days < length)
		{
			system->wMonth = (WORD)month;
			system->wDay = (WORD)(days + 1);
			break;
		}
		days -= length;
	}
	return TRUE;
}

BOOL WINAPI FileTimeToLocalFileTime(const FILETIME *file_time, LPFILETIME local)
{
	*local = *file_time;
	return TRUE;
}

BOOL WINAPI LocalFileTimeToFileTime(const FILETIME *local, LPFILETIME file_time)
{
	*file_time = *local;
	return TRUE;
}

void WINAPI GetLocalTime(LPSYSTEMTIME system)
{
	int year, month, day, day_of_week, hour, minute, second, hundredths;

	dos_date_time(&year, &month, &day, &day_of_week, &hour, &minute, &second, &hundredths);
	system->wYear = (WORD)year;
	system->wMonth = (WORD)month;
	system->wDay = (WORD)day;
	system->wDayOfWeek = (WORD)day_of_week;
	system->wHour = (WORD)hour;
	system->wMinute = (WORD)minute;
	system->wSecond = (WORD)second;
	system->wMilliseconds = (WORD)(hundredths * 10);
}

/* The coordinated time (UTC: GetSystemTime, the C runtime's time()) is
DOS's local time less its zone's offset. Internet play needs it right to the
minute: a game's listing carries its host's time, and a browser takes only
listings within ten minutes of its own (p2p_lobby.c). The offset is an
internet time server's once the network is up (dos_net.cpp: it also takes
in how far DOS's clock is off; without an answer, the setting time_zone's),
else DOS's TZ variable as mTCP reads it ("EST5EDT": hours west), else none.
(The settings are not read here: the C runtime asks the time before they
can be.) */

#define TICKS_A_SECOND 10000000LL

/* local less UTC, in 100 ns ticks, once the network's start set it */
static long long server_offset;
static int server_answered;

void dos_set_utc_offset(long long ticks)
{
	server_offset = ticks;
	server_answered = 1;
}

static long long utc_offset(void)
{
	const char *variable;

	if (server_answered)
		return server_offset;
	for (variable = dos_environment(); variable && *variable; variable += strlen(variable) + 1)
	{
		if ((variable[0] == 'T' || variable[0] == 't') && (variable[1] == 'Z' || variable[1] == 'z') &&
			variable[2] == '=')
		{
			const char *zone = variable + 3;

			while ((*zone >= 'A' && *zone <= 'Z') || (*zone >= 'a' && *zone <= 'z'))
				zone++;
			if ((*zone >= '0' && *zone <= '9') || *zone == '-' || *zone == '+')
				return -(long long)(atof(zone) * 3600.0) * TICKS_A_SECOND;
		}
	}
	return 0;
}

void WINAPI GetSystemTimeAsFileTime(LPFILETIME file_time)
{
	SYSTEMTIME local;
	FILETIME local_file_time;
	unsigned long long ticks;

	GetLocalTime(&local);
	SystemTimeToFileTime(&local, &local_file_time);
	ticks = (((unsigned long long)local_file_time.dwHighDateTime << 32) | local_file_time.dwLowDateTime) -
		(unsigned long long)utc_offset();
	file_time->dwLowDateTime = (DWORD)ticks;
	file_time->dwHighDateTime = (DWORD)(ticks >> 32);
}

void WINAPI GetSystemTime(LPSYSTEMTIME system)
{
	FILETIME file_time;

	GetSystemTimeAsFileTime(&file_time);
	FileTimeToSystemTime(&file_time, system);
}

/* the zone as Windows gives it (UTC = local + Bias minutes): the offset to
the quarter hour (the rest is how far DOS's clock is off) */
DWORD WINAPI GetTimeZoneInformation(LPTIME_ZONE_INFORMATION information)
{
	long long minutes = utc_offset() / (60 * TICKS_A_SECOND);

	memset(information, 0, sizeof(*information));
	minutes = (minutes >= 0 ? minutes + 7 : minutes - 7) / 15 * 15;
	information->Bias = -(LONG)minutes;
	return TIME_ZONE_ID_UNKNOWN;
}

/* ---------- one thread */

void WINAPI InitializeCriticalSection(LPCRITICAL_SECTION section)
{
	memset(section, 0, sizeof(*section));
}

void WINAPI DeleteCriticalSection(LPCRITICAL_SECTION section)
{
	(void)section;
}

void WINAPI EnterCriticalSection(LPCRITICAL_SECTION section)
{
	section->RecursionCount++;
}

void WINAPI LeaveCriticalSection(LPCRITICAL_SECTION section)
{
	section->RecursionCount--;
}

BOOL WINAPI TryEnterCriticalSection(LPCRITICAL_SECTION section)
{
	section->RecursionCount++;
	return TRUE;
}

HANDLE WINAPI CreateEventA(LPSECURITY_ATTRIBUTES security, BOOL manual_reset, BOOL initial_state, LPCSTR name)
{
	struct handle *entry;
	HANDLE result = handle_new(_handle_event, &entry);

	(void)security;
	(void)name;
	if (result != INVALID_HANDLE_VALUE)
	{
		entry->manual_reset = manual_reset;
		entry->signaled = initial_state;
		return result;
	}
	return NULL;
}

BOOL WINAPI SetEvent(HANDLE event)
{
	struct handle *handle = handle_get(event);

	if (!handle)
		return FALSE;
	handle->signaled = TRUE;
	return TRUE;
}

BOOL WINAPI ResetEvent(HANDLE event)
{
	struct handle *handle = handle_get(event);

	if (!handle)
		return FALSE;
	handle->signaled = FALSE;
	return TRUE;
}

DWORD WINAPI WaitForSingleObject(HANDLE object, DWORD milliseconds)
{
	struct handle *handle = handle_get(object);

	(void)milliseconds;
	if (!handle)
		return WAIT_FAILED;
	if (handle->type == _handle_event)
	{
		if (!handle->signaled)
			return WAIT_TIMEOUT;
		if (!handle->manual_reset)
			handle->signaled = FALSE;
	}
	return WAIT_OBJECT_0;
}

HANDLE WINAPI CreateThread(LPSECURITY_ATTRIBUTES security, SIZE_T stack_size, LPTHREAD_START_ROUTINE start,
	LPVOID parameter, DWORD flags, LPDWORD thread_id)
{
	(void)security;
	(void)stack_size;
	(void)start;
	(void)parameter;
	(void)flags;
	(void)thread_id;
	last_error = ERROR_NOT_SUPPORTED;
	return NULL;
}

/* ---------- memory (the C runtime's heap: dos_malloc.c) */

HLOCAL WINAPI LocalAlloc(UINT flags, SIZE_T size)
{
	void *memory = malloc(size ? size : 1);

	if (memory && (flags & LMEM_ZEROINIT))
		memset(memory, 0, size);
	return memory;
}

HLOCAL WINAPI LocalFree(HLOCAL memory)
{
	free(memory);
	return NULL;
}

HGLOBAL WINAPI GlobalAlloc(UINT flags, SIZE_T size)
{
	return LocalAlloc(flags & GMEM_ZEROINIT ? LMEM_ZEROINIT : 0, size);
}

HGLOBAL WINAPI GlobalFree(HGLOBAL memory)
{
	free(memory);
	return NULL;
}
