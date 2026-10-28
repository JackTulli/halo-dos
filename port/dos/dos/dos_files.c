/*
DOS_FILES.C

DOS's file services through the transfer buffer (dos_files.h).

The transfer buffer's first 4 KB carry names and the structures DOS fills
in; the rest carries file data, a chunk at a time.
*/

#include "dos_dpmi.h"
#include "dos_files.h"

#include <stdio.h>
#include <string.h>

#define NAME_A 0x0000
#define NAME_B 0x0200
#define FIND_DATA 0x0400
#define DTA 0x0600
#define DIRECTORY 0x0800
#define FREE_SPACE 0x0A00
/* (the long names' detection, which runs in the middle of the first call
that asks for it, after that call has put its name at NAME_A) */
#define DETECTION_NAME 0x0C00
#define DETECTION_RESULT 0x0C10
#define DATA 0x1000
#define DATA_SIZE (DOS_TRANSFER_SIZE - DATA)

/* -1 not known yet, then 0 or 1 */
static int long_names = -1;

/* runs int 21h; the DOS error (AX when the carry is set) or 0 */
static int dos_call(struct dos_registers *r)
{
	if (!dos_real_interrupt(0x21, r))
		return DOS_ERROR_INVALID_FUNCTION;
	return (r->flags & DOS_CARRY) ? (int)(r->eax & 0xFFFF) : 0;
}

static void clear(struct dos_registers *r)
{
	memset(r, 0, sizeof(*r));
}

/* a name into the transfer buffer at offset; FALSE if too long */
static int put_name(const char *path, unsigned short offset)
{
	size_t length = strlen(path);

	if (length >= 260)
		return 0;
	memcpy(dos_transfer + offset, path, length + 1);
	/* DOS wants backslashes */
	for (char *c = (char *)dos_transfer + offset; *c; c++)
	{
		if (*c == '/')
			*c = '\\';
	}
	return 1;
}

/* an LFN call that DOS without long names does not know comes back with
AX = 7100h and the carry set */
static int unsupported(int error)
{
	return error == 0x7100 || error == DOS_ERROR_INVALID_FUNCTION;
}

int dos_long_file_names(void)
{
	if (long_names < 0)
	{
		struct dos_registers r;

		/* get volume information of C:\ (any drive answers when the
		functions are there) */
		put_name("C:\\", DETECTION_NAME);
		clear(&r);
		r.eax = 0x71A0;
		r.ds = dos_transfer_segment;
		r.edx = DETECTION_NAME;
		r.es = dos_transfer_segment;
		r.edi = DETECTION_RESULT;
		r.ecx = 32;
		r.flags = DOS_CARRY;
		long_names = dos_call(&r) == 0 && (r.eax & 0xFFFF) != 0x7100;
	}
	return long_names;
}

static int open_one(const char *path, unsigned short mode, unsigned short action, unsigned short *handle);

/* Without long file names a file's name is 8.3: one copied there from a
long name is either cut to 8.3 (by DOS, by the player) or the alias Windows
gives it (the first six characters, ~ and a number). A file a long name does
not find is looked for under those, the name cut to 8.3 first. */
static int short_alternative(const char *path, int which, char *out, size_t size)
{
	const char *name = strrchr(path, '\\'), *slash = strrchr(path, '/');
	const char *dot;
	size_t base_length, extension_length, prefix;
	char base[16], extension[8];

	if (slash > name)
		name = slash;
	name = name ? name + 1 : path;
	prefix = (size_t)(name - path);
	dot = strrchr(name, '.');
	base_length = dot ? (size_t)(dot - name) : strlen(name);
	extension_length = dot ? strlen(dot + 1) : 0;
	if (base_length <= 8 && extension_length <= 3)
		return 0;
	if (which == 0)
		snprintf(base, sizeof(base), "%.*s", (int)(base_length < 8 ? base_length : 8), name);
	else
		snprintf(base, sizeof(base), "%.*s~%d", (int)(base_length < 6 ? base_length : 6), name, which);
	snprintf(extension, sizeof(extension), "%.*s", (int)(extension_length < 3 ? extension_length : 3),
		dot ? dot + 1 : "");
	if (prefix + strlen(base) + 1 + strlen(extension) + 1 > size)
		return 0;
	snprintf(out, size, "%.*s%s%s%s", (int)prefix, path, base, extension[0] ? "." : "", extension);
	return 1;
}

