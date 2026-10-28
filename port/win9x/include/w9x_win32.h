/*
W9X_WIN32.H

Force-included ahead of every unit of the 9x build that includes the
Windows SDK's <windows.h>: the Windows-facing platform units
(port/windows/src/win32_*.c) and the C runtime (port/win9x/src,
port/win9x/pdclib). The SDK's headers are the Windows 10 SDK's; the 9x
build compiles them against its own C runtime (nxdk's PDCLib) instead of
the UCRT, and asks them for the Windows 95 / NT 4.0 interface.

The version macros only hide the newest declarations: most of what 9x
lacks (TryEnterCriticalSection, getaddrinfo, the W functions...) is still
declared. What the build may import is decided by the import lists (the
.def files of port/win9x/lib), and tools/win9x_check.py checks the linked
image against them.
*/

#ifndef __W9X_WIN32_H
#define __W9X_WIN32_H

/* Windows 95 / NT 4.0 headers; Windows 98 for the few 98 additions the
build uses (GetFileAttributesExA, GetDiskFreeSpaceExA) */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0400
#endif
#ifndef _WIN32_WINDOWS
#define _WIN32_WINDOWS 0x0410
#endif
#ifndef WINVER
#define WINVER 0x0400
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

/* what the UCRT's <vcruntime.h> gives <windows.h> in C: the MSVC ABI's
16-bit wchar_t (clang's Microsoft target has the same size) */
#ifndef _WCHAR_T_DEFINED
#define _WCHAR_T_DEFINED
typedef unsigned short wchar_t;
#endif

#endif /* __W9X_WIN32_H */
