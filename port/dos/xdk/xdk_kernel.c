/*
XDK_KERNEL.C

The Win32 and XAPI kernel calls the game makes (files, time, events,
memory, the debug output), for DOS and Windows 95 alike. The game sees them
under the Windows build's names (halo_windows_api_names.h: CreateFileA is
halo_xbox_CreateFileA here), so that the Windows 95 build's Win32 half can
call the real ones.

Files are the C runtime's descriptors (_open, _read, ...), opened on Xbox
paths translated below the program's folder (platform_translate_path).
There is one thread, as on the N64: the game's own worker threads belong to
what these builds do themselves, so CreateThread makes a handle that never
runs, while bungie_net's short jobs (a profile written to disk) run to
their end when they are started (dos_thread_finished); reads complete
before ReadFile returns.
*/

#include "platform.h"

#include <ctype.h>
#include <direct.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "memory/zlib/zlib.h"

/* the directory search of the system (port/dos/sys/sys_files.c) */
int sys_find_first(const char *pattern, char *name, unsigned long name_size, unsigned long *attributes,
	unsigned long *size, void **search);
int sys_find_next(void *search, char *name, unsigned long name_size, unsigned long *attributes,
	unsigned long *size);
void sys_find_close(void *search);
void dos_map_close(void);

/* ---------- logging */

void platform_log(const char *format, ...)
{
	char line[512];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	system_log(line);
}

void platform_unimplemented(const char *name)
{
	static const char *reported[64];
	static int reported_count;
	int i;

	for (i = 0; i < reported_count; i++)
	{
		if (reported[i] == name)
			return;
	}
	if (reported_count < 64)
		reported[reported_count++] = name;
	platform_log("unimplemented: %s", name);
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
	if (length)
		system_log(line);
}

/* ---------- errors */

static DWORD last_error;

DWORD WINAPI GetLastError(void)
{
	return last_error;
}

VOID WINAPI SetLastError(DWORD error)
{
	last_error = error;
}

static void error_from_errno(void)
{
	switch (errno)
	{
	case ENOENT: last_error = ERROR_FILE_NOT_FOUND; break;
	case EEXIST: last_error = ERROR_ALREADY_EXISTS; break;
	case EACCES: last_error = ERROR_ACCESS_DENIED; break;
	case EMFILE: last_error = ERROR_TOO_MANY_OPEN_FILES; break;
	case ENOSPC: last_error = ERROR_DISK_FULL; break;
	case ENOTEMPTY: last_error = ERROR_DIR_NOT_EMPTY; break;
	default: last_error = ERROR_GEN_FAILURE; break;
	}
}

/* ---------- paths */

static char data_root[260];

const char *platform_data_root(void)
{
	if (!data_root[0])
	{
		strncpy(data_root, system_program_directory(), sizeof(data_root) - 1);
		data_root[sizeof(data_root) - 1] = 0;
	}
	return data_root;
}

/* makes each folder of a path that does not exist yet (not the last part) */
static void make_parent_folders(const char *path)
{
	char partial[300];
	size_t length = strlen(path);

	if (length >= sizeof(partial))
		return;
	for (size_t i = 3; i < length; i++)
	{
		if (path[i] == '\\')
		{
			memcpy(partial, path, i);
			partial[i] = 0;
			_mkdir(partial);
		}
	}
}

void platform_translate_path(const char *xbox_path, char *host_path, unsigned long host_path_size)
{
	const char *rest = xbox_path;
	const char *folder = "";
	char drive = 0;
	size_t length;

	if (isalpha((unsigned char)rest[0]) && rest[1] == ':')
	{
		drive = (char)tolower((unsigned char)rest[0]);
		rest += 2;
	}
	while (*rest == '\\' || *rest == '/')
		rest++;
	switch (drive)
	{
	case 't': case 'u': folder = "SAVE\\"; break;
	case 'z': case 'y': case 'x': folder = "CACHE\\"; break;
	default: break;
	}
	snprintf(host_path, host_path_size, "%s\\%s%s", platform_data_root(), folder, rest);
	length = strlen(host_path);
	for (size_t i = 0; i < length; i++)
	{
		if (host_path[i] == '/')
			host_path[i] = '\\';
	}
	if (folder[0])
		make_parent_folders(host_path);
}

