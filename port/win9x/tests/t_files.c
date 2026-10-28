/*
T_FILES.C

Files through the 9x build's C library: PDCLib's streams over the KERNEL32
glue (port/win9x/pdclib/functions/_PDCLIB: fopen's modes, fread, fwrite,
fseek, ftell, append, rename, remove, tmpfile, text and binary), and the
MSVC descriptors (port/win9x/src/w9x_crt_io.c: _open, _read, _write,
_lseek, _lseeki64 past 4 GB, _chsize_s, _fstat, _stat, _access, _unlink,
_mkdir, _rmdir, _getcwd, _get_osfhandle, fdopen, pread and pwrite), all
with '/' in the names, which reach KERNEL32 as '\'.
*/

#include <windows.h>

#include <direct.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "w9x_test.h"
#include "w9x_runtime.h"

#define FOLDER "build/win9x/tests/files"

FILE *fdopen(int handle, const char *mode);

static void test_streams(void)
{
	static char block[100000], back[100000];
	FILE *file;
	long size;
	int i, c;
	char line[64];

	for (i = 0; i < (int)sizeof(block); i++)
		block[i] = (char)(i * 13 + 5);
	file = fopen(FOLDER "/stream.bin", "wb");
	CHECK(file != NULL);
	if (!file)
		return;
	CHECK(fwrite(block, 1, sizeof(block), file) == sizeof(block));
	CHECK(ftell(file) == (long)sizeof(block));
	CHECK(fclose(file) == 0);

	file = fopen(FOLDER "/stream.bin", "rb");
	CHECK(file != NULL);
	CHECK(fread(back, 1, sizeof(back), file) == sizeof(back));
	CHECK(!memcmp(block, back, sizeof(block)));
	CHECK(fgetc(file) == EOF && feof(file));
	CHECK(fseek(file, 4097, SEEK_SET) == 0 && ftell(file) == 4097);
	CHECK(fgetc(file) == (unsigned char)block[4097]);
	CHECK(fseek(file, -10, SEEK_END) == 0 && ftell(file) == (long)sizeof(block) - 10);
	CHECK(fgetc(file) == (unsigned char)block[sizeof(block) - 10]);
	CHECK(fseek(file, -1000, SEEK_CUR) == 0);
	CHECK(ftell(file) == (long)sizeof(block) - 1009);
	c = fgetc(file);
	CHECK(ungetc(c, file) == c && fgetc(file) == c);
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	CHECK(size == (long)sizeof(block));
	fclose(file);

	/* "r+": changes in place */
	file = fopen(FOLDER "\\stream.bin", "r+b");
	CHECK(file != NULL);
	fseek(file, 50000, SEEK_SET);
	fwrite("CHANGED", 1, 7, file);
	fseek(file, 49998, SEEK_SET);
	fread(line, 1, 11, file);
	CHECK(!memcmp(line + 2, "CHANGED", 7) && line[0] == block[49998] && line[9] == block[50007]);
	fclose(file);

	/* "a": appends, whatever the position */
	file = fopen(FOLDER "/text.txt", "w");
	CHECK(file != NULL);
	fprintf(file, "first line %d\n", 1);
	fclose(file);
	file = fopen(FOLDER "/text.txt", "a");
	fprintf(file, "second line %s\n", "two");
	fseek(file, 0, SEEK_SET);
	fputs("third\n", file);
	fclose(file);
	file = fopen(FOLDER "/text.txt", "r");
	CHECK(fgets(line, sizeof(line), file) && !strcmp(line, "first line 1\n"));
	CHECK(fgets(line, sizeof(line), file) && !strcmp(line, "second line two\n"));
	CHECK(fgets(line, sizeof(line), file) && !strcmp(line, "third\n"));
	CHECK(!fgets(line, sizeof(line), file));
	fclose(file);

	/* "w+": truncates, reads back */
	file = fopen(FOLDER "/text.txt", "w+");
	CHECK(file != NULL);
	fprintf(file, "%d %s", 12345, "word");
	rewind(file);
	{
		int number = 0;
		char word[16] = "";

		CHECK(fscanf(file, "%d %15s", &number, word) == 2 && number == 12345 && !strcmp(word, "word"));
	}
	fclose(file);

	/* rename and remove (a name that exists already: refused, as MSVC's) */
	remove(FOLDER "/renamed.txt");
	CHECK(rename(FOLDER "/text.txt", FOLDER "/renamed.txt") == 0);
	CHECK(fopen(FOLDER "/text.txt", "r") == NULL && errno == ENOENT);
	CHECK(rename(FOLDER "/stream.bin", FOLDER "/renamed.txt") != 0);
	CHECK(remove(FOLDER "/renamed.txt") == 0);
	CHECK(remove(FOLDER "/renamed.txt") != 0);

	/* tmpfile: gone when closed */
	file = tmpfile();
	CHECK(file != NULL);
	if (file)
	{
		fputs("temporary", file);
		rewind(file);
		CHECK(fgets(line, sizeof(line), file) && !strcmp(line, "temporary"));
		fclose(file);
	}
	/* many streams open at once (fopen's lock each, freed by fclose) */
	{
		FILE *files[40];
		int opened = 0;

		for (i = 0; i < 40; i++)
		{
			files[i] = fopen(FOLDER "/stream.bin", "rb");
			opened += files[i] != NULL;
		}
		for (i = 0; i < 40; i++)
		{
			if (files[i])
				fclose(files[i]);
		}
		CHECK(opened == 40);
	}
}

