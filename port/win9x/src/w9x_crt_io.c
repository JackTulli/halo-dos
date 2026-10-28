/*
W9X_CRT_IO.C

The Microsoft C runtime's low-level I/O for the 9x build: descriptors over
KERNEL32 file handles, for the game's libtiff (crt/io.h of the Xbox build)
and for the Windows build's platform units, which take _get_osfhandle,
_lseeki64, _chsize_s and the rest from the UCRT (port/windows/src/
win32_files.c, win32_posix.c, windows_crt.c), and the directory functions.

Windows 9x differs from NT here in what CreateFileA takes: no
FILE_SHARE_DELETE, no directories, no offsets in an OVERLAPPED on a disk
file. So files are shared for reading and writing only, and pread and
pwrite (w9x_pread, w9x_pwrite) hold the descriptor's lock across the seek
and the transfer. Names may use '/', which is turned into '\' here, the
boundary where the platform layer's host paths meet KERNEL32.

Descriptors 0, 1 and 2 are the process's standard handles. Each descriptor
has a lock of its own (a CRITICAL_SECTION made the first time its slot is
used, and kept for the slot's next file), so one thread's seek and read
cannot interleave with another's on the same file. Files are always
binary, as the Windows build makes them (_set_fmode(_O_BINARY)).
*/

#include <windows.h>

#include <direct.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "w9x_runtime.h"

/* the PDCLib glue's (port/win9x/pdclib/functions/_PDCLIB) */
char *_PDCLIB_w9x_path(const char *name, char *buffer);
int _PDCLIB_w32errno(unsigned long werror);

#define PATH_SIZE 260
#define MAXIMUM_DESCRIPTORS 256

struct descriptor
{
	HANDLE handle;
	int flags;
	/* whether the slot is in use; whether its lock is made */
	int open;
	int lock_made;
	CRITICAL_SECTION lock;
};

static struct descriptor descriptors[MAXIMUM_DESCRIPTORS];

/* the table's own lock, made on first use: 0 none, 1 being made, 2 made */
static CRITICAL_SECTION table_lock;
static volatile LONG table_state;

static void lock_table(void)
{
	if (table_state != 2)
	{
		if (InterlockedCompareExchange(&table_state, 1, 0) == 0)
		{
			InitializeCriticalSection(&table_lock);
			InterlockedExchange(&table_state, 2);
		}
		else
		{
			while (table_state != 2)
				Sleep(1);
		}
	}
	EnterCriticalSection(&table_lock);
}

static void unlock_table(void)
{
	LeaveCriticalSection(&table_lock);
}

static int set_error(int error)
{
	errno = error;
	return -1;
}

static int set_win32_error(void)
{
	errno = _PDCLIB_w32errno(GetLastError());
	return -1;
}

/* the open descriptor's entry, locked; NULL (EBADF) when it is not open */
static struct descriptor *take(int handle)
{
	struct descriptor *entry;

	if (handle < 0 || handle >= MAXIMUM_DESCRIPTORS)
	{
		errno = EBADF;
		return NULL;
	}
	entry = &descriptors[handle];
	lock_table();
	if (!entry->open && handle <= 2)
	{
		/* the standard handles, on first use */
		HANDLE standard = GetStdHandle(handle == 0 ? STD_INPUT_HANDLE : handle == 1 ? STD_OUTPUT_HANDLE :
			STD_ERROR_HANDLE);

		if (standard && standard != INVALID_HANDLE_VALUE)
		{
			if (!entry->lock_made)
			{
				InitializeCriticalSection(&entry->lock);
				entry->lock_made = 1;
			}
			entry->handle = standard;
			entry->flags = handle == 0 ? _O_RDONLY : _O_WRONLY;
			entry->open = 1;
		}
	}
	if (!entry->open)
	{
		unlock_table();
		errno = EBADF;
		return NULL;
	}
	unlock_table();
	/* (a slot's lock lives as long as the program: a close while another
	thread waits here leaves it a closed descriptor, which it sees) */
	EnterCriticalSection(&entry->lock);
	if (!entry->open)
	{
		LeaveCriticalSection(&entry->lock);
		errno = EBADF;
		return NULL;
	}
	return entry;
}

static void give_back(struct descriptor *entry)
{
	LeaveCriticalSection(&entry->lock);
}

