# Tests of the settings window

These run on the PC that builds the game, not on Windows 98: they check
the launcher's layout (`port/win9x/platform/w9x_launcher.c`) and the
settings file's table interface and writer (`port/linux/src/port_config.c`)
without opening a window.

- `t_launcher.c` includes the launcher's unit with `LAUNCHER_LAYOUT_ONLY`,
  builds the dialog template for the settings it is given on standard
  input and prints what the template's bytes say of every control, which
  setting each control edits and which settings the Advanced list holds.
- `t_config.c` builds `port_config.c` as the 9x build does (`HALO_WIN9X`)
  over the UCRT, with the launcher and the few Windows-facing functions
  stood in for, and runs scenes: the table, the first run (no file; the
  launcher is shown the settings and its change is written), a file as a
  player left it (comments, unknown keys, spacing, CRLF, a comment after a
  value: only the values change), a file that does not parse, and the
  command line (`-nolauncher`, `-launcher`, an invite link, Quit).
- `launcher_check.py` reads the table from `port_config.c` for the 9x
  build and for the native Windows build, runs `t_launcher` for each and
  checks: ids used once, every control inside the dialog and its page,
  no two controls that show at once overlapping, the window within 600
  by 440 pixels at 96 dpi, every setting the build reads bound to exactly
  one control or to the Advanced list, and the hand-made controls
  (renderer, mode, field of view, ...) in place. With `--config-test` it
  also runs `t_config`'s scenes and compares the table it prints with the
  source's.

Build them in the shell the Windows build is made in (the Windows SDK and
the UCRT in `INCLUDE` and `LIB`), from the repository's root:

```sh
OUT=build/win9x/tests/launcher
mkdir -p $OUT
COMMON="--target=i686-pc-windows-msvc -march=pentium3 -mfpmath=sse -ffp-eval-method=source -ffp-contract=off
  -fno-math-errno -O1 -fms-extensions -fno-strict-aliasing -std=gnu11 -D_USE_32BIT_TIME_T
  -D_CRT_SECURE_NO_WARNINGS -D_CRT_NONSTDC_NO_WARNINGS"
clang $COMMON -Wall -DWIN32_LEAN_AND_MEAN -DNOMINMAX -Iport/linux/src \
  -c port/win9x/tests/launcher/t_launcher.c -o $OUT/t_launcher.o
clang --target=i686-pc-windows-msvc -fuse-ld=lld $OUT/t_launcher.o -o $OUT/t_launcher.exe -luser32
FLAGS="$COMMON -DHALO_LINUX_PLATFORM_LAYER -DHALO_WIN9X=1 -include port/windows/include/halo_windows_prefix.h
  -Iport/windows/include/posix -Iport/windows/include/crt -Iport/linux/src -Iport/windows/include
  -iquote port/linux/include -Isource -Isource/cseries -Iport/include/xdk -Iport/third_party/tomlc17
  -Iport/win9x/platform"
clang $FLAGS -w -c port/linux/src/port_config.c -o $OUT/port_config.o
clang $COMMON -w -c port/third_party/tomlc17/tomlc17.c -o $OUT/tomlc17.o
clang $COMMON -Wall -Iport/windows/include/posix -Iport/linux/src -Iport/win9x/tests \
  -c port/win9x/tests/launcher/t_config.c -o $OUT/t_config.o
clang --target=i686-pc-windows-msvc -fuse-ld=lld $OUT/port_config.o $OUT/tomlc17.o $OUT/t_config.o -o $OUT/t_config.exe
python3 port/win9x/tests/launcher/launcher_check.py $OUT/t_launcher.exe --config-test $OUT/t_config.exe
```

The script ends with `all checks passed`, or lists what failed.
