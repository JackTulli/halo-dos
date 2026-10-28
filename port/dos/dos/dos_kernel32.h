/*
DOS_KERNEL32.H

What the DOS units share besides dos_dpmi.h: the program's surroundings as
the start-up read them, and the log.
*/

#ifndef __DOS_KERNEL32_H
#define __DOS_KERNEL32_H

/* the command line as Windows would give it (the program's path first) */
const char *dos_command_line(void);
/* the program's full path ("C:\GAMES\HALO\HALO.EXE") */
const char *dos_program_path(void);
/* DOS's environment block: NAME=value strings, an empty one last */
const char *dos_environment(void);

/* a line for the log file (the console as well until the video mode
changes): OutputDebugStringA's and the platform's */
void dos_log_text(const char *text);
/* the console is the screen's text (before the game's video mode and after
it): messages for the player go there too */
void dos_console_set_visible(int visible);

/* DOS's local time less the coordinated time (UTC), in 100 ns ticks: an
internet time server's answer (dos_net.cpp) */
void dos_set_utc_offset(long long ticks);

#endif /* __DOS_KERNEL32_H */