/* ---------- handles */

enum
{
	_handle_free,
	_handle_file,
	_handle_event,
	_handle_thread,
	_handle_find,
	_handle_mutex,
};

#define MAXIMUM_HANDLES 64

struct handle
{
	long type;
	int descriptor;
	BOOL signaled;
	BOOL manual_reset;
	void *search;
	/* a thread's */
	DWORD exit_code;
};

static struct handle handles[MAXIMUM_HANDLES];

static struct handle *handle_new(long type)
{
	for (int i = 0; i < MAXIMUM_HANDLES; i++)
	{
		if (handles[i].type == _handle_free)
		{
			memset(&handles[i], 0, sizeof(handles[i]));
			handles[i].type = type;
			handles[i].descriptor = -1;
			return &handles[i];
		}
	}
	last_error = ERROR_TOO_MANY_OPEN_FILES;
	return NULL;
}

static struct handle *handle_get(HANDLE value)
{
	struct handle *handle = (struct handle *)value;

	if (handle < handles || handle >= handles + MAXIMUM_HANDLES || handle->type == _handle_free)
	{
		last_error = ERROR_INVALID_HANDLE;
		return NULL;
	}
	return handle;
}

BOOL WINAPI CloseHandle(HANDLE object)
{
	struct handle *handle = handle_get(object);

	if (!handle)
		return FALSE;
	if (handle->type == _handle_file && handle->descriptor >= 0)
		_close(handle->descriptor);
	if (handle->type == _handle_find && handle->search)
		sys_find_close(handle->search);
	handle->type = _handle_free;
	return TRUE;
}

/* ---------- files */

HANDLE WINAPI CreateFileA(LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
	DWORD disposition, DWORD flags, HANDLE template_file)
{
	char host_path[300];
	struct handle *handle;
	int open_flags = _O_BINARY;
	int descriptor;

	(void)share;
	(void)security;
	(void)flags;
	(void)template_file;
	platform_translate_path(path, host_path, sizeof(host_path));
	if ((access & GENERIC_READ) && (access & GENERIC_WRITE))
		open_flags |= _O_RDWR;
	else if (access & GENERIC_WRITE)
		open_flags |= _O_WRONLY;
	else
		open_flags |= _O_RDONLY;
	switch (disposition)
	{
	case CREATE_NEW: open_flags |= _O_CREAT | _O_EXCL; break;
	case CREATE_ALWAYS: open_flags |= _O_CREAT | _O_TRUNC; break;
	case OPEN_ALWAYS: open_flags |= _O_CREAT; break;
	case TRUNCATE_EXISTING: open_flags |= _O_TRUNC; break;
	default: break;
	}
	if (open_flags & _O_CREAT)
		make_parent_folders(host_path);
	descriptor = _open(host_path, open_flags, _S_IREAD | _S_IWRITE);
	if (descriptor < 0)
	{
		error_from_errno();
		return INVALID_HANDLE_VALUE;
	}
	handle = handle_new(_handle_file);
	if (!handle)
	{
		_close(descriptor);
		return INVALID_HANDLE_VALUE;
	}
	handle->descriptor = descriptor;
	last_error = 0;
	return (HANDLE)handle;
}

/* an overlapped transfer completes at once: its result and its event */
static void complete(LPOVERLAPPED overlapped, DWORD done)
{
	if (overlapped)
	{
		overlapped->Internal = 0;
		overlapped->InternalHigh = done;
		if (overlapped->hEvent)
			SetEvent(overlapped->hEvent);
	}
}

BOOL WINAPI ReadFile(HANDLE file, LPVOID buffer, DWORD size, LPDWORD bytes_read, LPOVERLAPPED overlapped)
{
	struct handle *handle = handle_get(file);
	int count;

	if (bytes_read)
		*bytes_read = 0;
	if (!handle || handle->type != _handle_file)
		return FALSE;
	if (overlapped)
		_lseeki64(handle->descriptor, (long long)overlapped->Offset | ((long long)overlapped->OffsetHigh << 32),
			SEEK_SET);
	count = _read(handle->descriptor, buffer, size);
	if (count < 0)
	{
		error_from_errno();
		return FALSE;
	}
	if (bytes_read)
		*bytes_read = (DWORD)count;
	complete(overlapped, (DWORD)count);
	return TRUE;
}

