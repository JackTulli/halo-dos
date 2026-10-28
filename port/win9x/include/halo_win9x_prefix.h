/*
HALO_WIN9X_PREFIX.H

Force-included ahead of every game unit and every Xbox-facing platform unit
in the Windows 98 SE / Me build (clang -include). The 9x build is the
native Windows build on an older system: the same game, the same platform
layer and the same Windows names (halo_windows_prefix.h), with the Xbox
build's C runtime (nxdk's PDCLib, port/win9x/pdclib) in place of the UCRT,
which needs Windows Vista. HALO_WIN9X marks the few places where the two
differ; HALO_WINDOWS and HALO_LINUX stay defined.

The Windows-facing units (port/windows/src/win32_*.c and the runtime in
port/win9x/src) never see this header: they see w9x_win32.h.
*/

#ifndef __HALO_WIN9X_PREFIX_H
#define __HALO_WIN9X_PREFIX_H

#define HALO_WIN9X 1

#include "../../../chupathingyce/port/windows/include/halo_windows_prefix.h"

#endif /* __HALO_WIN9X_PREFIX_H */
