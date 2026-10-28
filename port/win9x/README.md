# The C runtime (from the Windows 98 build)

This folder comes from halo-ce-universal's Windows 98 SE / Me build. The DOS
and Windows 95 builds use its C runtime: nxdk's PDCLib over KERNEL32, with
the 9x build's own pieces.

- `pdclib/`: PDCLib's platform layer for KERNEL32 (and PDCLib functions
  replaced here);
- `src/`: start-up, `malloc`, `printf`/`strtod`, `_chkstk`, thread-local
  storage and the processor check;
- `include/`: the runtime's headers and `w9x_win32.h`, the Win32 subset the
  runtime sees;
- `lib/*.def`: the only DLL functions the programs may import (the Windows 95
  build is checked against them: Windows 95 has no others);
- `port.json`: which PDCLib and nxdk units the runtime takes or replaces
  (read by `tools/dos_build.py`).

On DOS, `port/dos/dos/dos_kernel32.c` answers the runtime's KERNEL32 calls
over DPMI and DOS, so the same runtime serves both programs.

`platform/` (the 98 build's window, input and sound layer) and `tests/`
came along with the folder; the DOS and Windows 95 builds do not compile
them.