int dos_open(const char *path, unsigned short mode, unsigned short action, unsigned short *handle)
{
	int error = open_one(path, mode, action, handle);

	if (error == DOS_ERROR_FILE_NOT_FOUND && !(action & DOS_IF_MISSING_CREATE))
	{
		char alternative[300];

		for (int which = 0; which <= 4; which++)
		{
			if (!short_alternative(path, which, alternative, sizeof(alternative)))
				break;
			if (!open_one(alternative, mode, action, handle))
				return 0;
		}
	}
	return error;
}

static int open_one(const char *path, unsigned short mode, unsigned short action, unsigned short *handle)
{
	struct dos_registers r;
	int error;

	if (!put_name(path, NAME_A))
		return DOS_ERROR_PATH_NOT_FOUND;
	if (dos_long_file_names())
	{
		clear(&r);
		r.eax = 0x716C;
		r.ebx = mode | 0x0040; /* share deny none */
		r.ecx = 0;
		r.edx = action;
		r.ds = dos_transfer_segment;
		r.esi = NAME_A;
		r.edi = 0;
		error = dos_call(&r);
		if (!unsupported(error))
		{
			if (!error)
				*handle = (unsigned short)r.eax;
			return error;
		}
	}
	/* DOS 4's extended open */
	clear(&r);
	r.eax = 0x6C00;
	r.ebx = mode | 0x0040;
	r.ecx = 0;
	r.edx = action;
	r.ds = dos_transfer_segment;
	r.esi = NAME_A;
	error = dos_call(&r);
	if (!error)
	{
		*handle = (unsigned short)r.eax;
		return 0;
	}
	if (error != DOS_ERROR_INVALID_FUNCTION)
		return error;
	/* older DOS: open, then create */
	clear(&r);
	r.eax = 0x3D00 | (mode & 0x03);
	r.ds = dos_transfer_segment;
	r.edx = NAME_A;
	error = dos_call(&r);
	if (!error)
	{
		if (!(action & 0x0F))
		{
			struct dos_registers c;

			clear(&c);
			c.eax = 0x3E00;
			c.ebx = r.eax & 0xFFFF;
			dos_call(&c);
			return DOS_ERROR_FILE_EXISTS;
		}
		*handle = (unsigned short)r.eax;
		if ((action & 0x0F) == DOS_IF_EXISTS_TRUNCATE)
		{
			unsigned long done;

			return dos_write(*handle, "", 0, &done);
		}
		return 0;
	}
	if (error != DOS_ERROR_FILE_NOT_FOUND || !(action & DOS_IF_MISSING_CREATE))
		return error;
	clear(&r);
	r.eax = 0x3C00;
	r.ecx = 0;
	r.ds = dos_transfer_segment;
	r.edx = NAME_A;
	error = dos_call(&r);
	if (!error)
		*handle = (unsigned short)r.eax;
	return error;
}

int dos_close(unsigned short handle)
{
	struct dos_registers r;

	clear(&r);
	r.eax = 0x3E00;
	r.ebx = handle;
	return dos_call(&r);
}

int dos_read(unsigned short handle, void *buffer, unsigned long size, unsigned long *done)
{
	unsigned char *out = buffer;

	*done = 0;
	while (size)
	{
		struct dos_registers r;
		unsigned long chunk = size < DATA_SIZE ? size : DATA_SIZE;
		unsigned long got;
		int error;

		clear(&r);
		r.eax = 0x3F00;
		r.ebx = handle;
		r.ecx = chunk;
		r.ds = dos_transfer_segment;
		r.edx = DATA;
		error = dos_call(&r);
		if (error)
			return error;
		got = r.eax & 0xFFFF;
		memcpy(out, dos_transfer + DATA, got);
		out += got;
		*done += got;
		size -= got;
		if (got < chunk)
			break;
	}
	return 0;
}

