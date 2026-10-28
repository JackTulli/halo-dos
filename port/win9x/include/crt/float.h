/*
FLOAT.H

<float.h> of the 9x build: the compiler's and PDCLib's, the Xbox build's
MSVC control-word interface (port/xbox/include/crt/float.h) for units that
do not see that one, and the status-word bits, which the 9x build's
_statusfp and _clearfp report for the x87 and SSE together
(port/win9x/src/w9x_crt.c).
*/

#ifndef __HALO_WIN9X_FLOAT_H
#define __HALO_WIN9X_FLOAT_H

#include_next <float.h>

#ifndef __HALO_XBOX_FLOAT_H
#define _MCW_EM 0x0008001f
#define _MCW_IC 0x00040000
#define _MCW_RC 0x00000300
#define _MCW_PC 0x00030000

#define _EM_INEXACT 0x00000001
#define _EM_UNDERFLOW 0x00000002
#define _EM_OVERFLOW 0x00000004
#define _EM_ZERODIVIDE 0x00000008
#define _EM_INVALID 0x00000010
#define _EM_DENORMAL 0x00080000

#define _RC_NEAR 0x00000000
#define _RC_DOWN 0x00000100
#define _RC_UP 0x00000200
#define _RC_CHOP 0x00000300

#define _PC_24 0x00020000
#define _PC_53 0x00010000
#define _PC_64 0x00000000

#define _IC_AFFINE 0x00040000
#define _IC_PROJECTIVE 0x00000000

#define CW_DEFAULT (_RC_NEAR + _PC_53 + _EM_INVALID + _EM_ZERODIVIDE + _EM_OVERFLOW + _EM_UNDERFLOW + _EM_INEXACT + _EM_DENORMAL)

unsigned int _control87(unsigned int new_value, unsigned int mask);
unsigned int _controlfp(unsigned int new_value, unsigned int mask);
unsigned int _clearfp(void);
unsigned int _statusfp(void);
int _isnan(double value);
int _finite(double value);
#endif

/* SSE2's denormal control (MXCSR's DAZ and FTZ): accepted and ignored */
#define _MCW_DN 0x03000000
#define _DN_SAVE 0x00000000
#define _DN_FLUSH 0x01000000

#define _SW_INEXACT 0x00000001
#define _SW_UNDERFLOW 0x00000002
#define _SW_OVERFLOW 0x00000004
#define _SW_ZERODIVIDE 0x00000008
#define _SW_INVALID 0x00000010
#define _SW_DENORMAL 0x00080000

#endif