BOOL WINAPI WriteFile(HANDLE file, LPCVOID buffer, DWORD size, LPDWORD written, LPOVERLAPPED overlapped)
{
	struct handle *handle = handle_get(file);
	int count;

	if (written)
		*written = 0;
	if (!handle || handle->type != _handle_file)
		return FALSE;
	if (overlapped)
		_lseeki64(handle->descriptor, (long long)overlapped->Offset | ((long long)overlapped->OffsetHigh << 32),
			SEEK_SET);
	count = _write(handle->descriptor, buffer, size);
	if (count < 0)
	{
		error_from_errno();
		return FALSE;
	}
	if (written)
		*written = (DWORD)count;
	complete(overlapped, (DWORD)count);
	return TRUE;
}

DWORD WINAPI SetFilePointer(HANDLE file, LONG distance, PLONG distance_high, DWORD method)
{
	struct handle *handle = handle_get(file);
	long long offset = distance_high ? ((long long)*distance_high << 32) | (unsigned long)distance : distance;
	long long position;

	if (!handle || handle->type != _handle_file)
		return INVALID_SET_FILE_POINTER;
	position = _lseeki64(handle->descriptor, offset,
		method == FILE_BEGIN ? SEEK_SET : method == FILE_CURRENT ? SEEK_CUR : SEEK_END);
	if (position < 0)
	{
		error_from_errno();
		return INVALID_SET_FILE_POINTER;
	}
	if (distance_high)
		*distance_high = (LONG)(position >> 32);
	last_error = 0;
	return (DWORD)position;
}

DWORD WINAPI GetFileSize(HANDLE file, LPDWORD size_high)
{
	struct handle *handle = handle_get(file);
	long long size;

	if (size_high)
		*size_high = 0;
	if (!handle || handle->type != _handle_file)
		return INVALID_FILE_SIZE;
	size = _filelengthi64(handle->descriptor);
	if (size < 0)
		return INVALID_FILE_SIZE;
	if (size_high)
		*size_high = (DWORD)(size >> 32);
	return (DWORD)size;
}

/* (the map packs' list compares its maps' times: no times here, so it reads
them again) */
BOOL WINAPI GetFileTime(HANDLE file, LPFILETIME creation, LPFILETIME access, LPFILETIME write)
{
	(void)file;
	if (creation)
		memset(creation, 0, sizeof(*creation));
	if (access)
		memset(access, 0, sizeof(*access));
	if (write)
		memset(write, 0, sizeof(*write));
	return TRUE;
}

BOOL WINAPI SetEndOfFile(HANDLE file)
{
	struct handle *handle = handle_get(file);
	long long position;

	if (!handle || handle->type != _handle_file)
		return FALSE;
	position = _lseeki64(handle->descriptor, 0, SEEK_CUR);
	if (position < 0 || _chsize_s(handle->descriptor, position) != 0)
	{
		error_from_errno();
		return FALSE;
	}
	return TRUE;
}

BOOL WINAPI DeleteFileA(LPCSTR path)
{
	char host_path[300];

	platform_translate_path(path, host_path, sizeof(host_path));
	if (_unlink(host_path) != 0)
	{
		error_from_errno();
		return FALSE;
	}
	return TRUE;
}

BOOL WINAPI MoveFileA(LPCSTR from, LPCSTR to)
{
	char host_from[300], host_to[300];

	platform_translate_path(from, host_from, sizeof(host_from));
	platform_translate_path(to, host_to, sizeof(host_to));
	if (rename(host_from, host_to) != 0)
	{
		error_from_errno();
		return FALSE;
	}
	return TRUE;
}

BOOL WINAPI CreateDirectoryA(LPCSTR path, LPSECURITY_ATTRIBUTES security)
{
	char host_path[300];

	(void)security;
	platform_translate_path(path, host_path, sizeof(host_path));
	if (_mkdir(host_path) != 0)
	{
		error_from_errno();
		return FALSE;
	}
	return TRUE;
}