int dos_write(unsigned short handle, const void *buffer, unsigned long size, unsigned long *done)
{
	const unsigned char *in = buffer;

	*done = 0;
	do
	{
		struct dos_registers r;
		unsigned long chunk = size < DATA_SIZE ? size : DATA_SIZE;
		unsigned long put;
		int error;

		memcpy(dos_transfer + DATA, in, chunk);
		clear(&r);
		r.eax = 0x4000;
		r.ebx = handle;
		r.ecx = chunk;
		r.ds = dos_transfer_segment;
		r.edx = DATA;
		error = dos_call(&r);
		if (error)
			return error;
		put = r.eax & 0xFFFF;
		in += put;
		*done += put;
		size -= put;
		if (put < chunk)
			break;
	} while (size);
	return 0;
}

int dos_seek(unsigned short handle, long offset, int origin, unsigned long *position)
{
	struct dos_registers r;
	int error;

	clear(&r);
	r.eax = 0x4200 | (origin & 3);
	r.ebx = handle;
	r.ecx = ((unsigned long)offset >> 16) & 0xFFFF;
	r.edx = (unsigned long)offset & 0xFFFF;
	error = dos_call(&r);
	if (!error && position)
		*position = ((r.edx & 0xFFFF) << 16) | (r.eax & 0xFFFF);
	return error;
}

int dos_commit(unsigned short handle)
{
	struct dos_registers r;

	clear(&r);
	r.eax = 0x6800;
	r.ebx = handle;
	return dos_call(&r);
}

int dos_is_device(unsigned short handle)
{
	struct dos_registers r;

	clear(&r);
	r.eax = 0x4400;
	r.ebx = handle;
	if (dos_call(&r))
		return 0;
	return (r.edx & 0x80) != 0;
}

int dos_file_time(unsigned short handle, unsigned long *date_time)
{
	struct dos_registers r;
	int error;

	clear(&r);
	r.eax = 0x5700;
	r.ebx = handle;
	error = dos_call(&r);
	if (!error)
		*date_time = ((r.edx & 0xFFFF) << 16) | (r.ecx & 0xFFFF);
	return error;
}

/* a call taking one name in DS:DX: the long-name function, else DOS's */
static int name_call(const char *path, unsigned short long_function, unsigned short function,
	struct dos_registers *r)
{
	struct dos_registers saved = *r;
	int error;

	if (!put_name(path, NAME_A))
		return DOS_ERROR_PATH_NOT_FOUND;
	if (dos_long_file_names())
	{
		r->eax = long_function;
		r->ds = dos_transfer_segment;
		r->edx = NAME_A;
		error = dos_call(r);
		if (!unsupported(error))
			return error;
		*r = saved;
	}
	r->eax = function;
	r->ds = dos_transfer_segment;
	r->edx = NAME_A;
	return dos_call(r);
}

int dos_delete(const char *path)
{
	struct dos_registers r;

	clear(&r);
	r.esi = 0; /* no wildcards */
	r.ecx = 0;
	return name_call(path, 0x7141, 0x4100, &r);
}

int dos_rename(const char *from, const char *to)
{
	struct dos_registers r;

	if (!put_name(to, NAME_B))
		return DOS_ERROR_PATH_NOT_FOUND;
	clear(&r);
	r.es = dos_transfer_segment;
	r.edi = NAME_B;
	return name_call(from, 0x7156, 0x5600, &r);
}

int dos_get_attributes(const char *path, unsigned short *attributes)
{
	struct dos_registers r;
	int error;

	clear(&r);
	r.ebx = 0;
	error = name_call(path, 0x7143, 0x4300, &r);
	if (!error)
		*attributes = (unsigned short)r.ecx;
	return error;
}

int dos_set_attributes(const char *path, unsigned short attributes)
{
	struct dos_registers r;

	clear(&r);
	r.ebx = 1;
	r.ecx = attributes;
	return name_call(path, 0x7143, 0x4301, &r);
}