/* a free slot for handle, or -1 (EMFILE) */
static int add_descriptor(HANDLE handle, int flags)
{
	int number;

	lock_table();
	for (number = 3; number < MAXIMUM_DESCRIPTORS; number++)
	{
		struct descriptor *entry = &descriptors[number];

		if (entry->open)
			continue;
		if (!entry->lock_made)
		{
			InitializeCriticalSection(&entry->lock);
			entry->lock_made = 1;
		}
		entry->handle = handle;
		entry->flags = flags;
		entry->open = 1;
		unlock_table();
		return number;
	}
	unlock_table();
	return set_error(EMFILE);
}

static const char *host_path(const char *path, char *buffer)
{
	if (!path)
	{
		errno = EINVAL;
		return NULL;
	}
	return _PDCLIB_w9x_path(path, buffer);
}

/* seconds since 1970 of a FILETIME (UTC); 0 for none */
static time_t time_of(const FILETIME *file_time)
{
	unsigned long long ticks = ((unsigned long long)file_time->dwHighDateTime << 32) | file_time->dwLowDateTime;

	if (ticks < 116444736000000000ULL)
		return 0;
	return (time_t)((ticks - 116444736000000000ULL) / 10000000ULL);
}

/* ---------- opening and closing */

int _open(const char *path, int flags, ...)
{
	char buffer[PATH_SIZE];
	DWORD access, disposition, attributes = FILE_ATTRIBUTE_NORMAL;
	HANDLE handle;
	int number;

	if (!host_path(path, buffer))
		return -1;
	switch (flags & (_O_RDONLY | _O_WRONLY | _O_RDWR))
	{
	case _O_RDONLY: access = GENERIC_READ; break;
	case _O_WRONLY: access = GENERIC_WRITE; break;
	case _O_RDWR: access = GENERIC_READ | GENERIC_WRITE; break;
	default: return set_error(EINVAL);
	}
	if (flags & _O_CREAT)
	{
		va_list arguments;
		int mode;

		va_start(arguments, flags);
		mode = va_arg(arguments, int);
		va_end(arguments);
		if (!(mode & _S_IWRITE))
			attributes = FILE_ATTRIBUTE_READONLY;
		disposition = (flags & _O_EXCL) ? CREATE_NEW : (flags & _O_TRUNC) ? CREATE_ALWAYS : OPEN_ALWAYS;
	}
	else
		disposition = (flags & _O_TRUNC) ? TRUNCATE_EXISTING : OPEN_EXISTING;
	/* _O_NOINHERIT, _O_SEQUENTIAL, _O_RANDOM, _O_TEXT and the like: no
	difference here (no child processes; always binary) */
	handle = CreateFileA(buffer, access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, disposition, attributes, NULL);
	if (handle == INVALID_HANDLE_VALUE)
	{
		/* (a directory cannot be opened on 9x) */
		DWORD attributes_found = GetFileAttributesA(buffer);

		if (attributes_found != INVALID_FILE_ATTRIBUTES && (attributes_found & FILE_ATTRIBUTE_DIRECTORY))
			return set_error(EACCES);
		return set_win32_error();
	}
	number = add_descriptor(handle, flags);
	if (number < 0)
		CloseHandle(handle);
	return number;
}

int _close(int handle)
{
	struct descriptor *entry = take(handle);
	HANDLE file;

	if (!entry)
		return -1;
	file = entry->handle;
	lock_table();
	entry->open = 0;
	entry->handle = INVALID_HANDLE_VALUE;
	unlock_table();
	give_back(entry);
	/* (the standard handles are the process's: forgotten, not closed) */
	if (handle <= 2)
		return 0;
	return CloseHandle(file) ? 0 : set_win32_error();
}

intptr_t _get_osfhandle(int handle)
{
	struct descriptor *entry = take(handle);
	HANDLE file;

	if (!entry)
		return (intptr_t)-1;
	file = entry->handle;
	give_back(entry);
	return (intptr_t)file;
}

/* fdopen's (port/win9x/pdclib/functions/stdio/fdopen.c): the stream takes
the descriptor's handle over, and the descriptor is free again */
void *w9x_take_descriptor_handle(int handle)
{
	struct descriptor *entry = take(handle);
	HANDLE file;

	if (!entry)
		return NULL;
	file = entry->handle;
	lock_table();
	entry->open = 0;
	entry->handle = INVALID_HANDLE_VALUE;
	unlock_table();
	give_back(entry);
	return file;
}

