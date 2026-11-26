# makefile for OpenWatcom wmake - MT7921.OS2 driver
#
#   wmake            build mt7921.os2
#   wmake clean
#
# Needs the OpenWatcom environment (WATCOM, PATH, INCLUDE) set,
# e.g. via %WATCOM%\owsetenv.cmd.

CC      = wcc
AS      = wasm
LD      = wlink

# -ms small model, -3 386 code, -s no stack checks, -zl no default lib,
# -zu SS != DGROUP (essential in a PDD), -zfp/-zgp no FS/GS use,
# -zdp DS pegged to DGROUP
CFLAGS  = -bt=os2 -ms -3 -s -zl -zu -zfp -zgp -zdp -oirs -wx -zq
AFLAGS  = -bt=os2 -zq

LIBPATH = $(%WATCOM)\lib286\os2;$(%WATCOM)\lib286

all: mt7921.os2 .SYMBOLIC

mt7921.os2: header.obj mt7921.obj mt7921.lnk
    set LIB=$(LIBPATH)
    $(LD) @mt7921.lnk

header.obj: header.asm
    $(AS) $(AFLAGS) header.asm

mt7921.obj: mt7921.c wrnd_ioctl.h
    $(CC) $(CFLAGS) mt7921.c

clean: .SYMBOLIC
    @if exist *.obj del *.obj
    @if exist mt7921.os2 del mt7921.os2
    @if exist mt7921.map del mt7921.map
