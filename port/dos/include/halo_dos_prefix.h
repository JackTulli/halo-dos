/*
HALO_DOS_PREFIX.H

Force-included ahead of every game unit and every Xbox-facing platform unit
of the MS-DOS and Windows 95 builds (clang -include). Both are one build of
the game: the same objects, compiled for the Microsoft target (the game's
ABI, as on Windows) and for any Pentium (x87 arithmetic, no MMX or SSE),
linked twice over two platform layers (port/dos/README.md).

The game is the N64 build's cut-down Halo: its renderer, menus, sound
engine, scripts, actor AI and networking are left out (port/dos/port.json)
and the platform layer draws and plays it instead. HALO_DOS marks what the
game does differently for that; HALO_LINUX and the Windows build's names
stay as they are, since the Microsoft target is the Windows build's.
*/

#ifndef __HALO_DOS_PREFIX_H
#define __HALO_DOS_PREFIX_H

#define HALO_DOS 1
/* the work only the port's own renderer leaves undrawn, cut out of the game
(lights, particle systems, contrails: tools/dos_hooks.py), but not in the
Direct3D build (HALO_D3D), which draws with Halo's own renderer
(port/dos/DIRECT3D.md) */
#ifndef HALO_D3D
#define HALO_DOS_TRIMS 1
#endif
/* the desktop builds' extras a console leaves out (halo_port_features.h:
the telnet console, the high-res HUD, the tag schema checks, the updater,
Discord...), with the desktop builds' network limits (no
HALO_CONSOLE_PLAYERS): their games and ours are the same on the wire */
#define HALO_CONSOLE 1

#include "../../../chupathingyce/port/windows/include/halo_windows_prefix.h"

/* the Xbox SDK functions the platform layer defines (port/dos/xdk) beyond
the Windows build's list (halo_windows_api_names.h): the Windows 95 build's
Win32 half calls the real ones */
#define FindClose halo_xbox_FindClose

#endif /* __HALO_DOS_PREFIX_H */
