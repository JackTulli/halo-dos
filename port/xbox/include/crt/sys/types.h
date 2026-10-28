/*
SYS/TYPES.H

The few POSIX types the game's libtiff names; nxdk's C library has no
such header.
*/

#ifndef __HALO_XBOX_SYS_TYPES_H
#define __HALO_XBOX_SYS_TYPES_H

#include <stddef.h>

typedef long off_t;
typedef int ssize_t;
typedef unsigned short ino_t;
typedef unsigned int dev_t;
typedef unsigned short mode_t;
typedef short nlink_t;
typedef short uid_t;
typedef short gid_t;
typedef int pid_t;

#endif