int _set_fmode(int mode)
{
	return mode == _O_BINARY || mode == _O_TEXT ? 0 : EINVAL;
}

int _get_fmode(int *mode)
{
	if (!mode)
		return EINVAL;
	*mode = _O_BINARY;
	return 0;
}

int _setmode(int handle, int mode)
{
	(void)mode;
	if (handle < 0 || handle >= MAXIMUM_DESCRIPTORS)
		return set_error(EBADF);
	return _O_BINARY;
}

/* ---------- reading and writing */

int _read(int handle, void *buffer, unsigned int count)
{
	struct descriptor *entry = take(handle);
	DWORD read = 0;

	if (!entry)
		return -1;
	if (!ReadFile(entry->handle, buffer, count, &read, NULL))
	{
		DWORD error = GetLastError();

		give_back(entry);
		/* a pipe whose writer has gone: the end of the input */
		if (error == ERROR_BROKEN_PIPE || error == ERROR_HANDLE_EOF)
			return 0;
		errno = _PDCLIB_w32errno(error);
		return -1;
	}
	give_back(entry);
	return (int)read;
}

int _write(int handle, const void *buffer, unsigned int count)
{
	struct descriptor *entry = take(handle);
	DWORD written = 0;

	if (!entry)
		return -1;
	if (entry->flags & _O_APPEND)
	{
		LONG high = 0;

		SetFilePointer(entry->handle, 0, &high, FILE_END);
	}
	if (!WriteFile(entry->handle, buffer, count, &written, NULL))
	{
		give_back(entry);
		return set_win32_error();
	}
	give_back(entry);
	if (written == 0 && count != 0)
		return set_error(ENOSPC);
	return (int)written;
}

/* the position after the seek, or -1; the entry is locked */
static long long seek_locked(struct descriptor *entry, long long offset, int origin)
{
	LONG high = (LONG)(offset >> 32);
	DWORD low;
	long long position;

	if (origin != SEEK_SET && origin != SEEK_CUR && origin != SEEK_END)
		return set_error(EINVAL);
	SetLastError(NO_ERROR);
	low = SetFilePointer(entry->handle, (LONG)(offset & 0xFFFFFFFF), &high, (DWORD)origin);
	if (low == INVALID_SET_FILE_POINTER && GetLastError() != NO_ERROR)
		return set_win32_error();
	position = (long long)(((unsigned long long)(DWORD)high << 32) | low);
	return position;
}

long long _lseeki64(int handle, long long offset, int origin)
{
	struct descriptor *entry = take(handle);
	long long position;

	if (!entry)
		return -1;
	position = seek_locked(entry, offset, origin);
	give_back(entry);
	return position;
}

long _lseek(int handle, long offset, int origin)
{
	long long position = _lseeki64(handle, offset, origin);

	if (position > 0x7FFFFFFFLL)
		return set_error(EINVAL);
	return (long)position;
}

int w9x_pread(int descriptor, void *buffer, unsigned long size, long long offset)
{
	struct descriptor *entry = take(descriptor);
	DWORD read = 0;

	if (!entry)
		return -1;
	/* (the file's position moves, as with the Windows build's OVERLAPPED
	read: no caller relies on it staying) */
	if (seek_locked(entry, offset, SEEK_SET) < 0)
	{
		give_back(entry);
		return -1;
	}
	if (!ReadFile(entry->handle, buffer, size, &read, NULL))
	{
		DWORD error = GetLastError();

		give_back(entry);
		if (error == ERROR_HANDLE_EOF || error == ERROR_BROKEN_PIPE)
			return 0;
		errno = _PDCLIB_w32errno(error);
		return -1;
	}
	give_back(entry);
	return (int)read;
}

int w9x_pwrite(int descriptor, const void *buffer, unsigned long size, long long offset)
{
	struct descriptor *entry = take(descriptor);
	DWORD written = 0;

	if (!entry)
		return -1;
	if (seek_locked(entry, offset, SEEK_SET) < 0)
	{
		give_back(entry);
		return -1;
	}
	if (!WriteFile(entry->handle, buffer, size, &written, NULL))
	{
		give_back(entry);
		return set_win32_error();
	}
	give_back(entry);
	return (int)written;
}

