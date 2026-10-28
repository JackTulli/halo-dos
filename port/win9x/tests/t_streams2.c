/*
T_STREAMS2.C

More of fopen's modes than t_files.c, against the UCRT's (built a second
time as a reference and compared line for line): "a+" and its spellings,
reading and appending on one stream, ftell after an append that was
positioned elsewhere (a note only: PDCLib answers from where the stream was
put, the UCRT from the file's end), the game's own "a+b" and "a+t" opens,
"r+" past the end, and a descriptor opened with _O_APPEND. Each line is
what the stream answered; the file's contents are printed after each step.
*/

#include <windows.h>

#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define NAME "build/win9x/tests/streams2.txt"

static void contents(const char *step)
{
	char text[256];
	size_t length = 0, i;
	FILE *file = fopen(NAME, "rb");

	if (file)
	{
		length = fread(text, 1, sizeof(text) - 1, file);
		fclose(file);
	}
	/* (line ends spelt out: the UCRT's standard output would turn a CR LF
	into CR CR LF) */
	printf("%s: [", step);
	for (i = 0; i < length; i++)
	{
		if (text[i] == '\r')
			printf("\\r");
		else if (text[i] == '\n')
			printf("\\n");
		else
			putchar(text[i]);
	}
	printf("] (%u bytes)\n", (unsigned)length);
}

static void fresh(const char *text)
{
	FILE *file = fopen(NAME, "wb");

	fputs(text, file);
	fclose(file);
}

int main(void)
{
	static const char *const modes[] = { "a+", "a+b", "ab+", "at+" };
	char line[64];
	size_t i;
	FILE *file;
	int handle;

	CreateDirectoryA("build\\win9x\\tests", NULL);
	for (i = 0; i < sizeof(modes) / sizeof(modes[0]); i++)
	{
		fresh("0123456789");
		file = fopen(NAME, modes[i]);
		printf("fopen %s: %s\n", modes[i], file ? "opened" : "NULL");
		if (!file)
			continue;
		/* reads from the start after a seek */
		printf("  fseek 0: %d\n", fseek(file, 0, SEEK_SET));
		memset(line, 0, sizeof(line));
		printf("  fread 4: %u [%s]\n", (unsigned)fread(line, 1, 4, file), line);
		/* writes go to the end whatever the position */
		printf("  fseek 2: %d\n", fseek(file, 2, SEEK_SET));
		printf("  fputs: %d\n", fputs("AB", file) >= 0);
		/* (a note: the UCRT answers the file's end plus the bytes waiting,
		PDCLib where the stream was put plus them) */
		printf("#  ftell after the append: %ld\n", ftell(file));
		printf("  fflush: %d\n", fflush(file));
		printf("  ftell after the flush: %ld\n", ftell(file));
		printf("  fseek 0: %d\n", fseek(file, 0, SEEK_SET));
		memset(line, 0, sizeof(line));
		printf("  fread all: %u [%s]\n", (unsigned)fread(line, 1, sizeof(line) - 1, file), line);
		fclose(file);
		contents("  file");
	}

	/* the game's debug log (source/cseries/errors.c): "a+b", written only,
	opened again and appended to */
	remove(NAME);
	for (i = 0; i < 2; i++)
	{
		file = fopen(NAME, "a+b");
		printf("debug log %u: %s\n", (unsigned)i, file ? "opened" : "NULL");
		if (!file)
			continue;
		printf("  fprintf: %d\n", fprintf(file, "line %u\r\n", (unsigned)i));
		printf("  fflush: %d\n", fflush(file));
		fclose(file);
	}
	contents("debug log");

	/* the game's init file (source/main/main.c): "wt", then "a+t" */
	remove(NAME);
	file = fopen(NAME, "wt");
	printf("fopen wt: %s\n", file ? "opened" : "NULL");
	if (file)
	{
		fputs("map_name a10\n", file);
		fclose(file);
	}
	file = fopen(NAME, "a+t");
	printf("fopen a+t: %s\n", file ? "opened" : "NULL");
	if (file)
	{
		fputs(";core\n", file);
		fclose(file);
	}
	/* (no contents: the UCRT writes CR LF in text mode, this runtime LF) */

	/* "a": ftell straight after a positioned append, before any flush */
	fresh("0123456789");
	file = fopen(NAME, "a");
	if (file)
	{
		printf("a: ftell at open: %ld\n", ftell(file));
		fputs("xy", file);
		printf("# a: ftell after fputs: %ld\n", ftell(file));
		fclose(file);
	}
	contents("a");

	/* "r+": a write straight after the open, then a read after a seek */
	fresh("0123");
	file = fopen(NAME, "r+b");
	if (file)
	{
		printf("r+: fputs at open: %d\n", fputs("AB", file) >= 0);
		printf("r+: fseek 1: %d\n", fseek(file, 1, SEEK_SET));
		printf("r+: fgetc: %c\n", fgetc(file));
		fclose(file);
	}
	contents("r+");

	/* "r+": a write past the end after a seek there */
	fresh("0123");
	file = fopen(NAME, "r+b");
	if (file)
	{
		printf("r+: fseek 6: %d\n", fseek(file, 6, SEEK_SET));
		fputs("Z", file);
		fclose(file);
	}
	{
		FILE *back = fopen(NAME, "rb");
		int c;

		printf("r+: bytes");
		while (back && (c = fgetc(back)) != EOF)
			printf(" %02x", c);
		printf("\n");
		if (back)
			fclose(back);
	}

	/* a descriptor in append mode, positioned at 0 */
	fresh("0123456789");
	handle = _open(NAME, _O_RDWR | _O_APPEND | _O_BINARY);
	printf("_open append: %s\n", handle >= 0 ? "opened" : "failed");
	if (handle >= 0)
	{
		printf("  _lseek 0: %ld\n", _lseek(handle, 0, SEEK_SET));
		printf("  _write: %d\n", _write(handle, "QQ", 2));
		printf("  _lseek cur: %ld\n", _lseek(handle, 0, SEEK_CUR));
		_close(handle);
	}
	contents("_open append");

	remove(NAME);
	fflush(stdout);
	return 0;
}
