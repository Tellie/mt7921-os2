@echo off
rem SPDX-License-Identifier: GPL-2.0-only
rem Build MT7921.OS2 on OS/2 / ArcaOS with Open Watcom 2.0 (binp on PATH, WATCOM set) + NASM.
rem usage: build    (run in mt7921-os2\src; objects go to obj\, the driver to MT7921.OS2)
rem The compiler flags are written out on each wcc line: CMD.EXE does not expand %VAR% the same way
rem inside a FOR command, and wcc then sees the flags as extra file names (E1139).
if "%WATCOM%"=="" set WATCOM=u:\WATCOM
set PATH=%WATCOM%\binp;C:\usr\bin;%PATH%
set beginlibpath=%WATCOM%\binp\dll
set INCLUDE=
if not exist obj mkdir obj
for %%f in (hdr lib ndisx end) do nasm -f obj os2\%%f.asm -o obj\%%f.obj
for %%f in (strat pci mem ndis genmac dbg) do wcc -bt=os2 -2 -obilmr -s -ze -zl -zq -zff -zgf -ms -wx -zp1 -zu -j -i=os2 -i=wlan -i=mt76 -fo=obj\%%f.obj os2\%%f.c
for %%f in (sta) do wcc -bt=os2 -2 -obilmr -s -ze -zl -zq -zff -zgf -ms -wx -zp1 -zu -j -i=os2 -i=wlan -i=mt76 -fo=obj\%%f.obj wlan\%%f.c
for %%f in (mt_dma mt_fw mt_mac) do wcc -bt=os2 -2 -obilmr -s -ze -zl -zq -zff -zgf -ms -wx -zp1 -zu -j -i=os2 -i=wlan -i=mt76 -fo=obj\%%f.obj mt76\%%f.c
wlink @mt7921.lnk
