/*
T_OPEN.C

The DOS file layer's opening of long names on DOS without long file names:
each name given on the command line, opened through dos_open and through
the C runtime (fopen), and the log's creation. Prints what each returned.
*/

#include <stdio.h>
#include <string.h>

#include "dos_dpmi.h"
#include "dos_files.h"
#include "dos_kernel32.h"

int main(int argc, char **argv)
{
	printf("long file names: %d\n", dos_long_file_names());
	for (int i = 1; i < argc; i++)
	{
		unsigned short handle = 0;
		int error = dos_open(argv[i], DOS_OPEN_READ, DOS_IF_EXISTS_OPEN, &handle);
		FILE *file;

		printf("%s: dos_open %d (handle %u)\n", argv[i], error, handle);
		if (!error)
			dos_close(handle);
		file = fopen(argv[i], "rb");
		printf("%s: fopen %s\n", argv[i], file ? "ok" : "failed");
		if (file)
			fclose(file);
	}
	{
		unsigned short handle = 0;
		unsigned long done = 0;
		int error = dos_open("C:\\T_OPEN.TXT", DOS_OPEN_WRITE, DOS_IF_EXISTS_TRUNCATE | DOS_IF_MISSING_CREATE, &handle);

		printf("create C:\\T_OPEN.TXT: %d (handle %u)\n", error, handle);
		if (!error)
		{
			error = dos_write(handle, "written\r\n", 9, &done);
			printf("write: %d, %lu bytes\n", error, done);
			dos_close(handle);
		}
	}
	{
		static const char *const names[] = { "C:\\HALO.LOG", "C:\\HALO2.LOG", "C:\\HALOLOG.TXT" };

		for (int i = 0; i < 3; i++)
		{
			unsigned short handle = 0;
			int error = dos_open(names[i], DOS_OPEN_WRITE, DOS_IF_EXISTS_TRUNCATE | DOS_IF_MISSING_CREATE, &handle);

			printf("create %s: %d\n", names[i], error);
			if (!error)
				dos_close(handle);
		}
	}
	dos_log_text("t_open: a line for the log");
	printf("program %s\n", dos_program_path());
	return 0;
}
