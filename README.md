# MT7921 WiFi driver for OS/2 and ArcaOS

A native driver for the **MediaTek MT7921** (Filogic 330, PCI `14C3:7961`, also sold as AMD RZ608/RZ616) PCIe
WiFi adapter, written on an HP Victus 15-fa0xxx (subsystem `1A3B:4682`).

Since v0.30 the driver has two parts:

* **The OS/2 side** (`src/os2/`, `src/wlan/`) is taken unchanged (apart from names) from Paul Smedley's
  [iwlwifi driver for OS/2](https://github.com/psmedley/iwlwifi):
  * a **GenMAC-compatible device** (`\DEV\WRND32$`, DosDevIOCtl category 99h OIDs), so the ArcaOS Wireless LAN
    Monitor and [wpa_supplicant](https://github.com/psmedley/wpa_supplicant-os2) can drive it;
  * an **NDIS 2.0.1 MAC** (`MT7921_NIF`), so TCP/IP binds to it as `lanN`;
  * the station: scan table, authentication, association, WPA2 keys, EAPOL, Ethernet <-> 802.11.
* **The chip side** (`src/mt76/`) is the MediaTek back end behind Paul's `wlan/hw.h`, following Linux
  **mt76** (`mt7921/`, `mt792x_*`, `mt76_connac*`). It grew out of the stand-alone MT7921.OS2 v0.01-v0.20, which is
  kept in `old/` for reference.

Paul asked to keep the brands apart, so this is a separate driver that uses his code as its base, not a back end
inside his repository.

## Status (v0.37)

Working on the HP Victus (ArcaOS 5.1.2) since v0.36: firmware load (cold and warm-reboot reattach), scan on 2.4
and 5 GHz, WPA2-PSK (CCMP) via wpa_supplicant, DHCP, ping to the router and to the internet, interrupt-driven.

Known issues / not done:
* A second access point of the same network (mesh extender, 50:e0:39:...) delivers data frames that the station
  drops as "not from our AP" (v0.37 shows them as DROP0= in the status text). Workaround: `bssid=` of the main
  router in wpa.conf.
* Legacy rates only (max 54 Mbit/s, 20 MHz): no HT/VHT transmit rates, no aggregation, no 40/80 MHz, no power
  save, no WPA3/TKIP/WEP.

## Install (for testing)

1. Copy `MT7921.OS2` (from `src/` after building, or `bin/` in the release zip) and `install\MT7921.NIF` to
   `C:\IBMCOM\MACS`.
2. Copy the two firmware files from linux-firmware `mediatek/` to `C:\IBMCOM\MACS\MT7921\`:
   `WIFI_MT7961_patch_mcu_1_2_hdr.bin` and `WIFI_RAM_CODE_MT7961_1.bin`.
3. `CONFIG.SYS`: `DEVICE=C:\IBMCOM\MACS\MT7921.OS2` (only one `WRND32$` driver can be loaded: remove the line of
   the old stand-alone MT7921.OS2, GenMAC or any other WRND32$ driver).
4. `PROTOCOL.INI`: bind `MT7921_NIF` as in `install\PROTOCOL.INI.example`.
5. `STARTUP.CMD`: the `copy /b` line from `install\STARTUP.CMD.example` (patch first, then the RAM code, as one
   copy with `+`).
6. Reboot. `copy \DEV\WRND32$ con` prints the driver state (`STATE=0064 READY`, `HWMAC=`...). `MTMAC.CMD` writes
   the adapter's MAC into PROTOCOL.INI.
7. Connect with wpa_supplicant (`wpa_supplicant -i lan1 -c wpa.conf`) or the Wireless LAN Monitor.
8. DHCP: set lan1 to DHCP in TCPCFG2 (DHCP can be on one interface only), then `dhcpstrt -i lan1 -d 30` once
   wpa_supplicant is connected. `install\WIFI.CMD` does both (wpa_supplicant in a minimised window, then DHCP);
   call it from the startup.cmd in the Startup folder. To avoid the boot-time DHCP wait, change `-d NN` to `-d 0`
   in `\MPTN\BIN\SETUP.CMD`.

## Building

Linux (cross): Open Watcom 2.0 (`binl64` on the PATH, `WATCOM` set) and NASM, then `make -C src`.
OS/2: `build.cmd` in `src` (Open Watcom 2.0 + NASM). The driver is `src\MT7921.OS2`.

## Layout

```
src/os2/    OS/2 plumbing (Paul Smedley): device header, DevHelp glue, strategy routine, NDIS MAC, GenMAC OIDs,
            DMA memory, PCI, status text
src/wlan/   the chip-independent station (Paul Smedley) and hw.h, the contract a chip back end implements
src/mt76/   the MediaTek MT7921 back end: registers, WFDMA rings, MCU messages, firmware, scan, join, keys, TX/RX
install/    NIF, PROTOCOL.INI and STARTUP.CMD examples, MTMAC.CMD
old/        the stand-alone MT7921.OS2 v0.01-v0.20 and its test tool wrndtest
```

## Licence

GPL-2.0-only (`COPYING`), because of the OS/2 side from the iwlwifi driver. The mt76 code the back end follows is
ISC licensed and compatible with that. The firmware is MediaTek's, distributed separately under its own licence.
See `PROVENANCE.md`.
