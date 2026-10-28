@echo off
rem Runs HALO.EXE in DOSBox-X as run-dos does, with a network: an NE2000
rem card on DOSBox-X's own network (it shares this PC's connection; the DOS
rem machine is 10.0.2.15 behind it, and gets that address from DHCP itself),
rem and the card's packet driver, NE2000.COM from DOSBox-X's drive Z:, loaded
rem before the game. The game's ONLINE pages then find the games hosted
rem on this PC (ChupathingyCE's, HALO95.EXE's) and joins them. run-dos does
rem all this too (where this DOSBox-X build is there); this one is for
rem hosting a game for the PCs of this LAN:
rem
rem   run-dos-net          as fast as this PC can emulate
rem   run-dos-net host     hosting from DOS: this PC's ports 5150 are passed
rem                        to the DOS machine, and games on this PC join it
rem                        through 127.0.0.1 (their network.broadcast)
rem
rem Over the internet (as ChupathingyCE plays): BROWSE GAMES lists the
rem online game list's games beside the LAN's. A hosted game is listed
rem there (network.host_public = false in HALODOS.CFG keeps it off the
rem list), and its invite link is written to INVITE.TXT beside HALO.EXE. To
rem join a game by its link, put the link in JOIN.TXT beside HALO.EXE before
rem starting: the game reaches its host, and the game then shows on the JOIN
rem page (JOIN.TXT becomes JOIN.OLD: a link works only while its host runs).
rem
rem It needs a DOSBox-X build with the "slirp" network (the MinGW builds of
rem DOSBox-X's releases: dosbox-x-mingw64-...-portable.zip, mingw\dosbox-x.exe);
rem the Visual Studio build the installer puts in C:\DOSBox-X has no network.
rem On a real DOS PC, load the network card's own packet driver instead
rem (the game finds it on interrupt 60h to 80h) and run HALO.EXE.
setlocal

rem where the MinGW build of DOSBox-X is, the first found: the DOSBOX_X
rem variable (its dosbox-x.exe), or its portable zip unpacked into a
rem DOSBox-X folder beside this file
set DOSBOX=
for %%D in ("%DOSBOX_X%" "%~dp0DOSBox-X\mingw-build\mingw\dosbox-x.exe" "%~dp0DOSBox-X\mingw\dosbox-x.exe") do if not defined DOSBOX if exist "%%~D" set "DOSBOX=%%~D"
rem settings for this run only, over HALODOS.CFG's
set GAME=
rem (the PC games search the DOS machine at 10.0.2.2, DOSBox-X's name for
rem this PC: broadcasts do not cross its network)
set GAME=%GAME% -network.broadcast 10.0.2.2
set FORWARDS=
if /i "%~1"=="host" (
	set FORWARDS=5150
	set GAME=-network.address 127.0.0.1
)

if not defined DOSBOX (
	echo DOSBox-X's MinGW build ^(it has the slirp network^) was not found: unpack
	echo dosbox-x-mingw64-...-portable.zip from DOSBox-X's releases into a folder
	echo named DOSBox-X here, or set DOSBOX_X to the path of its dosbox-x.exe.
	pause
	exit /b 1
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

set HERE=%~dp0
set HERE=%HERE:~0,-1%
start "" "%DOSBOX%" -nopromptfolder -defaultconf -fastlaunch ^
	-set "dosbox quit warning=false" -set "dosbox memsize=64" -set "dosbox machine=svga_s3" ^
	-set "cpu cputype=pentium" -set "cpu core=dynamic_x86" -set "cpu use dynamic core with fpu=true" ^
	-set "cpu cycles=max" -set "cpu rdtsc rate=1000000" ^
	-set "dos lfn=true" -set "sdl autolock=true" ^
	-set "sdl output=opengl" -set "sdl windowresolution=1280x960" -set "render aspect=true" ^
	-set "ne2000 ne2000=true" -set "ne2000 backend=slirp" -set "ne2000 nicbase=300" -set "ne2000 nicirq=3" ^
	-set "ethernet, slirp tcp_port_forwards=%FORWARDS%" -set "ethernet, slirp udp_port_forwards=%FORWARDS%" ^
	-c "mount c \"%HERE%\"" -c "c:" -c "Z:\SYSTEM\NE2000 0x60 3 0x300" -c "PLAY.BAT %GAME%"