static void test_descriptors(void)
{
	char buffer[256];
	struct _stat information;
	int handle, second;
	long long position;

	handle = _open(FOLDER "/descriptor.bin", _O_CREAT | _O_TRUNC | _O_RDWR | _O_BINARY | _O_NOINHERIT,
		_S_IREAD | _S_IWRITE);
	CHECK(handle >= 3);
	if (handle < 0)
		return;
	CHECK(_write(handle, "0123456789", 10) == 10);
	CHECK(_lseek(handle, 0, SEEK_CUR) == 10);
	CHECK(_lseek(handle, 2, SEEK_SET) == 2);
	CHECK(_read(handle, buffer, 4) == 4 && !memcmp(buffer, "2345", 4));
	/* far past the end (a seek only: nothing written there) */
	position = _lseeki64(handle, 5000000000LL, SEEK_SET);
	printf("# _lseeki64 to 5000000000: %lld\n", position);
	CHECK(position == 5000000000LL);
	CHECK(_lseeki64(handle, 0, SEEK_CUR) == 5000000000LL);
	CHECK(_lseek(handle, 0, SEEK_CUR) == -1 && errno == EINVAL);
	CHECK(_lseeki64(handle, -4999999990LL, SEEK_CUR) == 10);
	CHECK(_read(handle, buffer, 10) == 0);
	CHECK(_fstat(handle, &information) == 0 && information.st_size == 10 && (information.st_mode & _S_IFREG));
	CHECK(_filelength(handle) == 10);
	CHECK(_chsize_s(handle, 4) == 0 && _filelength(handle) == 4);
	CHECK(_lseek(handle, 0, SEEK_CUR) == 10);
	CHECK(_chsize_s(handle, 20000) == 0 && _filelength(handle) == 20000);
	CHECK(_get_osfhandle(handle) != -1 && GetFileType((HANDLE)_get_osfhandle(handle)) == FILE_TYPE_DISK);
	/* pread and pwrite (the map cache's) */
	CHECK(w9x_pwrite(handle, "PWRITE", 6, 12345) == 6);
	CHECK(w9x_pread(handle, buffer, 8, 12344) == 8 && !memcmp(buffer + 1, "PWRITE", 6));
	CHECK(w9x_pread(handle, buffer, 100, 20000) == 0);
	CHECK(_close(handle) == 0);
	CHECK(_close(handle) == -1 && errno == EBADF);
	CHECK(_read(handle, buffer, 1) == -1 && errno == EBADF);

	/* _O_EXCL, _O_APPEND */
	CHECK(_open(FOLDER "/descriptor.bin", _O_CREAT | _O_EXCL | _O_WRONLY, _S_IWRITE) == -1 && errno == EEXIST);
	handle = _open(FOLDER "/descriptor.bin", _O_WRONLY | _O_APPEND);
	CHECK(handle >= 0);
	_lseek(handle, 0, SEEK_SET);
	CHECK(_write(handle, "END", 3) == 3);
	_close(handle);
	CHECK(_stat(FOLDER "/descriptor.bin", &information) == 0 && information.st_size == 20003);
	printf("# st_mtime is %ld seconds from now\n", (long)(information.st_mtime - time(NULL)));
	CHECK(information.st_mtime - time(NULL) < 60 && time(NULL) - information.st_mtime < 60);

	/* fdopen: a stream on a descriptor */
	handle = _open(FOLDER "/descriptor.bin", _O_RDONLY);
	{
		FILE *stream = fdopen(handle, "rb");

		CHECK(stream != NULL);
		if (stream)
		{
			char end[4] = "";

			fseek(stream, -3, SEEK_END);
			CHECK(fread(end, 1, 3, stream) == 3 && !memcmp(end, "END", 3));
			fclose(stream);
		}
	}
	/* a read-only file */
	second = _open(FOLDER "/readonly.bin", _O_CREAT | _O_WRONLY, _S_IREAD);
	CHECK(second >= 0);
	_close(second);
	CHECK(_access(FOLDER "/readonly.bin", 0) == 0);
	CHECK(_access(FOLDER "/readonly.bin", 2) == -1 && errno == EACCES);
	SetFileAttributesA(FOLDER "\\readonly.bin", FILE_ATTRIBUTE_NORMAL);
	CHECK(_unlink(FOLDER "/readonly.bin") == 0);
	CHECK(_access(FOLDER "/readonly.bin", 0) == -1 && errno == ENOENT);
	CHECK(_unlink(FOLDER "/descriptor.bin") == 0);

	/* the standard descriptors */
	CHECK(_write(1, "# written to descriptor 1\n", 26) == 26);
}

