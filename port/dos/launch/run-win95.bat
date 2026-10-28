@echo off
rem Runs HALO95.EXE, the Windows 95 build, from this folder. The maps go in
rem MAPS here, the settings are in HALO.CFG and the last run's log is HALO.LOG.
rem The first start on a map unpacks it into CACHE\MAPS. Alt+Enter switches
rem between a window and the whole screen.
setlocal
cd /d "%~dp0"

if not exist HALO95.EXE (
	echo HALO95.EXE is not in %~dp0: build it first ^(ninja -f build/dos/build.ninja win95^).
	pause
	exit /b 1
)
rem the maps: MAPS here; else the folder that has them, asked for once and
rem linked here as MAPS (a junction: nothing is copied)
if exist "MAPS\*.map" goto maps_found
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
if exist MAPS rmdir MAPS 2>nul
mklink /J MAPS "%MAPS_FROM%" >nul
if not exist "MAPS\*.map" (
	echo MAPS could not be linked to %MAPS_FROM%: copy the maps into %~dp0MAPS instead.
	pause
	exit /b 1
)
:maps_found

start "" HALO95.EXE %*