int dos_make_directory(const char *path)
{
	struct dos_registers r;

	clear(&r);
	return name_call(path, 0x7139, 0x3900, &r);
}

int dos_remove_directory(const char *path)
{
	struct dos_registers r;

	clear(&r);
	return name_call(path, 0x713A, 0x3A00, &r);
}

int dos_change_directory(const char *path)
{
	struct dos_registers r;

	/* a drive letter changes the drive too */
	if (path[0] && path[1] == ':')
	{
		clear(&r);
		r.eax = 0x0E00;
		r.edx = (unsigned long)((path[0] | 0x20) - 'a');
		dos_call(&r);
		if (!path[2])
			return 0;
	}
	clear(&r);
	return name_call(path, 0x713B, 0x3B00, &r);
}

int dos_current_directory(char *path, unsigned long size)
{
	struct dos_registers r;
	int drive;
	int error = DOS_ERROR_INVALID_FUNCTION;
	size_t length;

	clear(&r);
	r.eax = 0x1900;
	dos_call(&r);
	drive = (int)(r.eax & 0xFF);
	if (dos_long_file_names())
	{
		clear(&r);
		r.eax = 0x7147;
		r.edx = 0;
		r.ds = dos_transfer_segment;
		r.esi = DIRECTORY;
		error = dos_call(&r);
	}
	if (error)
	{
		clear(&r);
		r.eax = 0x4700;
		r.edx = 0;
		r.ds = dos_transfer_segment;
		r.esi = DIRECTORY;
		error = dos_call(&r);
		if (error)
			return error;
	}
	length = strlen((char *)dos_transfer + DIRECTORY);
	if (length + 4 > size)
		return DOS_ERROR_NOT_ENOUGH_MEMORY;
	path[0] = (char)('A' + drive);
	path[1] = ':';
	path[2] = '\\';
	memcpy(path + 3, dos_transfer + DIRECTORY, length + 1);
	return 0;
}

int dos_disk_space(int drive, unsigned long long *free_bytes, unsigned long long *total_bytes)
{
	struct dos_registers r;
	int error;

	/* FAT32's extended free space first (drives over 2 GB) */
	if (drive > 0)
	{
		char root[4] = { (char)('A' + drive - 1), ':', '\\', 0 };

		put_name(root, NAME_A);
		memset(dos_transfer + FREE_SPACE, 0, 44);
		clear(&r);
		r.eax = 0x7303;
		r.ds = dos_transfer_segment;
		r.edx = NAME_A;
		r.es = dos_transfer_segment;
		r.edi = FREE_SPACE;
		r.ecx = 44;
		if (!dos_call(&r) && (r.eax & 0xFFFF) != 0x7300)
		{
			const unsigned long *info = (const unsigned long *)(dos_transfer + FREE_SPACE);
			unsigned long long cluster = (unsigned long long)info[1] * info[2];

			*free_bytes = cluster * info[3];
			*total_bytes = cluster * info[4];
			return 0;
		}
	}
	clear(&r);
	r.eax = 0x3600;
	r.edx = (unsigned long)drive;
	error = dos_call(&r);
	if (error)
		return error;
	if ((r.eax & 0xFFFF) == 0xFFFF)
		return DOS_ERROR_INVALID_FUNCTION;
	*free_bytes = (unsigned long long)(r.eax & 0xFFFF) * (r.ecx & 0xFFFF) * (r.ebx & 0xFFFF);
	*total_bytes = (unsigned long long)(r.eax & 0xFFFF) * (r.ecx & 0xFFFF) * (r.edx & 0xFFFF);
	return 0;
}

/* ---------- searches */

static void found_long(struct dos_find *find)
{
	const unsigned char *data = dos_transfer + FIND_DATA;

	find->attributes = (unsigned short)*(const unsigned long *)data;
	find->size = *(const unsigned long *)(data + 32);
	/* (date and time in DOS's format: asked for with SI = 1) */
	find->date_time = ((unsigned long)*(const unsigned short *)(data + 22) << 16) |
		*(const unsigned short *)(data + 20);
	strncpy(find->name, (const char *)data + 44, sizeof(find->name) - 1);
	find->name[sizeof(find->name) - 1] = 0;
}

