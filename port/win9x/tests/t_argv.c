/*
T_ARGV.C

argv as the start-up code splits it (port/win9x/src/w9x_start.c), against
the UCRT's: prints argc and each argument between brackets. Run with raw
command lines (tools/win9x_build.py cannot pass one; the scratch script
that runs this test gives CreateProcess the line as it is) by both builds,
and the outputs compared.
*/

#include <stdio.h>

int main(int argc, char **argv)
{
	int i;

	printf("argc %d\n", argc);
	for (i = 0; i < argc; i++)
		printf("argv[%d] [%s]\n", i, argv[i]);
	return 0;
}
