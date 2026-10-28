/*
HALO_D3D8.H

Direct3D 8 for the Direct3D renderer (port/dos/DIRECT3D.md): Wine's
d3d8.h, d3d8types.h and d3d8caps.h (LGPL 2.1 or later, beside this file;
the Windows SDK no longer has them) over the Windows SDK's headers as the
Windows 95 units compile them (WINVER 0x0400). Include this, not <d3d8.h>.
*/

#ifndef __HALO_D3D8_H
#define __HALO_D3D8_H

#ifndef COBJMACROS
#define COBJMACROS
#endif

#include <windows.h>

/* (a Wine spelling of a long constant) */
#ifndef __MSABI_LONG
#define __MSABI_LONG(x) x##L
#endif

/* (the SDK declares monitors for Windows 2000 and later only; Direct3D 8's
interface names the type) */
#if WINVER < 0x0500 && !defined(HMONITOR_DECLARED)
DECLARE_HANDLE(HMONITOR);
#define HMONITOR_DECLARED
#endif

#include <d3d8.h>

#endif /* __HALO_D3D8_H */
