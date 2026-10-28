/*
TIME.H

nxdk's <time.h> (PDCLib) plus the reentrant conversion the platform layer
uses (port/xbox/src/msvc_crt.c).
*/

#ifndef __HALO_XBOX_TIME_H
#define __HALO_XBOX_TIME_H

#include_next <time.h>

struct tm *localtime_r(const time_t *timer, struct tm *result);

#endif
