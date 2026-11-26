# MT7921 WiFi-stuurprogramma voor OS/2 en ArcaOS

*English: [README.md](README.md)*

Een eigen stuurprogramma voor de **MediaTek MT7921** (Filogic 330, PCI `14C3:7961`, ook verkocht als AMD
RZ608/RZ616) PCIe-wifikaart, geschreven op een HP Victus 15-fa0xxx (subsystem `1A3B:4682`) met ArcaOS 5.1.2.

Het stuurprogramma bestaat uit twee delen:

* **De OS/2-kant** (`src/os2/`, `src/wlan/`) komt (op namen en een paar reparaties na) uit Paul Smedley's
  [iwlwifi-stuurprogramma voor OS/2](https://github.com/psmedley/iwlwifi):
  * een **GenMAC-compatibel apparaat** (`\DEV\WRND32$`), zodat de ArcaOS **Wireless LAN Monitor (XWLAN)** en
    [wpa_supplicant](https://github.com/psmedley/wpa_supplicant-os2) ermee kunnen werken;
  * een **NDIS 2.0.1-MAC** (`MT7921_NIF`), zodat TCP/IP er als `lanN` aan koppelt;
  * het station: scanlijst, authenticatie, associatie, WPA2-sleutels, EAPOL, block-ack-volgorde.
* **De chip-kant** (`src/mt76/`) is het MediaTek-deel achter Pauls `wlan/hw.h`, naar het voorbeeld van Linux
  **mt76** (`mt7921/`, `mt792x_*`, `mt76_connac*`). Het is gegroeid uit het losse MT7921.OS2 v0.01-v0.20 (bewaard in
  `old/`).

Paul wilde de merken gescheiden houden. Daarom is dit een apart stuurprogramma dat zijn code als basis gebruikt.

## Stand van zaken (v0.51)

Werkt op de HP Victus, dagelijks in gebruik:

* firmware laden (koude start; na een warme herstart wordt de draaiende firmware hergebruikt, en gereset en opnieuw
  geladen als hij niet reageert);
* scannen op 2,4 en 5 GHz; WPA2-PSK (CCMP) met wpa_supplicant of XWLAN; DHCP;
* 802.11n en 802.11ac: 20/40/80 MHz, twee antenne-stromen, aggregatie bij ontvangen en zenden (block-ack).

Gemeten op 5 GHz, VHT80, twee stromen (router op een paar meter):

| | Snelheid |
|---|---|
| één download (wget), TCP-venster 246723 | ongeveer 8 MB/s (64 Mbit/s) |
| meerdere verbindingen tegelijk | ongeveer 32 MB/s (255 Mbit/s) |

Eén TCP-verbinding wordt begrensd door het maximale venster van de OS/2-stack (246723 bytes) gedeeld door de
reistijd, niet door de wifi. Zie "TCP-vensters" hieronder.

Nog niet: WPA3 (vraagt ook SAE-ondersteuning in wpa_supplicant), TKIP, WEP, energiebesparing, 160 MHz.

## Installeren

1. Kopieer `MT7921.OS2` (`bin\` in de zip) en `install\MT7921.NIF` naar `C:\IBMCOM\MACS`.
2. Kopieer de twee firmwarebestanden uit linux-firmware `mediatek/` naar `C:\IBMCOM\MACS\MT7921\`:
   `WIFI_MT7961_patch_mcu_1_2_hdr.bin` en `WIFI_RAM_CODE_MT7961_1.bin`.
3. `CONFIG.SYS`: `DEVICE=C:\IBMCOM\MACS\MT7921.OS2`. Er kan maar één `WRND32$`-stuurprogramma geladen zijn: haal
   GenMAC en andere wifi-stuurprogramma's weg.
4. `PROTOCOL.INI`: koppel `MT7921_NIF` zoals in `install\PROTOCOL.INI.example`. De volgorde van de koppelingen
   bepaalt het lan-nummer (met een bekabelde kaart eerst is wifi `lan1`). `MTMAC.CMD` zet het MAC-adres van de
   kaart erin.
5. `C:\STARTUP.CMD`: de regels uit `install\STARTUP.CMD.example`: de `copy /b` van de firmware (eerst de patch,
   dan de RAM-code, als één kopie met `+`) en de `inetcfg`-regels voor grotere TCP-vensters.
6. TCP/IP-instellingen (TCPCFG2): zet de wifi (`lan1`) op DHCP. DHCP kan maar op één interface tegelijk.
7. Herstart. `copy \DEV\WRND32$ con` toont de toestand; `STATE=0064 READY` betekent dat de kaart klaar is.
8. Verbinden, op een van deze twee manieren:
   * **Wireless LAN Monitor (XWLAN)**, aanbevolen: zet de widget in de XCenter (of start `wlanstat.exe` uit
     `\sys\apps\xwps\plugins\xcenter`). Bij *Properties - Device* staat "MediaTek MT7921 PCIe WLAN". Rechter
     muisknop - *Add/Edit Profile*: SSID, *WPA2-PSK* met het wachtwoord, TCP/IP *DHCP*. Daarna *Select Profile*.
     XWLAN start zelf wpa_supplicant en DHCP.
   * **wpa_supplicant met de hand**: `install\WIFI.CMD` start wpa_supplicant (geminimaliseerd venster) en DHCP op
     `lan1`; roep het aan vanuit de startup.cmd in de map Opstarten. Om het wachten op DHCP bij het opstarten te
     voorkomen: verander `-d NN` in `-d 0` in `\MPTN\BIN\SETUP.CMD`. Gebruik niet allebei tegelijk.

## TCP-vensters

De OS/2-stack begint met een ontvangstvenster van 32 KB: ongeveer 1,5 MB/s naar een server op 20 ms, 400 KB/s
naar een in Amerika. De `inetcfg`-regels in `STARTUP.CMD.example` zetten het op het maximum:

```
inetcfg -s winscale 1
inetcfg -s sack 1
inetcfg -s tcprwinsize 246723
inetcfg -s tcpswinsize 246723
```

`inetcfg -g tcprwinsize` toont de huidige waarde en het maximum; een waarde boven het maximum geeft een fout.

## Statustekst en commando's

`copy \DEV\WRND32$ con` (of `copy \DEV\WRND32$ c:\status.txt`) toont de toestand van het stuurprogramma. Handige
regels: `LINK` (kanaal, VHT80/HT40/HT20, stromen), `RXWIDTH`/`TXMODE` (breedtes en stromen die echt gebruikt
worden), `BA`/`TXBA` (aggregatie), `RXRING` (beste seconde in KB/s, hexadecimaal).

Instellingen die zonder herstart te veranderen zijn (ze gelden vanaf de volgende verbinding):

| Commando | Effect |
|---|---|
| `echo up > \DEV\WRND32$` | kaart opnieuw starten na een storing |
| `echo width 20` / `40` / `80` `> \DEV\WRND32$` | maximale kanaalbreedte |
| `echo streams 1` / `2` `> \DEV\WRND32$` | antenne-stromen |
| `echo rxba 0` / `1` `> \DEV\WRND32$` | aggregatie bij ontvangen uit / aan |
| `echo txba 0` / `1` `> \DEV\WRND32$` | aggregatie bij zenden uit / aan |
| `echo fwht 0` / `1` `> \DEV\WRND32$` | 802.11n/ac-snelheden in de firmware uit / aan |

Sleutelwoorden in `PROTOCOL.INI` voor hetzelfde: `MAXWIDTH`, `STREAMS`, `FWHT`, `AMPDU`, `AGGTX` (zie het
voorbeeldbestand).

## Bouwen

Linux (cross): Open Watcom 2.0 (`binl64` in het PATH, `WATCOM` gezet) en NASM, dan `make -C src`.
OS/2: `build.cmd` in `src` (Open Watcom 2.0 + NASM). Het stuurprogramma is `src\MT7921.OS2`.

## Indeling

```
src/os2/    OS/2-deel (Paul Smedley): device header, DevHelp, strategy-routine, NDIS-MAC, GenMAC-OIDs,
            DMA-geheugen, PCI, statustekst
src/wlan/   het chip-onafhankelijke station (Paul Smedley) en hw.h, het contract dat een chip-deel invult
src/mt76/   het MediaTek MT7921-deel: registers, WFDMA-ringen, MCU-berichten, firmware, scan, verbinden,
            sleutels, zenden/ontvangen, block-ack
install/    NIF, voorbeelden voor PROTOCOL.INI en STARTUP.CMD, WIFI.CMD, MTMAC.CMD
old/        het losse MT7921.OS2 v0.01-v0.20 en het testprogramma wrndtest
```

## Licentie

GPL-2.0-only (`COPYING`), vanwege de OS/2-kant uit het iwlwifi-stuurprogramma. De mt76-code waar het chip-deel op
gebaseerd is valt onder ISC en is daarmee verenigbaar. De firmware is van MediaTek en wordt apart verspreid onder
een eigen licentie. Zie `PROVENANCE.md`.