BOOL WINAPI RemoveDirectoryA(LPCSTR path)
{
	char host_path[300];

	platform_translate_path(path, host_path, sizeof(host_path));
	if (_rmdir(host_path) != 0)
	{
		error_from_errno();
		return FALSE;
	}
	return TRUE;
}

DWORD WINAPI GetFileAttributesA(LPCSTR path)
{
	char host_path[300];
	struct _stat status;

	platform_translate_path(path, host_path, sizeof(host_path));
	if (_stat(host_path, &status) != 0)
	{
		error_from_errno();
		return 0xFFFFFFFF;
	}
	return (status.st_mode & _S_IFDIR) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}

BOOL WINAPI GetFileAttributesExA(LPCSTR path, GET_FILEEX_INFO_LEVELS level, LPVOID information)
{
	char host_path[300];
	struct _stat status;
	WIN32_FILE_ATTRIBUTE_DATA *data = information;

	(void)level;
	platform_translate_path(path, host_path, sizeof(host_path));
	if (_stat(host_path, &status) != 0)
	{
		error_from_errno();
		return FALSE;
	}
	memset(data, 0, sizeof(*data));
	data->dwFileAttributes = (status.st_mode & _S_IFDIR) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
	data->nFileSizeLow = (DWORD)status.st_size;
	return TRUE;
}

BOOL WINAPI SetFileAttributesA(LPCSTR path, DWORD attributes)
{
	(void)path;
	(void)attributes;
	return TRUE;
}

static void fill_find_data(WIN32_FIND_DATAA *data, const char *name, unsigned long attributes, unsigned long size)
{
	memset(data, 0, sizeof(*data));
	data->dwFileAttributes = attributes ? attributes : FILE_ATTRIBUTE_NORMAL;
	data->nFileSizeLow = size;
	strncpy(data->cFileName, name, sizeof(data->cFileName) - 1);
}

HANDLE WINAPI FindFirstFileA(LPCSTR pattern, LPWIN32_FIND_DATAA data)
{
	char host_pattern[300], name[260];
	unsigned long attributes, size;
	void *search;
	struct handle *handle;

	platform_translate_path(pattern, host_pattern, sizeof(host_pattern));
	if (!sys_find_first(host_pattern, name, sizeof(name), &attributes, &size, &search))
	{
		last_error = ERROR_FILE_NOT_FOUND;
		return INVALID_HANDLE_VALUE;
	}
	handle = handle_new(_handle_find);
	if (!handle)
	{
		sys_find_close(search);
		return INVALID_HANDLE_VALUE;
	}
	handle->search = search;
	fill_find_data(data, name, attributes, size);
	return (HANDLE)handle;
}

BOOL WINAPI FindNextFileA(HANDLE find, LPWIN32_FIND_DATAA data)
{
	struct handle *handle = handle_get(find);
	char name[260];
	unsigned long attributes, size;

	if (!handle || handle->type != _handle_find)
		return FALSE;
	if (!sys_find_next(handle->search, name, sizeof(name), &attributes, &size))
	{
		last_error = ERROR_NO_MORE_FILES;
		return FALSE;
	}
	fill_find_data(data, name, attributes, size);
	return TRUE;
}

BOOL WINAPI FindClose(HANDLE find)
{
	return CloseHandle(find);
}

/* ---------- events and threads */

