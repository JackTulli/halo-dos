/*
EXCPT.H

What the UCRT's <excpt.h> gives the Windows SDK's <winnt.h> and the game:
the exception dispositions. The game's __try/__except is compiled away
(halo_windows_prefix.h); the 9x build's one exception handler is the
start-up processor check's (port/win9x/src/w9x_cpucheck.c).
*/

#ifndef __HALO_WIN9X_EXCPT_H
#define __HALO_WIN9X_EXCPT_H

typedef enum _EXCEPTION_DISPOSITION
{
	ExceptionContinueExecution,
	ExceptionContinueSearch,
	ExceptionNestedException,
	ExceptionCollidedUnwind
} EXCEPTION_DISPOSITION;

#define EXCEPTION_EXECUTE_HANDLER 1
#define EXCEPTION_CONTINUE_SEARCH 0
#define EXCEPTION_CONTINUE_EXECUTION -1

#ifndef GetExceptionCode
#define GetExceptionCode() 0UL
#define GetExceptionInformation() ((struct _EXCEPTION_POINTERS *)0)
#define AbnormalTermination() 0
#endif

#endif
