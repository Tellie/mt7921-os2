# SPDX-License-Identifier: GPL-2.0-only
# Cross build on Linux with Open Watcom 2.0 (binl64 on PATH, WATCOM set) and NASM:  make
# On OS/2 use build.cmd (same flags). Objects go to obj/, the driver to MT7921.OS2.
#
#   os2/   OS/2 plumbing: device header, DevHelp glue, strategy routine, NDIS MAC, GenMAC OIDs, DMA memory, PCI
#   wlan/  the chip-independent station (scan table, join, keys, EAPOL, Ethernet <-> 802.11) and hw.h
#   mt76/  the MediaTek MT7921 back end (Linux mt76: WFDMA rings, MCU messages, firmware, scan, join, keys)
CFLAGS = -bt=os2 -2 -obilmr -s -ze -zl -zq -zff -zgf -ms -wx -zp1 -zu -j -i=os2 -i=wlan -i=mt76
ASM    = hdr lib ndisx end
OS2C   = strat pci mem ndis genmac dbg
WLANC  = sta
MTC    = mt_dma mt_fw mt_mac
HDRS   = os2/drv.h os2/os2dd.h os2/ndis.h wlan/hw.h wlan/wlan.h mt76/mt.h

OBJS   = $(addprefix obj/,$(addsuffix .obj,$(ASM) $(OS2C) $(WLANC) $(MTC)))

MT7921.OS2: obj $(OBJS) mt7921.lnk
	wlink @mt7921.lnk

obj:
	mkdir -p obj

obj/%.obj: os2/%.asm
	nasm -f obj $< -o $@

obj/%.obj: os2/%.c $(HDRS)
	wcc $(CFLAGS) -fo=$@ $<

obj/%.obj: wlan/%.c $(HDRS)
	wcc $(CFLAGS) -fo=$@ $<

obj/%.obj: mt76/%.c $(HDRS)
	wcc $(CFLAGS) -fo=$@ $<

clean:
	rm -rf obj MT7921.OS2 MT7921.map

.PHONY: clean