HANDLE WINAPI CreateEventA(LPSECURITY_ATTRIBUTES security, BOOL manual_reset, BOOL initial_state, LPCSTR name)
{
	struct handle *handle = handle_new(_handle_event);

	(void)security;
	(void)name;
	if (!handle)
		return NULL;
	handle->manual_reset = manual_reset;
	handle->signaled = initial_state;
	return (HANDLE)handle;
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

/* A thread the game makes never runs: the game's long-lived ones (the
keyboard's) belong to what these builds do themselves. Its short jobs (a
profile written, bungie_net's create_thread) run to their end where they
are started instead, as one already finished (dos_thread_finished). */
HANDLE WINAPI CreateThread(LPSECURITY_ATTRIBUTES security, DWORD stack_size, LPTHREAD_START_ROUTINE start,
	LPVOID parameter, DWORD flags, LPDWORD thread_id)
{
	struct handle *handle = handle_new(_handle_thread);

	(void)security;
	(void)stack_size;
	(void)start;
	(void)parameter;
	(void)flags;
	if (!handle)
		return NULL;
	if (thread_id)
		*thread_id = 2;
	handle->exit_code = STILL_ACTIVE;
	return (HANDLE)handle;
}

/* thread_win32.c's create_thread (HALO_DOS): the job done, its thread one
that has ended with this exit code */
HANDLE dos_thread_finished(DWORD exit_code)
{
	struct handle *handle = handle_new(_handle_thread);

	if (!handle)
		return NULL;
	handle->exit_code = exit_code;
	handle->signaled = TRUE;
	return (HANDLE)handle;
}

DWORD WINAPI ResumeThread(HANDLE thread)
{
	return handle_get(thread) ? 1 : (DWORD)-1;
}

BOOL WINAPI GetExitCodeThread(HANDLE thread, LPDWORD exit_code)
{
	struct handle *handle = handle_get(thread);

	if (!handle || handle->type != _handle_thread)
		return FALSE;
	*exit_code = handle->exit_code;
	return TRUE;
}

/* (one thread: a mutex is always free) */
HANDLE WINAPI CreateMutexA(LPSECURITY_ATTRIBUTES security, BOOL initial_owner, LPCSTR name)
{
	(void)security;
	(void)initial_owner;
	(void)name;
	return (HANDLE)handle_new(_handle_mutex);
}

BOOL WINAPI ReleaseMutex(HANDLE mutex)
{
	return handle_get(mutex) != NULL;
}

BOOL WINAPI SetThreadPriority(HANDLE thread, int priority)
{
	(void)thread;
	(void)priority;
	return TRUE;
}

BOOL WINAPI SwitchToThread(VOID)
{
	return FALSE;
}

/* internet play's pass (p2p.c, HALO_DOS): run wherever the game waits too,
not only from its frames' drawing (dos_network_update): the host waiting
for its players to load draws nothing, and the game's own waits for a
stream to drain need the pass that drains it */
void dos_p2p_poll(void);

VOID WINAPI Sleep(DWORD milliseconds)
{
	dos_p2p_poll();
	dsound_pump();
	system_sleep(milliseconds);
}

/* ---------- time */

BOOL WINAPI QueryPerformanceCounter(LARGE_INTEGER *counter)
{
	counter->QuadPart = (LONGLONG)system_microseconds();
	return TRUE;
}

BOOL WINAPI QueryPerformanceFrequency(LARGE_INTEGER *frequency)
{
	frequency->QuadPart = 1000000;
	return TRUE;
}

DWORD WINAPI GetTickCount(VOID)
{
	return (DWORD)(system_microseconds() / 1000);
}

VOID WINAPI GetSystemTime(LPSYSTEMTIME system_time)
{
	time_t now = time(NULL);
	struct tm *parts = gmtime(&now);

	memset(system_time, 0, sizeof(*system_time));
	if (parts)
	{
		system_time->wYear = (WORD)(parts->tm_year + 1900);
		system_time->wMonth = (WORD)(parts->tm_mon + 1);
		system_time->wDayOfWeek = (WORD)parts->tm_wday;
		system_time->wDay = (WORD)parts->tm_mday;
		system_time->wHour = (WORD)parts->tm_hour;
		system_time->wMinute = (WORD)parts->tm_min;
		system_time->wSecond = (WORD)parts->tm_sec;
	}
}

/* ---------- memory */

LPVOID WINAPI XPhysicalAlloc(SIZE_T size, ULONG_PTR address, ULONG_PTR alignment, DWORD protect)
{
	void *result;

	(void)address;
	(void)protect;
	if (alignment < 16)
		alignment = 16;
	result = aligned_alloc(alignment, (size + alignment - 1) & ~(alignment - 1));
	if (!result)
		platform_log("XPhysicalAlloc(%lu) failed", (unsigned long)size);
	return result;
}

VOID WINAPI XPhysicalFree(LPVOID address)
{
	free(address);
}

VOID WINAPI XPhysicalProtect(LPVOID address, SIZE_T size, DWORD protect)
{
	(void)address;
	(void)size;
	(void)protect;
}

DWORD WINAPI XQueryMemoryProtect(LPVOID address)
{
	(void)address;
	return PAGE_READWRITE;
}

BOOL WINAPI VirtualProtect(LPVOID address, SIZE_T size, DWORD protect, PDWORD old_protect)
{
	(void)address;
	(void)size;
	(void)protect;
	if (old_protect)
		*old_protect = PAGE_READWRITE;
	return TRUE;
}

/* the game's own GlobalAlloc users keep their block's size (LocalSize) */
struct sized_block
{
	SIZE_T size;
	SIZE_T pad[3];
};

HGLOBAL WINAPI GlobalAlloc(UINT flags, SIZE_T size)
{
	struct sized_block *block = malloc(sizeof(*block) + (size ? size : 1));

	if (!block)
		return NULL;
	block->size = size;
	if (flags & GMEM_ZEROINIT)
		memset(block + 1, 0, size);
	return (HGLOBAL)(block + 1);
}

HGLOBAL WINAPI GlobalReAlloc(HGLOBAL memory, SIZE_T size, UINT flags)
{
	struct sized_block *block;

	(void)flags;
	if (!memory)
		return GlobalAlloc(flags, size);
	block = realloc((struct sized_block *)memory - 1, sizeof(*block) + (size ? size : 1));
	if (!block)
		return NULL;
	block->size = size;
	return (HGLOBAL)(block + 1);
}

HLOCAL WINAPI LocalFree(HLOCAL memory)
{
	if (memory)
		free((struct sized_block *)memory - 1);
	return NULL;
}

SIZE_T WINAPI LocalSize(HLOCAL memory)
{
	return memory ? ((struct sized_block *)memory - 1)->size : 0;
}

VOID WINAPI GlobalMemoryStatus(LPMEMORYSTATUS status)
{
	memset(status, 0, sizeof(*status));
	status->dwLength = sizeof(*status);
	status->dwTotalPhys = 64 * 1024 * 1024;
	status->dwAvailPhys = 32 * 1024 * 1024;
}

/* ---------- the XAPI and the debug monitor */

DWORD WINAPI XGetLaunchInfo(PDWORD type, PLAUNCH_DATA data)
{
	(void)data;
	*type = 0;
	return ERROR_FILE_NOT_FOUND;
}

DWORD WINAPI XGetLanguage(VOID)
{
	return XC_LANGUAGE_ENGLISH;
}

HRESULT WINAPI DmWalkLoadedModules(PDM_WALK_MODULES *walk, PDMN_MODLOAD module)
{
	(void)walk;
	(void)module;
	return XBDM_ENDOFLIST;
}

HRESULT WINAPI DmWalkModuleSections(PDM_WALK_MODSECT *walk, LPCSTR module_name, PDMN_SECTIONLOAD section)
{
	(void)walk;
	(void)module_name;
	(void)section;
	return XBDM_ENDOFLIST;
}

HRESULT WINAPI DmCloseModuleSections(PDM_WALK_MODSECT walk)
{
	(void)walk;
	return S_OK;
}

/* ---------- the map file (port/dos/game: cache_file_open, the renderer's
textures), for the game-side units, which see no Win32 types

A map from the Xbox's disc is its 2 KB header and then one zlib stream of
the rest; the console decompressed it to its hard disk before playing it
(cache/cache_files_decompress_windows.c). So does this layer: into the
CACHE folder beside the program (z:\maps\), once, the next games reading
that copy. A map whose file is already its whole length is read as it is. */

static HANDLE map_file = INVALID_HANDLE_VALUE;

/* the game-side glue shows how far it has come (dos_render_game.c) */
void dos_loading_progress(const char *what, long permille);

/* the header's file_length (cache_files.c's struct cache_file_header) */
#define HEADER_FILE_LENGTH 8
#define DECOMPRESS_CHUNK 0x10000

static HANDLE open_reading(const char *path)
{
	return CreateFileA(path, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
}

/* the decompressed copy of a compressed map, made if it is not there (or not
whole); INVALID_HANDLE_VALUE if it cannot be */
static HANDLE decompressed_map(const char *map_name, HANDLE source, const void *header, unsigned long header_size)
{
	char path[128], what[64];
	unsigned long length = *(const unsigned long *)((const char *)header + HEADER_FILE_LENGTH);
	unsigned long source_size = GetFileSize(source, NULL), consumed = header_size;
	unsigned char *in = NULL, *out = NULL, *existing_header = NULL;
	HANDLE copy;
	z_stream stream;
	DWORD done = 0;
	int result = Z_OK;

	snprintf(path, sizeof(path), "z:\\maps\\%s.map", map_name);
	copy = open_reading(path);
	if (copy != INVALID_HANDLE_VALUE)
	{
		existing_header = malloc(header_size);
		if (existing_header && GetFileSize(copy, NULL) == length &&
			ReadFile(copy, existing_header, header_size, &done, NULL) && done == header_size &&
			!memcmp(existing_header, header, header_size))
		{
			free(existing_header);
			return copy;
		}
		free(existing_header);
		CloseHandle(copy);
	}

	snprintf(what, sizeof(what), "Decompressing %s for its first game", map_name);
	platform_log("map: %s (%lu KB to %lu KB, into CACHE\\MAPS)", what, source_size / 1024, length / 1024);
	copy = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
	in = malloc(DECOMPRESS_CHUNK);
	out = malloc(DECOMPRESS_CHUNK);
	memset(&stream, 0, sizeof(stream));
	if (copy == INVALID_HANDLE_VALUE || !in || !out || inflateInit(&stream) != Z_OK ||
		!WriteFile(copy, header, header_size, &done, NULL) || done != header_size)
	{
		free(in);
		free(out);
		if (copy != INVALID_HANDLE_VALUE)
			CloseHandle(copy);
		system_error("Could not decompress the map into the CACHE folder (is the disk full?).");
		return INVALID_HANDLE_VALUE;
	}
	SetFilePointer(source, (LONG)header_size, NULL, FILE_BEGIN);
	while (result != Z_STREAM_END)
	{
		if (!stream.avail_in)
		{
			DWORD got = 0;

			if (!ReadFile(source, in, DECOMPRESS_CHUNK, &got, NULL) || !got)
				break;
			stream.next_in = in;
			stream.avail_in = got;
			consumed += got;
			dos_loading_progress(what, (long)((unsigned long long)consumed * 1000 / (source_size ? source_size : 1)));
		}
		stream.next_out = out;
		stream.avail_out = DECOMPRESS_CHUNK;
		result = inflate(&stream, Z_NO_FLUSH);
		if (result != Z_OK && result != Z_STREAM_END)
			break;
		if (!WriteFile(copy, out, DECOMPRESS_CHUNK - stream.avail_out, &done, NULL) ||
			done != DECOMPRESS_CHUNK - stream.avail_out)
		{
			result = Z_ERRNO;
			break;
		}
	}
	inflateEnd(&stream);
	free(in);
	free(out);
	CloseHandle(copy);
	if (result != Z_STREAM_END)
	{
		DeleteFileA(path);
		system_error("The map would not decompress (a damaged map file, or the disk is full).");
		return INVALID_HANDLE_VALUE;
	}
	return open_reading(path);
}

/* whether a map is in the maps folder (the menu lists those that are) */
int dos_map_exists(const char *map_name)
{
	char path[128];
	HANDLE file;

	snprintf(path, sizeof(path), "d:\\maps\\%s.map", map_name);
	file = open_reading(path);
	if (file == INVALID_HANDLE_VALUE)
		return FALSE;
	CloseHandle(file);
	return TRUE;
}

static void close_map(HANDLE *file)
{
	if (*file != INVALID_HANDLE_VALUE)
	{
		CloseHandle(*file);
		*file = INVALID_HANDLE_VALUE;
	}
}

static int open_map(HANDLE *file, const char *map_name, void *header, unsigned long header_size)
{
	char path[128];
	DWORD done = 0;

	close_map(file);
	snprintf(path, sizeof(path), "d:\\maps\\%s.map", map_name);
	*file = open_reading(path);
	if (*file == INVALID_HANDLE_VALUE)
		return FALSE;
	if (!ReadFile(*file, header, header_size, &done, NULL) || done != header_size)
	{
		close_map(file);
		return FALSE;
	}
	if (GetFileSize(*file, NULL) < *(const unsigned long *)((const char *)header + HEADER_FILE_LENGTH))
	{
		HANDLE copy = decompressed_map(map_name, *file, header, header_size);

		CloseHandle(*file);
		*file = copy;
		if (*file == INVALID_HANDLE_VALUE)
			return FALSE;
	}
	return TRUE;
}

/* (a big read, a map's tag data, in pieces: the loading screen's bar moves
between them, dos_render_game.c) */
#define READ_PIECE 0x40000

void dos_loading_read(unsigned long done, unsigned long total);

static int read_map(HANDLE file, unsigned long offset, void *buffer, unsigned long size)
{
	DWORD done = 0;
	unsigned long read = 0;

	if (file == INVALID_HANDLE_VALUE ||
		SetFilePointer(file, (LONG)offset, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER)
	{
		return FALSE;
	}
	while (read < size)
	{
		unsigned long piece = size - read > READ_PIECE ? READ_PIECE : size - read;

		if (!ReadFile(file, (char *)buffer + read, piece, &done, NULL) || done != piece)
			return FALSE;
		read += piece;
		if (size > 4 * READ_PIECE)
			dos_loading_read(read, size);
	}
	return TRUE;
}

int dos_map_open(const char *map_name, void *header, unsigned long header_size)
{
	return open_map(&map_file, map_name, header, header_size);
}

int dos_map_read(unsigned long offset, void *buffer, unsigned long size)
{
	return read_map(map_file, offset, buffer, size);
}

/* a Halo Custom Edition map (port/dos/game/dos_ce.c), by its path (an Xbox
path: d:\maps_ce\<name>.map), read where it is: Halo PC's maps are not
compressed */
int dos_map_open_path(const char *path, void *header, unsigned long header_size)
{
	DWORD done = 0;

	close_map(&map_file);
	map_file = open_reading(path);
	if (map_file == INVALID_HANDLE_VALUE)
		return FALSE;
	if (!ReadFile(map_file, header, header_size, &done, NULL) || done != header_size)
	{
		close_map(&map_file);
		return FALSE;
	}
	return TRUE;
}

/* the open map's file, for the Custom Edition maps' checks and sounds
(ChupathingyCE's ce_map_checks.c and ce_resources.c read it themselves) */
void *dos_map_handle(void)
{
	return map_file != INVALID_HANDLE_VALUE ? map_file : NULL;
}

unsigned long dos_map_size(void)
{
	return map_file != INVALID_HANDLE_VALUE ? GetFileSize(map_file, NULL) : 0;
}

void dos_map_close(void)
{
	close_map(&map_file);
}

/* a second map whose tags join the one being played (Firefight's
Covenant, from a campaign map: port/dos/game/dos_import.c), read beside it */
static HANDLE import_file = INVALID_HANDLE_VALUE;

int dos_import_map_open(const char *map_name, void *header, unsigned long header_size)
{
	return open_map(&import_file, map_name, header, header_size);
}

int dos_import_map_read(unsigned long offset, void *buffer, unsigned long size)
{
	return read_map(import_file, offset, buffer, size);
}

void dos_import_map_close(void)
{
	close_map(&import_file);
}

/* ---------- the narrow file calls the wide runtime makes (port/linux/src/
msvc_wide.c: _wfopen, _wremove...), on Xbox paths as the game's own are
(port/windows/src/windows_crt.c) */

FILE *halo_windows_fopen(const char *path, const char *mode);
FILE *halo_windows_freopen(const char *path, const char *mode, FILE *stream);
int halo_windows_remove(const char *path);

FILE *halo_linux_fopen(const char *path, const char *mode)
{
	return halo_windows_fopen(path, mode);
}

FILE *halo_linux_freopen(const char *path, const char *mode, FILE *stream)
{
	return halo_windows_freopen(path, mode, stream);
}

int halo_linux_remove(const char *path)
{
	return halo_windows_remove(path);
}