long long _filelengthi64(int handle)
{
	struct descriptor *entry = take(handle);
	DWORD low, high = 0;

	if (!entry)
		return -1;
	SetLastError(NO_ERROR);
	low = GetFileSize(entry->handle, &high);
	if (low == INVALID_FILE_SIZE && GetLastError() != NO_ERROR)
	{
		give_back(entry);
		return set_win32_error();
	}
	give_back(entry);
	return (long long)(((unsigned long long)high << 32) | low);
}

long _filelength(int handle)
{
	long long length = _filelengthi64(handle);

	if (length > 0x7FFFFFFFLL)
		return set_error(EINVAL);
	return (long)length;
}

int _chsize_s(int handle, long long size)
{
	struct descriptor *entry;
	long long position;
	int result = 0;

	if (size < 0)
		return EINVAL;
	entry = take(handle);
	if (!entry)
		return EBADF;
	position = seek_locked(entry, 0, SEEK_CUR);
	if (position < 0 || seek_locked(entry, size, SEEK_SET) < 0)
		result = errno;
	else if (!SetEndOfFile(entry->handle))
		result = _PDCLIB_w32errno(GetLastError());
	if (position >= 0)
		seek_locked(entry, position, SEEK_SET);
	give_back(entry);
	if (result)
		errno = result;
	return result;
}

int _chsize(int handle, long size)
{
	return _chsize_s(handle, size) ? -1 : 0;
}

int _commit(int handle)
{
	struct descriptor *entry = take(handle);
	int result = 0;

	if (!entry)
		return -1;
	if (!FlushFileBuffers(entry->handle))
		result = set_win32_error();
	give_back(entry);
	return result;
}

/* ---------- file information */

static unsigned short mode_of(DWORD attributes)
{
	unsigned short mode = (attributes & FILE_ATTRIBUTE_DIRECTORY) ? (_S_IFDIR | _S_IEXEC) : _S_IFREG;

	mode |= _S_IREAD;
	if (!(attributes & FILE_ATTRIBUTE_READONLY))
		mode |= _S_IWRITE;
	/* (the owner's bits copied to the group's and the others', as MSVC's) */
	mode |= (mode & 0700) >> 3;
	mode |= (mode & 0700) >> 6;
	return mode;
}

int _fstat(int handle, struct _stat *buffer)
{
	struct descriptor *entry = take(handle);
	BY_HANDLE_FILE_INFORMATION information;
	DWORD type;

	if (!entry)
		return -1;
	memset(buffer, 0, sizeof(*buffer));
	type = GetFileType(entry->handle);
	if (type != FILE_TYPE_DISK)
	{
		give_back(entry);
		buffer->st_mode = (unsigned short)(type == FILE_TYPE_CHAR ? _S_IFCHR : _S_IFIFO);
		buffer->st_mode |= _S_IREAD | _S_IWRITE;
		buffer->st_nlink = 1;
		buffer->st_rdev = buffer->st_dev = (unsigned int)handle;
		return 0;
	}
	if (!GetFileInformationByHandle(entry->handle, &information))
	{
		give_back(entry);
		return set_win32_error();
	}
	give_back(entry);
	buffer->st_mode = mode_of(information.dwFileAttributes);
	buffer->st_nlink = 1;
	buffer->st_size = (long)information.nFileSizeLow;
	buffer->st_atime = time_of(&information.ftLastAccessTime);
	buffer->st_mtime = time_of(&information.ftLastWriteTime);
	buffer->st_ctime = time_of(&information.ftCreationTime);
	return 0;
}

/* the name without separators at its end (but a root keeps its own) */
static const char *trimmed_path(const char *path, char *buffer)
{
	size_t length;

	if (!host_path(path, buffer))
		return NULL;
	length = strlen(buffer);
	while (length > 1 && buffer[length - 1] == '\\' && !(length == 3 && buffer[1] == ':'))
		buffer[--length] = '\0';
	return buffer;
}

