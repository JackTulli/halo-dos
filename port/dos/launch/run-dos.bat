@echo off
rem Runs HALO.EXE in DOSBox-X, with this folder as drive C:. The maps go in
rem MAPS here, the settings are in HALODOS.CFG (320x200 in 256 colours with
rem the frame rate shown, until the menu's SETTINGS page changes them) and the
rem last run's log is HALO.LOG.
rem The first start on a map unpacks it into CACHE\MAPS (about a minute).
rem
rem   run-dos            as fast as this PC can emulate
rem   run-dos 133        a Pentium of that speed, in MHz (for slow machines)
rem
rem (DOSBox-X's cycles a millisecond are the emulated processor's speed. At
rem "max" it runs as many as the PC can, and the processor's time stamp
rem counter, the game's clock, ticks at a steady 1 GHz in real time: HALO.LOG's
rem first line then says 999 MHz whatever the speed. At a fixed speed the
rem counter counts the cycles, and the line shows the speed; a PC that cannot
rem keep up then runs the game slower than real time.)
rem
rem The network: with the MinGW build of DOSBox-X (the "slirp" network: the
rem dosbox-x-mingw64-...-portable.zip of DOSBox-X's releases, mingw\dosbox-x.exe)
rem the DOS machine gets an NE2000 card on DOSBox-X's own network, which
rem shares this PC's connection (the DOS machine is 10.0.2.15 there and gets
rem that address from DHCP itself), and the card's packet driver, NE2000.COM
rem from DOSBox-X's drive Z:, is loaded before the game. ONLINE then
rem finds the games hosted on this PC and online, and joins them; to host a
rem game for the PCs of this LAN, run-dos-net host. The installer's build in
rem C:\DOSBox-X has no network: the game runs without one there.
rem On a real DOS PC, load the network card's own packet driver before
rem HALO.EXE (the game finds it on interrupt 60h to 80h).
setlocal

rem where DOSBox-X is, the first found: the DOSBOX_X variable (its
rem dosbox-x.exe); the MinGW build, which has the network, unpacked into a
rem DOSBox-X folder beside this file (dosbox-x-mingw64-...-portable.zip from
rem DOSBox-X's releases: DOSBox-X\mingw-build\mingw\dosbox-x.exe); the
rem installer's build in C:\DOSBox-X or Program Files (no network)
set DOSBOX=
for %%D in ("%DOSBOX_X%" "%~dp0DOSBox-X\mingw-build\mingw\dosbox-x.exe" "%~dp0DOSBox-X\mingw\dosbox-x.exe" "C:\DOSBox-X\dosbox-x.exe" "%ProgramFiles%\DOSBox-X\dosbox-x.exe") do if not defined DOSBOX if exist "%%~D" set "DOSBOX=%%~D"
rem settings for this run only, over HALODOS.CFG's (as -name value: -play
rem bloodgulch skips the menu)
set GAME=

set CYCLES=max
set RDTSC=1000000
if not "%~1"=="" (
	set CYCLES=fixed %~1000
	set RDTSC=0
)
if /i "%~1"=="max" (
	set CYCLES=max
	set RDTSC=1000000
)

if not defined DOSBOX (
	echo DOSBox-X was not found. Install it ^(dosbox-x.com^), or unpack its MinGW
	echo portable zip into a folder named DOSBox-X here ^(that build has the
	echo network^), or set DOSBOX_X to the path of its dosbox-x.exe.
	pause
	exit /b 1
)
rem the network card and its packet driver, where DOSBox-X has a network
rem (a MinGW build); the game's LAN search goes to this PC (10.0.2.2 on
rem DOSBox-X's network: broadcasts do not leave it)
set NETWORK=
set PACKET_DRIVER=
echo "%DOSBOX%" | find /i "mingw" >nul
if not errorlevel 1 (
	set NETWORK=-set "ne2000 ne2000=true" -set "ne2000 backend=slirp" -set "ne2000 nicbase=300" -set "ne2000 nicirq=3"
	set PACKET_DRIVER=-c "Z:\SYSTEM\NE2000 0x60 3 0x300"
	set GAME=%GAME% -network.broadcast 10.0.2.2
)
if not exist "%~dp0HALO.EXE" (
	echo HALO.EXE is not in %~dp0: build it first ^(ninja -f build/dos/build.ninja dos^).
	pause
	exit /b 1
)
rem the maps: MAPS here; else the folder that has them, asked for once and
rem linked here as MAPS (a junction: nothing is copied)
if exist "%~dp0MAPS\*.map" goto maps_found
echo There are no maps in %~dp0MAPS.
echo Type the folder that has the Xbox maps ^(bloodgulch.map and the others^) and
echo press Enter: it is linked here as MAPS ^(nothing is copied^), once.
set MAPS_FROM=
set /p MAPS_FROM=Maps folder:
if not defined MAPS_FROM exit /b 1
set MAPS_FROM=%MAPS_FROM:"=%
if not exist "%MAPS_FROM%\*.map" (
	echo There are no .map files in %MAPS_FROM%.
	pause
	exit /b 1
)
if exist "%~dp0MAPS" rmdir "%~dp0MAPS" 2>nul
mklink /J "%~dp0MAPS" "%MAPS_FROM%" >nul
if not exist "%~dp0MAPS\*.map" (
	echo MAPS could not be linked to %MAPS_FROM%: copy the maps into %~dp0MAPS instead.
	pause
	exit /b 1
)
:maps_found

rem DOSBox-X mounts this folder as C: and runs the game there; the prompt
rem stays open afterwards, so a message the game printed can be read. The
rem emulated 3dfx Voodoo Graphics is there for SETTINGS > RENDERER > 3DFX
rem VOODOO (its software emulation, the one the game was tried on). The
rem dynamic core does the floating point too (DOSBox-X's advice for DOS
rem games: else every x87 instruction drops to the slow core, ten times
rem slower here)
set HERE=%~dp0
set HERE=%HERE:~0,-1%
start "" "%DOSBOX%" -nopromptfolder -defaultconf -fastlaunch ^
	-set "dosbox quit warning=false" -set "dosbox memsize=64" -set "dosbox machine=svga_s3" ^
	-set "cpu cputype=pentium" -set "cpu core=dynamic_x86" -set "cpu use dynamic core with fpu=true" ^
	-set "cpu cycles=%CYCLES%" -set "cpu rdtsc rate=%RDTSC%" ^
	-set "dos lfn=true" -set "sdl autolock=true" ^
	-set "sdl output=opengl" -set "sdl windowresolution=1280x960" -set "render aspect=true" ^
	-set "dosbox enable pci bus=true" -set "voodoo voodoo_card=software" ^
	%NETWORK% ^
	-c "mount c \"%HERE%\"" -c "c:" %PACKET_DRIVER% -c "PLAY.BAT %GAME%"
