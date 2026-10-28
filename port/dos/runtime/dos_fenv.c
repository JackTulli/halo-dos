/*
DOS_FENV.C

The floating-point environment's flags for the DOS and Windows 95 builds'
C runtime, on the x87 alone (no SSE unit to ask): nxdk's nearbyint tests
and clears the inexact flag with them.
*/

#include <fenv.h>

int feclearexcept(int excepts)
{
	if (excepts & FE_ALL_EXCEPT)
		__asm__ __volatile__("fnclex");
	return 0;
}

int fetestexcept(int excepts)
{
	unsigned short status;

	__asm__ __volatile__("fnstsw %0" : "=m"(status));
	return status & excepts & FE_ALL_EXCEPT;
}
