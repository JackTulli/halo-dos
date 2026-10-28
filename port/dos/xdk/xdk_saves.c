/*
XDK_SAVES.C

The Xbox's save game and content signature services for the DOS and Windows
95 builds (the campaign's checkpoints and the player profiles), as the PC
port has them (port/linux/src/xbox_xapi.c, whose layout this keeps): each
save is a folder <root>\UDATA\<id>\ (u:\ is SAVE\ beside the program,
platform_translate_path), its display name in SaveMeta.xbx; signatures are
SHA-1 digests keyed with a fixed title key, which is what the game needs (a
stable, content-dependent signature it can check later). Also the copying of
a file and the free space of a drive.

The handles are the game's to pass back only to these functions: a save
game search's, a signature's.
*/

#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SAVE_DATA_DIRECTORY "UDATA"
#define SAVE_META_FILE "SaveMeta.xbx"

#ifndef ERROR_FILE_EXISTS
#define ERROR_FILE_EXISTS 80
#endif

/* (xdk_kernel.c's: the game's file searches) */
BOOL WINAPI FindClose(HANDLE find);

static unsigned long wide_length(LPCWSTR text)
{
	unsigned long length = 0;

	while (text[length])
		length++;
	return length;
}

/* ---------- save games */

static unsigned long long save_name_hash(LPCWSTR name)
{
	/* FNV-1a over the UTF-16 code units */
	unsigned long long hash = 1469598103934665603ULL;

	for (; *name; name++)
	{
		hash ^= (unsigned long long)*name;
		hash *= 1099511628211ULL;
	}
	return hash;
}

static const char *separator_after(const char *root)
{
	unsigned long length = (unsigned long)strlen(root);

	return length && (root[length - 1] == '\\' || root[length - 1] == '/') ? "" : "\\";
}

static void save_directory_for(LPCSTR root, LPCWSTR name, char *directory, unsigned long size)
{
	snprintf(directory, size, "%s%s" SAVE_DATA_DIRECTORY "\\%012llX\\", root, separator_after(root),
		save_name_hash(name) & 0xffffffffffffULL);
}

