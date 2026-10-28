/*
MATH.H

nxdk's <math.h> for the game, with sin, pow and the rest the same on every
port (port/include/halo_math.h), and the MSVC names.
*/

#ifndef __HALO_XBOX_MATH_H
#define __HALO_XBOX_MATH_H

#include_next <math.h>

/* exact in IEEE arithmetic, so the same on every port: the x87's own
instructions rather than calls into the C library (the game calls sqrt
from some five hundred places) */
#define sqrt(x) __builtin_sqrt(x)
#define sqrtf(x) __builtin_sqrtf(x)
#define fabs(x) __builtin_fabs(x)
#define fabsf(x) __builtin_fabsf(x)

double _hypot(double x, double y);
double _copysign(double x, double y);

#include "../../../../chupathingyce/port/include/halo_math.h"

#endif
