# MT7921 WiFi driver for OS/2 and ArcaOS

*Nederlands: [README.nl.md](README.nl.md)*

A native driver for the **MediaTek MT7921** (Filogic 330, PCI `14C3:7961`, also sold as AMD RZ608/RZ616) PCIe
WiFi adapter, written on an HP Victus 15-fa0xxx (subsystem `1A3B:4682`) with ArcaOS 5.1.2.

The driver has two parts:

* **The OS/2 side** (`src/os2/`, `src/wlan/`) is taken (apart from names and a few fixes) from Paul Smedley's
  [iwlwifi driver for OS/2](https://github.com/psmedley/iwlwifi):
  * a **GenMAC-compatible device** (`\DEV\WRND32$`), so the ArcaOS **Wireless LAN Monitor (XWLAN)** and
    [wpa_supplicant](https://github.com/psmedley/wpa_supplicant-os2) can drive it;
  * an **NDIS 2.0.1 MAC** (`MT7921_NIF`), so TCP/IP binds to it as `lanN`;
  * the station: scan table, authentication, association, WPA2 keys, EAPOL, block-ack reordering.
* **The chip side** (`src/mt76/`) is the MediaTek back end behind Paul's `wlan/hw.h`, following Linux **mt76**
  (`mt7921/`, `mt792x_*`, `mt76_connac*`). It grew out of the stand-alone MT7921.OS2 v0.01-v0.20 (kept in `old/`).

Paul asked to keep the brands apart, so this is a separate driver that uses his code as its base.

## Status (v0.51)

Works on the HP Victus, daily use:

* firmware load (cold boot; after a warm reboot the running firmware is reused, and reset and loaded again if it
  does not answer);
* scan on 2.4 and 5 GHz; WPA2-PSK (CCMP) with wpa_supplicant or XWLAN; DHCP;
* 802.11n and 802.11ac: 20/40/80 MHz, two spatial streams, receive and transmit aggregation (block-ack).

Measured on 5 GHz, VHT80, two streams (router a few metres away):

| | Speed |
|---|---|
| one download (wget), TCP window 246723 | about 8 MB/s (64 Mbit/s) |
| several connections at once | about 32 MB/s (255 Mbit/s) |

One TCP connection is limited by the OS/2 stack's maximum window (246723 bytes) divided by the round-trip time,
not by the WiFi. See "TCP windows" below.

Not done: WPA3 (needs SAE support in wpa_supplicant as well), TKIP, WEP, power save, 160 MHz.

## Install

1. Copy `MT7921.OS2` (`bin\` in the release zip) and `install\MT7921.NIF` to `C:\IBMCOM\MACS`.
2. Copy the two firmware files from linux-firmware `mediatek/` to `C:\IBMCOM\MACS\MT7921\`:
   `WIFI_MT7961_patch_mcu_1_2_hdr.bin` and `WIFI_RAM_CODE_MT7961_1.bin`.
3. `CONFIG.SYS`: `DEVICE=C:\IBMCOM\MACS\MT7921.OS2`. Only one `WRND32$` driver can be loaded: remove GenMAC or
   any other WiFi driver line.
4. `PROTOCOL.INI`: bind `MT7921_NIF` as in `install\PROTOCOL.INI.example`. The order of the bindings decides the
   lan number (with a wired card first, WiFi is `lan1`). `MTMAC.CMD` writes the adapter's MAC address in.
5. `C:\STARTUP.CMD`: the lines from `install\STARTUP.CMD.example`: the `copy /b` of the firmware (patch first,
   then the RAM code, as one copy with `+`) and the `inetcfg` lines for bigger TCP windows.
6. TCP/IP configuration (TCPCFG2): set the WiFi interface (`lan1`) to DHCP. DHCP can be on one interface only.
7. Reboot. `copy \DEV\WRND32$ con` shows the driver state; `STATE=0064 READY` means the adapter is up.
8. Connect, either way:
   * **Wireless LAN Monitor (XWLAN)**, recommended: add the widget to the XCenter (or start `wlanstat.exe` from
     `\sys\apps\xwps\plugins\xcenter`). Under *Properties - Device* it shows "MediaTek MT7921 PCIe WLAN". Right
     mouse button - *Add/Edit Profile*: SSID, *WPA2-PSK* with the passphrase, TCP/IP *DHCP*. Then *Select Profile*.
     XWLAN starts wpa_supplicant and DHCP itself.
   * **wpa_supplicant by hand**: `install\WIFI.CMD` starts wpa_supplicant (minimised window) and DHCP on `lan1`;
     call it from the startup.cmd in the Startup folder. To avoid the boot-time DHCP wait, change `-d NN` to `-d 0`
     in `\MPTN\BIN\SETUP.CMD`. Do not use both ways at the same time.

## TCP windows

The OS/2 stack starts with a 32 KB receive window: about 1.5 MB/s to a server 20 ms away, 400 KB/s to one in
America. The `inetcfg` lines in `STARTUP.CMD.example` raise it to the maximum:

```
inetcfg -s winscale 1
inetcfg -s sack 1
inetcfg -s tcprwinsize 246723
inetcfg -s tcpswinsize 246723
```

`inetcfg -g tcprwinsize` shows the current value and the maximum; a value above the maximum fails.

## Status text and commands

`copy \DEV\WRND32$ con` (or `copy \DEV\WRND32$ c:\status.txt`) prints the driver state. Useful lines: `LINK`
(channel, VHT80/HT40/HT20, streams), `RXWIDTH`/`TXMODE` (widths and streams actually used), `BA`/`TXBA`
(aggregation), `RXRING` (best second in KB/s, hex).

Settings that can be changed without a reboot (they take effect at the next connection):

| Command | Effect |
|---|---|
| `echo up > \DEV\WRND32$` | start the adapter again after a failure |
| `echo width 20` / `40` / `80` `> \DEV\WRND32$` | maximum channel width |
| `echo streams 1` / `2` `> \DEV\WRND32$` | spatial streams |
| `echo rxba 0` / `1` `> \DEV\WRND32$` | receive aggregation off / on |
| `echo txba 0` / `1` `> \DEV\WRND32$` | transmit aggregation off / on |
| `echo fwht 0` / `1` `> \DEV\WRND32$` | 802.11n/ac rates in the firmware off / on |

`PROTOCOL.INI` keywords for the same: `MAXWIDTH`, `STREAMS`, `FWHT`, `AMPDU`, `AGGTX` (see the example file).

## Building

Linux (cross): Open Watcom 2.0 (`binl64` on the PATH, `WATCOM` set) and NASM, then `make -C src`.
OS/2: `build.cmd` in `src` (Open Watcom 2.0 + NASM). The driver is `src\MT7921.OS2`.

## Layout

```
src/os2/    OS/2 plumbing (Paul Smedley): device header, DevHelp glue, strategy routine, NDIS MAC, GenMAC OIDs,
            DMA memory, PCI, status text
src/wlan/   the chip-independent station (Paul Smedley) and hw.h, the contract a chip back end implements
src/mt76/   the MediaTek MT7921 back end: registers, WFDMA rings, MCU messages, firmware, scan, join, keys, TX/RX,
            block-ack
install/    NIF, PROTOCOL.INI and STARTUP.CMD examples, WIFI.CMD, MTMAC.CMD
old/        the stand-alone MT7921.OS2 v0.01-v0.20 and its test tool wrndtest
```

## Licence

GPL-2.0-only (`COPYING`), because of the OS/2 side from the iwlwifi driver. The mt76 code the back end follows is
ISC licensed and compatible with that. The firmware is MediaTek's, distributed separately under its own licence.
See `PROVENANCE.md`.
