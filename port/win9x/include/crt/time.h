/*
TIME.H

<time.h> of the 9x build: PDCLib's (with the Xbox build's additions for
the game), and the reentrant local time the platform layer uses
(port/win9x/src/w9x_crt.c).
*/

#ifndef __HALO_WIN9X_TIME_H
#define __HALO_WIN9X_TIME_H

#include_next <time.h>

struct tm *localtime_r(const time_t *timer, struct tm *result);

#endif