static BOOL save_read_name(const char *directory, WCHAR *name, unsigned long name_count)
{
	char meta_path[MAX_PATH + 32];
	HANDLE file;
	DWORD read = 0;
	BOOL success;

	snprintf(meta_path, sizeof(meta_path), "%s" SAVE_META_FILE, directory);
	file = CreateFileA(meta_path, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return FALSE;
	memset(name, 0, name_count * sizeof(WCHAR));
	success = ReadFile(file, name, (name_count - 1) * sizeof(WCHAR), &read, NULL);
	CloseHandle(file);
	return success;
}

DWORD WINAPI XCreateSaveGame(LPCSTR root_path_name, LPCWSTR save_game_name, DWORD creation_disposition,
	DWORD create_flags, LPSTR path_buffer, UINT buffer_size)
{
	char directory[MAX_PATH];
	char data_directory[MAX_PATH];
	char meta_path[MAX_PATH + 32];
	HANDLE file;
	DWORD written;

	(void)create_flags;
	save_directory_for(root_path_name, save_game_name, directory, sizeof(directory));
	if (GetFileAttributesA(directory) != (DWORD)-1)
	{
		if (creation_disposition == CREATE_NEW)
			return ERROR_ALREADY_EXISTS;
	}
	else
	{
		if (creation_disposition == OPEN_EXISTING)
			return ERROR_PATH_NOT_FOUND;
		snprintf(data_directory, sizeof(data_directory), "%s%s" SAVE_DATA_DIRECTORY, root_path_name,
			separator_after(root_path_name));
		CreateDirectoryA(root_path_name, NULL);
		CreateDirectoryA(data_directory, NULL);
		if (!CreateDirectoryA(directory, NULL))
			return GetLastError();
		snprintf(meta_path, sizeof(meta_path), "%s" SAVE_META_FILE, directory);
		file = CreateFileA(meta_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
		if (file == INVALID_HANDLE_VALUE)
			return GetLastError();
		WriteFile(file, save_game_name, wide_length(save_game_name) * sizeof(WCHAR), &written, NULL);
		CloseHandle(file);
	}
	if (path_buffer && buffer_size)
	{
		if (strlen(directory) + 1 > buffer_size)
			return ERROR_INSUFFICIENT_BUFFER;
		strcpy(path_buffer, directory);
	}
	return ERROR_SUCCESS;
}

static void delete_tree(const char *xbox_directory)
{
	char pattern[MAX_PATH + 4];
	WIN32_FIND_DATAA data;
	HANDLE find;

	snprintf(pattern, sizeof(pattern), "%s*", xbox_directory);
	find = FindFirstFileA(pattern, &data);
	if (find != INVALID_HANDLE_VALUE)
	{
		do
		{
			char child[MAX_PATH * 2];

			if (!strcmp(data.cFileName, ".") || !strcmp(data.cFileName, ".."))
				continue;
			snprintf(child, sizeof(child), "%s%s", xbox_directory, data.cFileName);
			if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			{
				strcat(child, "\\");
				delete_tree(child);
			}
			else
			{
				DeleteFileA(child);
			}
		} while (FindNextFileA(find, &data));
		FindClose(find);
	}
	RemoveDirectoryA(xbox_directory);
}

DWORD WINAPI XDeleteSaveGame(LPCSTR root_path_name, LPCWSTR save_game_name)
{
	char directory[MAX_PATH];

	save_directory_for(root_path_name, save_game_name, directory, sizeof(directory));
	if (GetFileAttributesA(directory) == (DWORD)-1)
		return ERROR_PATH_NOT_FOUND;
	delete_tree(directory);
	return ERROR_SUCCESS;
}

/* a save game search (its handle) */
struct save_game_find
{
	HANDLE directory_find;
	char data_directory[MAX_PATH];
};

static BOOL save_game_find_fill(struct save_game_find *find, WIN32_FIND_DATAA *entry, PXGAME_FIND_DATA data)
{
	do
	{
		if (!(entry->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || entry->cFileName[0] == '.')
			continue;
		memset(data, 0, sizeof(*data));
		data->wfd = *entry;
		snprintf(data->szSaveGameDirectory, sizeof(data->szSaveGameDirectory), "%s%s\\", find->data_directory,
			entry->cFileName);
		if (save_read_name(data->szSaveGameDirectory, data->szSaveGameName, MAX_GAMENAME))
			return TRUE;
	} while (FindNextFileA(find->directory_find, entry));
	SetLastError(ERROR_NO_MORE_FILES);
	return FALSE;
}

HANDLE WINAPI XFindFirstSaveGame(LPCSTR root_path_name, PXGAME_FIND_DATA find_game_data)
{
	struct save_game_find *find = calloc(1, sizeof(*find));
	char pattern[MAX_PATH + 4];
	WIN32_FIND_DATAA entry;

	if (!find)
	{
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return INVALID_HANDLE_VALUE;
	}
	snprintf(find->data_directory, sizeof(find->data_directory), "%s%s" SAVE_DATA_DIRECTORY "\\", root_path_name,
		separator_after(root_path_name));
	snprintf(pattern, sizeof(pattern), "%s*", find->data_directory);
	find->directory_find = FindFirstFileA(pattern, &entry);
	if (find->directory_find == INVALID_HANDLE_VALUE || !save_game_find_fill(find, &entry, find_game_data))
	{
		if (find->directory_find != INVALID_HANDLE_VALUE)
			FindClose(find->directory_find);
		free(find);
		SetLastError(ERROR_NO_MORE_FILES);
		return INVALID_HANDLE_VALUE;
	}
	return (HANDLE)find;
}

BOOL WINAPI XFindNextSaveGame(HANDLE find_game, PXGAME_FIND_DATA find_game_data)
{
	struct save_game_find *find = (struct save_game_find *)find_game;
	WIN32_FIND_DATAA entry;

	if (!find || find_game == INVALID_HANDLE_VALUE || !FindNextFileA(find->directory_find, &entry))
		return FALSE;
	return save_game_find_fill(find, &entry, find_game_data);
}

BOOL WINAPI XFindClose(HANDLE find_game)
{
	struct save_game_find *find = (struct save_game_find *)find_game;

	if (!find || find_game == INVALID_HANDLE_VALUE)
		return FALSE;
	FindClose(find->directory_find);
	free(find);
	return TRUE;
}

/* ---------- content signatures (SHA-1, keyed with a fixed title key) */

struct sha1_context
{
	unsigned long state[5];
	unsigned long long length;
	unsigned char buffer[64];
	unsigned long buffered;
};

#define SHA1_ROTATE(value, bits) (((value) << (bits)) | ((value) >> (32 - (bits))))

static void sha1_block(struct sha1_context *context, const unsigned char *block)
{
	unsigned long words[80];
	unsigned long a, b, c, d, e;
	int index;

	for (index = 0; index < 16; index++)
	{
		words[index] = ((unsigned long)block[index * 4] << 24) | ((unsigned long)block[index * 4 + 1] << 16) |
			((unsigned long)block[index * 4 + 2] << 8) | block[index * 4 + 3];
	}
	for (; index < 80; index++)
		words[index] = SHA1_ROTATE(words[index - 3] ^ words[index - 8] ^ words[index - 14] ^ words[index - 16], 1);
	a = context->state[0];
	b = context->state[1];
	c = context->state[2];
	d = context->state[3];
	e = context->state[4];
	for (index = 0; index < 80; index++)
	{
		unsigned long f, k, temporary;

		if (index < 20) { f = (b & c) | (~b & d); k = 0x5a827999; }
		else if (index < 40) { f = b ^ c ^ d; k = 0x6ed9eba1; }
		else if (index < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
		else { f = b ^ c ^ d; k = 0xca62c1d6; }
		temporary = SHA1_ROTATE(a, 5) + f + e + k + words[index];
		e = d;
		d = c;
		c = SHA1_ROTATE(b, 30);
		b = a;
		a = temporary;
	}
	context->state[0] += a;
	context->state[1] += b;
	context->state[2] += c;
	context->state[3] += d;
	context->state[4] += e;
}

static void sha1_initialize(struct sha1_context *context)
{
	memset(context, 0, sizeof(*context));
	context->state[0] = 0x67452301;
	context->state[1] = 0xefcdab89;
	context->state[2] = 0x98badcfe;
	context->state[3] = 0x10325476;
	context->state[4] = 0xc3d2e1f0;
}

static void sha1_update(struct sha1_context *context, const unsigned char *data, unsigned long size)
{
	context->length += size;
	while (size)
	{
		unsigned long count = 64 - context->buffered;

		if (count > size)
			count = size;
		memcpy(context->buffer + context->buffered, data, count);
		context->buffered += count;
		data += count;
		size -= count;
		if (context->buffered == 64)
		{
			sha1_block(context, context->buffer);
			context->buffered = 0;
		}
	}
}

static void sha1_finish(struct sha1_context *context, unsigned char digest[20])
{
	unsigned long long bits = context->length * 8;
	unsigned char padding = 0x80;
	unsigned char length_bytes[8];
	int index;

	sha1_update(context, &padding, 1);
	padding = 0;
	while (context->buffered != 56)
		sha1_update(context, &padding, 1);
	for (index = 0; index < 8; index++)
		length_bytes[index] = (unsigned char)(bits >> (56 - index * 8));
	sha1_update(context, length_bytes, 8);
	for (index = 0; index < 20; index++)
		digest[index] = (unsigned char)(context->state[index / 4] >> (24 - (index % 4) * 8));
}

static const unsigned char signature_title_key[] = "halo-dos content signature";

HANDLE WINAPI XCalculateSignatureBegin(DWORD flags)
{
	struct sha1_context *context = malloc(sizeof(*context));

	(void)flags;
	if (!context)
	{
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return INVALID_HANDLE_VALUE;
	}
	sha1_initialize(context);
	sha1_update(context, signature_title_key, sizeof(signature_title_key));
	return (HANDLE)context;
}

DWORD WINAPI XCalculateSignatureUpdate(HANDLE calculation, const BYTE *data, ULONG size)
{
	if (!calculation || calculation == INVALID_HANDLE_VALUE)
		return ERROR_INVALID_HANDLE;
	sha1_update((struct sha1_context *)calculation, data, size);
	return ERROR_SUCCESS;
}

DWORD WINAPI XCalculateSignatureEnd(HANDLE calculation, PXCALCSIG_SIGNATURE signature)
{
	if (!calculation || calculation == INVALID_HANDLE_VALUE)
		return ERROR_INVALID_HANDLE;
	if (signature)
		sha1_finish((struct sha1_context *)calculation, signature->Signature);
	free(calculation);
	return ERROR_SUCCESS;
}

/* ---------- files */

BOOL WINAPI CopyFileA(LPCSTR from, LPCSTR to, BOOL fail_if_exists)
{
	char host_from[300], host_to[300], buffer[4096];
	FILE *in, *out;
	size_t count;
	BOOL success = TRUE;

	platform_translate_path(from, host_from, sizeof(host_from));
	platform_translate_path(to, host_to, sizeof(host_to));
	if (fail_if_exists && (out = fopen(host_to, "rb")) != NULL)
	{
		fclose(out);
		SetLastError(ERROR_FILE_EXISTS);
		return FALSE;
	}
	in = fopen(host_from, "rb");
	if (!in)
	{
		SetLastError(ERROR_FILE_NOT_FOUND);
		return FALSE;
	}
	out = fopen(host_to, "wb");
	if (!out)
	{
		fclose(in);
		SetLastError(ERROR_ACCESS_DENIED);
		return FALSE;
	}
	while ((count = fread(buffer, 1, sizeof(buffer), in)) > 0)
	{
		if (fwrite(buffer, 1, count, out) != count)
		{
			success = FALSE;
			SetLastError(ERROR_DISK_FULL);
			break;
		}
	}
	fclose(in);
	fclose(out);
	return success;
}

/* (the game asks before it saves: a drive with room, 1 GB) */
BOOL WINAPI GetDiskFreeSpaceExA(LPCSTR directory, PULARGE_INTEGER available, PULARGE_INTEGER total,
	PULARGE_INTEGER free_bytes)
{
	(void)directory;
	if (available)
		available->QuadPart = 1ULL << 30;
	if (total)
		total->QuadPart = 2ULL << 30;
	if (free_bytes)
		free_bytes->QuadPart = 1ULL << 30;
	return TRUE;
}

/* ---------- nicknames (the names last used: none kept) */

BOOL WINAPI XSetNicknameW(LPCWSTR nickname, BOOL preserve_case)
{
	(void)nickname;
	(void)preserve_case;
	return TRUE;
}

HANDLE WINAPI XFindFirstNicknameW(BOOL this_title_only, LPWSTR nickname, UINT size)
{
	(void)this_title_only;
	if (size)
		nickname[0] = 0;
	SetLastError(ERROR_NO_MORE_FILES);
	return INVALID_HANDLE_VALUE;
}
