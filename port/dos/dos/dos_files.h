/*
DOS_FILES.H

DOS's file services (int 21h), called in real mode through DPMI with the
names passed in the transfer buffer and the data copied through it, so they
work under any DPMI host. Long file names (the 71xxh functions) are used
when the system has them (Windows 9x's DOS box, DOSLFN, DOSBox); without
them DOS shortens each name to 8.3 itself, which every map's name survives
("bloodgulch.map" is "bloodgul.map").

Each returns a DOS error code (0 for success), which is the same number as
Windows' for every error both have.
*/

#ifndef __DOS_FILES_H
#define __DOS_FILES_H

#define DOS_ERROR_INVALID_FUNCTION 1
#define DOS_ERROR_FILE_NOT_FOUND 2
#define DOS_ERROR_PATH_NOT_FOUND 3
#define DOS_ERROR_TOO_MANY_OPEN_FILES 4
#define DOS_ERROR_ACCESS_DENIED 5
#define DOS_ERROR_INVALID_HANDLE 6
#define DOS_ERROR_NOT_ENOUGH_MEMORY 8
#define DOS_ERROR_NO_MORE_FILES 18
#define DOS_ERROR_FILE_EXISTS 80

/* how dos_open opens: one of the access modes, and what to do when the file
exists or not */
#define DOS_OPEN_READ 0x0000
#define DOS_OPEN_WRITE 0x0001
#define DOS_OPEN_READ_WRITE 0x0002
#define DOS_IF_EXISTS_FAIL 0x0000
#define DOS_IF_EXISTS_OPEN 0x0001
#define DOS_IF_EXISTS_TRUNCATE 0x0002
#define DOS_IF_MISSING_FAIL 0x0000
#define DOS_IF_MISSING_CREATE 0x0010

/* file attributes (Windows' FILE_ATTRIBUTE_* have the same bits) */
#define DOS_ATTRIBUTE_READ_ONLY 0x01
#define DOS_ATTRIBUTE_HIDDEN 0x02
#define DOS_ATTRIBUTE_SYSTEM 0x04
#define DOS_ATTRIBUTE_DIRECTORY 0x10
#define DOS_ATTRIBUTE_ARCHIVE 0x20

/* whether the long file name functions answer */
int dos_long_file_names(void);

int dos_open(const char *path, unsigned short mode, unsigned short action, unsigned short *handle);
int dos_close(unsigned short handle);
/* reads or writes up to size bytes; *done is what was moved. A write of 0
bytes ends the file where its position is */
int dos_read(unsigned short handle, void *buffer, unsigned long size, unsigned long *done);
int dos_write(unsigned short handle, const void *buffer, unsigned long size, unsigned long *done);
/* origin 0 the start, 1 the position, 2 the end */
int dos_seek(unsigned short handle, long offset, int origin, unsigned long *position);
int dos_commit(unsigned short handle);
/* the handle is the console (or another character device) */
int dos_is_device(unsigned short handle);
/* the last write's date and time, DOS's packing (date << 16 | time) */
int dos_file_time(unsigned short handle, unsigned long *date_time);

int dos_delete(const char *path);
int dos_rename(const char *from, const char *to);
int dos_get_attributes(const char *path, unsigned short *attributes);
int dos_set_attributes(const char *path, unsigned short attributes);
int dos_make_directory(const char *path);
int dos_remove_directory(const char *path);
int dos_change_directory(const char *path);
/* the current drive and directory ("C:\GAMES\HALO") */
int dos_current_directory(char *path, unsigned long size);
/* the free and total bytes of a drive (0 the current one, 1 A:) */
int dos_disk_space(int drive, unsigned long long *free_bytes, unsigned long long *total_bytes);

/* a directory search (wildcards in the last part of the pattern) */
struct dos_find
{
	int long_names;
	unsigned short handle;
	/* the DTA of a search without long names */
	unsigned char dta[43];
	/* the entry found */
	char name[260];
	unsigned short attributes;
	unsigned long size;
	unsigned long date_time;
};
int dos_find_first(const char *pattern, struct dos_find *find);
int dos_find_next(struct dos_find *find);
void dos_find_close(struct dos_find *find);

/* the date and time now: year 1980-2099, month 1-12, ... */
void dos_date_time(int *year, int *month, int *day, int *day_of_week, int *hour, int *minute, int *second,
	int *hundredths);

#endif /* __DOS_FILES_H */
