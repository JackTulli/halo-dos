/*
SYS_FILES.C

What the Xbox-facing layer asks Win32 for that the C runtime has no call
for: a directory search (FindFirstFileA), for DOS and Windows 95 alike (on
DOS, port/dos/dos/dos_kernel32.c answers it).
*/

#include <windows.h>

#include <string.h>

struct search
{
	HANDLE find;
	WIN32_FIND_DATAA data;
	int pending;
};

static void found(const WIN32_FIND_DATAA *data, char *name, unsigned long name_size, unsigned long *attributes,
	unsigned long *size)
{
	strncpy(name, data->cFileName, name_size - 1);
	name[name_size - 1] = 0;
	*attributes = data->dwFileAttributes;
	*size = data->nFileSizeLow;
}

int sys_find_first(const char *pattern, char *name, unsigned long name_size, unsigned long *attributes,
	unsigned long *size, void **search_out)
{
	struct search *search = LocalAlloc(LMEM_ZEROINIT, sizeof(*search));

	*search_out = NULL;
	if (!search)
		return FALSE;
	search->find = FindFirstFileA(pattern, &search->data);
	if (search->find == INVALID_HANDLE_VALUE)
	{
		LocalFree(search);
		return FALSE;
	}
	found(&search->data, name, name_size, attributes, size);
	*search_out = search;
	return TRUE;
}

int sys_find_next(void *search_in, char *name, unsigned long name_size, unsigned long *attributes,
	unsigned long *size)
{
	struct search *search = search_in;

	if (!search || !FindNextFileA(search->find, &search->data))
		return FALSE;
	found(&search->data, name, name_size, attributes, size);
	return TRUE;
}

void sys_find_close(void *search_in)
{
	struct search *search = search_in;

	if (search)
	{
		FindClose(search->find);
		LocalFree(search);
	}
}