/* A file's attributes, size and times: GetFileAttributesExA where KERNEL32
has it (Windows 98 on: looked up, not imported, so that the program still
starts on Windows 95), else its directory entry (FindFirstFileA) */
static BOOL file_attributes(const char *name, WIN32_FILE_ATTRIBUTE_DATA *data)
{
#ifdef _KERNEL32_
	/* (DOS's, dos_kernel32.c) */
	return GetFileAttributesExA(name, GetFileExInfoStandard, data);
#else
	typedef BOOL (WINAPI *get_attributes)(LPCSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
	static get_attributes get;
	static int looked;
	WIN32_FIND_DATAA found;
	HANDLE find;

	if (!looked)
	{
		HMODULE kernel = GetModuleHandleA("KERNEL32.DLL");

		get = kernel ? (get_attributes)(void *)GetProcAddress(kernel, "GetFileAttributesExA") : NULL;
		looked = 1;
	}
	if (get)
		return get(name, GetFileExInfoStandard, data);
	find = FindFirstFileA(name, &found);
	if (find == INVALID_HANDLE_VALUE)
		return FALSE;
	FindClose(find);
	data->dwFileAttributes = found.dwFileAttributes;
	data->ftCreationTime = found.ftCreationTime;
	data->ftLastAccessTime = found.ftLastAccessTime;
	data->ftLastWriteTime = found.ftLastWriteTime;
	data->nFileSizeHigh = found.nFileSizeHigh;
	data->nFileSizeLow = found.nFileSizeLow;
	return TRUE;
#endif
}

int _stat(const char *path, struct _stat *buffer)
{
	char name[PATH_SIZE];
	WIN32_FILE_ATTRIBUTE_DATA data;

	if (!trimmed_path(path, name))
		return -1;
	if (strpbrk(name, "?*"))
		return set_error(ENOENT);
	memset(buffer, 0, sizeof(*buffer));
	if (!file_attributes(name, &data))
	{
		/* (9x answers a drive's root only to GetFileAttributesA) */
		DWORD attributes = GetFileAttributesA(name);

		if (attributes == INVALID_FILE_ATTRIBUTES)
			return set_win32_error();
		buffer->st_mode = mode_of(attributes);
		buffer->st_nlink = 1;
		return 0;
	}
	buffer->st_mode = mode_of(data.dwFileAttributes);
	buffer->st_nlink = 1;
	buffer->st_size = (long)data.nFileSizeLow;
	buffer->st_atime = time_of(&data.ftLastAccessTime);
	buffer->st_mtime = time_of(&data.ftLastWriteTime);
	buffer->st_ctime = time_of(&data.ftCreationTime);
	if (name[0] && name[1] == ':')
		buffer->st_rdev = buffer->st_dev = (unsigned int)((name[0] | 0x20) - 'a');
	return 0;
}

int _access(const char *path, int mode)
{
	char name[PATH_SIZE];
	DWORD attributes;

	if (!trimmed_path(path, name))
		return -1;
	attributes = GetFileAttributesA(name);
	if (attributes == INVALID_FILE_ATTRIBUTES)
		return set_win32_error();
	if ((mode & 2) && !(attributes & FILE_ATTRIBUTE_DIRECTORY) && (attributes & FILE_ATTRIBUTE_READONLY))
		return set_error(EACCES);
	return 0;
}

/* ---------- names and directories */

int _unlink(const char *path)
{
	char name[PATH_SIZE];

	if (!host_path(path, name))
		return -1;
	return DeleteFileA(name) ? 0 : set_win32_error();
}

int _mkdir(const char *path)
{
	char name[PATH_SIZE];

	if (!trimmed_path(path, name))
		return -1;
	return CreateDirectoryA(name, NULL) ? 0 : set_win32_error();
}

int _rmdir(const char *path)
{
	char name[PATH_SIZE];

	if (!trimmed_path(path, name))
		return -1;
	return RemoveDirectoryA(name) ? 0 : set_win32_error();
}

int _chdir(const char *path)
{
	char name[PATH_SIZE];

	if (!host_path(path, name))
		return -1;
	return SetCurrentDirectoryA(name) ? 0 : set_win32_error();
}

char *_getcwd(char *buffer, int size)
{
	char directory[PATH_SIZE];
	DWORD length = GetCurrentDirectoryA(sizeof(directory), directory);

	if (length == 0 || length >= sizeof(directory))
	{
		set_win32_error();
		return NULL;
	}
	if (!buffer)
	{
		int wanted = (int)length + 1 > size ? (int)length + 1 : size;

		buffer = malloc((size_t)wanted);
		if (!buffer)
		{
			errno = ENOMEM;
			return NULL;
		}
		size = wanted;
	}
	if ((int)length + 1 > size)
	{
		errno = ERANGE;
		return NULL;
	}
	memcpy(buffer, directory, length + 1);
	return buffer;
}