static void test_directories(void)
{
	struct _stat information;
	char here[260];

	_rmdir(FOLDER "/sub/deeper");
	_rmdir(FOLDER "/sub");
	CHECK(_mkdir(FOLDER "/sub") == 0);
	CHECK(_mkdir(FOLDER "/sub/") == -1 && errno == EEXIST);
	CHECK(_mkdir(FOLDER "/sub/deeper") == 0);
	CHECK(_stat(FOLDER "/sub/", &information) == 0 && (information.st_mode & _S_IFDIR));
	CHECK(_access(FOLDER "/sub", 2) == 0);
	CHECK(_open(FOLDER "/sub", _O_RDONLY) == -1 && errno == EACCES);
	CHECK(_rmdir(FOLDER "/sub") == -1);
	CHECK(_rmdir(FOLDER "/sub/deeper") == 0 && _rmdir(FOLDER "/sub") == 0);
	CHECK(_stat(FOLDER "/sub", &information) == -1 && errno == ENOENT);
	CHECK(_getcwd(here, sizeof(here)) != NULL);
	{
		char *allocated = _getcwd(NULL, 0);

		CHECK(allocated && !strcmp(allocated, here));
		free(allocated);
	}
	CHECK(_chdir(FOLDER) == 0);
	CHECK(fopen("../" "t_files.exe", "rb") != NULL);
	CHECK(_chdir(here) == 0);
	{
		/* a drive's root */
		char root[4] = { here[0], ':', '\\', 0 };

		CHECK(_stat(root, &information) == 0 && (information.st_mode & _S_IFDIR));
	}
	CHECK(_stat("C:/no such folder/file", &information) == -1 && errno == ENOENT);
}

int main(void)
{
	_mkdir(FOLDER);
	test_streams();
	test_descriptors();
	test_directories();
	remove(FOLDER "/stream.bin");
	CHECK(_rmdir(FOLDER) == 0);
	return test_finish("t_files");
}
