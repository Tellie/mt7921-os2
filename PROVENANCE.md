# Provenance

| Part | Source | Licence |
|---|---|---|
| `src/os2/*`, `src/wlan/*` (device header, DevHelp glue, strategy routine, NDIS MAC, GenMAC OIDs, DMA memory, PCI, status text, the station, `hw.h`) | Paul Smedley's [iwlwifi driver for OS/2](https://github.com/psmedley/iwlwifi), commit `e3c2af830befa679bc354e59f29a6d0fe6a8e223`, which took the OS/2 side from his QCA6174 driver and the OS2World RTL88EU driver. Changed here: driver, NIF and adapter names only. | GPL-2.0-only |
| `install/*` | the same repository's `install/` files, renamed for the MT7921 | GPL-2.0-only |
| `src/mt76/*` (register offsets, WFDMA ring set-up, MCU message formats, patch/RAM download, NIC capability, scan, STA/BSS records, ROC, TXD/TXP layout, RX descriptor parsing, keys) | written for this driver, following Linux `drivers/net/wireless/mediatek/mt76` (`mt7921/`, `mt792x_*`, `mt76_connac*`); first as the stand-alone MT7921.OS2 v0.01-v0.20 (`old/`) | GPL-2.0-only (mt76 itself is ISC) |
| firmware (not in this repository) | linux-firmware `mediatek/WIFI_MT7961_patch_mcu_1_2_hdr.bin`, `mediatek/WIFI_RAM_CODE_MT7961_1.bin` | MediaTek, `LICENCE.mediatek` |

The combined work is distributed under GPL-2.0-only (`COPYING`).
