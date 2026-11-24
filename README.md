# MT7921.OS2 – MediaTek MT7921 wifi driver for OS/2 (ArcaOS)

Target hardware: MediaTek MT7921 (PCI 14C3:7961), AzureWave module
(1A3B:4682), as fitted in the HP Victus 15-fa0xxx.

The driver presents itself as a GenMac-compatible `WRND32$` device, so
the existing [wpa_supplicant-os2](https://github.com/psmedley/wpa_supplicant-os2)
can drive it without changes. See `src/wrnd_ioctl.h` for the interface.

## Roadmap

| Phase | Goal | Status |
|---|---|---|
| 1 | PCI detection, `WRND32$`, wpa_supplicant probe OIDs | done (v0.01) |
| 2 | Enable PCI memory, map BAR0, fw/driver-own handshake, read chip ID | done (v0.04) |
| 3a | ROM patch from disk, bus master, WFDMA rings, MCU messages, ROM patch download | done (v0.10) |
| 3b | RAM code download, FW_START, N9 ready | done (v0.11) |
| 3c | Warm reboot: reuse running firmware, NIC capability, real MAC address | done (v0.13) |
| 4a | EFUSE mode, MAC enable, channel domain, RX path, station interface, HW scan, beacon list | **this version (v0.15)** |
| 4b | Scan results as OID_802_11_BSSID_LIST for wpa_supplicant | |
| 4 | MCU commands: read MAC from EEPROM, hw_scan → `BSSID_LIST` | |
| 5 | Mini-MLME: auth/assoc, EAPOL via 802_1X_RX/TX, keys via ADD_KEY | |
| 6 | NDIS 2.0.1 MAC: PROTMAN binding, 802.3 ⇄ 802.11 data path for TCP/IP | |

## Build

Requires OpenWatcom (1.9 or 2.0) for the driver, gcc for the test tool.

```
%WATCOM%\owsetenv.cmd
sh build.sh
```

Output goes to `out\`: `mt7921.os2`, `mt7921.map`, `wrndtest.exe`.

## Install (phase 1, safe: no hardware access)

Add to CONFIG.SYS (after the existing network drivers):

```
DEVICE=C:\IBMCOM\MACS\MT7921.OS2 /LAN:1
```

Options: `/LAN:n` – LAN number reported to wpa_supplicant (default 1),
`/Q` – no boot messages, `/NOMEM` – do not enable PCI memory decoding.

At boot you should see something like:

```
MT7921.OS2 v0.01 - MediaTek MT7921 wireless (phase 1)
MT7921.OS2: found at PCI 04:00.0  subsys 1A3B:4682
MT7921.OS2: BAR0=0000000080600000 IRQ=12 CMD=0000 as lan1
```

## Firmware (phase 3a)

Get `WIFI_MT7961_patch_mcu_1_2_hdr.bin` and `WIFI_RAM_CODE_MT7961_1.bin` from linux-firmware
(https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/tree/mediatek)
and put it in a `MT7921` folder next to the driver:

```
C:\IBMCOM\MACS\MT7921.OS2
C:\IBMCOM\MACS\MT7921\WIFI_MT7961_patch_mcu_1_2_hdr.bin
```

The driver reads it at boot and reports `... loaded (0x...... bytes)`.

## Test

Phase 2 hardware probe (runs only on request, never at boot):

```
wrndtest /hw
```

Expected: `result 0 = OK`, chip ID ending in `7961`.

Phase 3a ROM patch download (the system pauses for a few seconds):

```
wrndtest /fw
```

After a warm reboot the WiFi firmware is still running in the chip; since
v0.13 `/fw` then reuses it and only sets up the host DMA again. First scan (after `/fw`, same boot):

```
wrndtest /scan
```

Expected: `OK at step 9 (done)` and a list of nearby networks.

Expected for `/fw`: `OK at step 14 (done)`, sem get 2, patch finish 0, sem release 3
(sem get 1 = the patch was already loaded). The WFSYS reset is off by
default since v0.10 (`/x:8` turns it on; with it the WFDMA never starts).


```
wrndtest
wpa_supplicant -i lan1 -c wpa.conf -dd
```

`wrndtest` must report ISWIRELESS=1, version 0x00010008 and the PCI data.
wpa_supplicant should find `lan1`, initialise and start scanning
(with 0 results – there is no radio yet).
