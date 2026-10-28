@echo off
rem PLAY.BAT: runs HALO.EXE in DOS, and again whenever it ends asking to
rem (the menu's APPLY AND RESTART, for the settings it reads only as it
rem starts: it ends with exit code 42). Its options go to HALO.EXE as
rem they are (up to nine).
:again
HALO.EXE %1 %2 %3 %4 %5 %6 %7 %8 %9
if errorlevel 43 goto end
if errorlevel 42 goto again
:end
