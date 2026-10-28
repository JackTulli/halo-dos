/*
PLATFORM.H

Shared internals of the DOS and Windows 95 builds' Xbox-facing platform
layer (port/dos/xdk, and the Linux units these builds compile again:
port/linux/src/platform.h defers here under HALO_DOS). These files are
compiled with the game's ABI and see the Xbox SDK's declarations
(port/include/xdk), so every definition is type-checked against the SDK
prototype it replaces. The system below is reached through halo_system.h.
*/

#ifndef __HALO_DOS_PLATFORM_H
#define __HALO_DOS_PLATFORM_H

#define DEBUG_KEYBOARD
#include "../../../chupathingyce/port/linux/include/halo_linux_winsock_names.h"
#include <xtl.h>
#include <xbdm.h>
#include <d3d8perf.h>
#define HALO_LINUX_WINSOCK_NAMES_UNDEFINE
#include "../../../chupathingyce/port/linux/include/halo_linux_winsock_names.h"
#undef HALO_LINUX_WINSOCK_NAMES_UNDEFINE

#include "halo_system.h"
/* (one thread: port/dos/include/pthread.h) */
#include "../include/threads/pthread.h"

/* ---------- sound */

/* the mixer's turn (xdk_dsound.c): the device kept filled; DirectSoundDoWork
runs it every frame, the game's waits and the loading screen between */
void dsound_pump(void);

/* ---------- logging */

void platform_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

/* reports an SDK entry point these builds do not implement, once per name;
the caller then behaves as a failing or empty call */
void platform_unimplemented(const char *name);
#define PLATFORM_UNIMPLEMENTED() platform_unimplemented(__func__)

/* ---------- paths */

/* an Xbox path (d:\maps\a10.map, t:\..., u:\..., z:\...) as a path on this
computer: below the program's folder (d: is the folder itself, the others
folders of their own: SAVE for t: and u:, CACHE for z:) */
void platform_translate_path(const char *xbox_path, char *host_path, unsigned long host_path_size);
const char *platform_data_root(void);

#endif /* __HALO_DOS_PLATFORM_H */