static void found_short(struct dos_find *find)
{
	const unsigned char *dta = dos_transfer + DTA;

	memcpy(find->dta, dta, sizeof(find->dta));
	find->attributes = dta[21];
	find->date_time = ((unsigned long)*(const unsigned short *)(dta + 24) << 16) |
		*(const unsigned short *)(dta + 22);
	find->size = *(const unsigned long *)(dta + 26);
	strncpy(find->name, (const char *)dta + 30, 13);
	find->name[13] = 0;
}

static void set_dta(void)
{
	struct dos_registers r;

	clear(&r);
	r.eax = 0x1A00;
	r.ds = dos_transfer_segment;
	r.edx = DTA;
	dos_call(&r);
}

int dos_find_first(const char *pattern, struct dos_find *find)
{
	struct dos_registers r;
	int error;

	memset(find, 0, sizeof(*find));
	if (!put_name(pattern, NAME_A))
		return DOS_ERROR_PATH_NOT_FOUND;
	if (dos_long_file_names())
	{
		clear(&r);
		r.eax = 0x714E;
		r.ecx = DOS_ATTRIBUTE_DIRECTORY | DOS_ATTRIBUTE_READ_ONLY | DOS_ATTRIBUTE_ARCHIVE;
		r.esi = 1;
		r.ds = dos_transfer_segment;
		r.edx = NAME_A;
		r.es = dos_transfer_segment;
		r.edi = FIND_DATA;
		error = dos_call(&r);
		if (!unsupported(error))
		{
			if (error)
				return error;
			find->long_names = 1;
			find->handle = (unsigned short)r.eax;
			found_long(find);
			return 0;
		}
	}
	set_dta();
	clear(&r);
	r.eax = 0x4E00;
	r.ecx = DOS_ATTRIBUTE_DIRECTORY | DOS_ATTRIBUTE_READ_ONLY | DOS_ATTRIBUTE_ARCHIVE;
	r.ds = dos_transfer_segment;
	r.edx = NAME_A;
	error = dos_call(&r);
	if (error)
		return error;
	found_short(find);
	return 0;
}

int dos_find_next(struct dos_find *find)
{
	struct dos_registers r;
	int error;

	clear(&r);
	if (find->long_names)
	{
		r.eax = 0x714F;
		r.ebx = find->handle;
		r.esi = 1;
		r.es = dos_transfer_segment;
		r.edi = FIND_DATA;
		error = dos_call(&r);
		if (!error)
			found_long(find);
		return error;
	}
	/* (the search's state is in the DTA, which other calls may have
	overwritten) */
	memcpy(dos_transfer + DTA, find->dta, sizeof(find->dta));
	set_dta();
	clear(&r);
	r.eax = 0x4F00;
	error = dos_call(&r);
	if (!error)
		found_short(find);
	return error;
}

void dos_find_close(struct dos_find *find)
{
	if (find->long_names)
	{
		struct dos_registers r;

		clear(&r);
		r.eax = 0x71A1;
		r.ebx = find->handle;
		dos_call(&r);
		find->long_names = 0;
	}
}

void dos_date_time(int *year, int *month, int *day, int *day_of_week, int *hour, int *minute, int *second,
	int *hundredths)
{
	struct dos_registers r;

	clear(&r);
	r.eax = 0x2A00;
	dos_call(&r);
	*year = (int)(r.ecx & 0xFFFF);
	*month = (int)((r.edx >> 8) & 0xFF);
	*day = (int)(r.edx & 0xFF);
	*day_of_week = (int)(r.eax & 0xFF);
	clear(&r);
	r.eax = 0x2C00;
	dos_call(&r);
	*hour = (int)((r.ecx >> 8) & 0xFF);
	*minute = (int)(r.ecx & 0xFF);
	*second = (int)((r.edx >> 8) & 0xFF);
	*hundredths = (int)(r.edx & 0xFF);
}
