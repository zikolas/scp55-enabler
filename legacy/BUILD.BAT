@echo off
rem Build GSGO.EXE with Open Watcom 1.9 (16-bit, real mode).
rem Expects the Watcom environment to be set, e.g.:
rem   set WATCOM=C:\WATCOM
rem   set INCLUDE=C:\WATCOM\H
rem   set PATH=C:\WATCOM\BINW;%PATH%
wcc -ms GSGO.C -fo=GSGO.obj
if errorlevel 1 goto end
wlink system dos name GSGO.exe file GSGO.obj
:end
