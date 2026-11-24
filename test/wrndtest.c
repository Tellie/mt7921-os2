/*
 * wrndtest.c - talk to WRND32$ the same way wpa_supplicant-os2 does
 *
 * Build (gcc, 32-bit OS/2):  gcc -O2 -Zomf -I../src -o wrndtest.exe wrndtest.c
 * Usage:                     wrndtest [/hw] [/fw] [devicename]   (default WRND32$)
 *   /hw  run the phase 2 hardware probe (fw/driver own + chip ID)
 *   /fw  run the phase 3a firmware boot (WFDMA + ROM patch download)
 *   /x:N experiment flags for /fw (hex, see FWX_ in wrnd_ioctl.h)
 *   /scan  phase 4a: first scan (run /fw first, same session)
 */
#define INCL_DOS
#define INCL_DOSDEVIOCTL
#include <os2.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "wrnd_ioctl.h"

static HFILE hDev;

/* exactly the same call layout as PacketRequest() in packet32.c */
static int oid_req(int set, ULONG oid, void *buf, ULONG len,
                   ULONG *status, ULONG *done)
{
    ULONG parm[WRND_PARM_COUNT];
    ULONG plen = sizeof(parm), dlen = len;
    APIRET rc;

    memset(parm, 0, sizeof(parm));
    parm[0] = oid;
    parm[1] = len;
    rc = DosDevIOCtl(hDev, WRND_CATEGORY,
                     set ? WRND_FUNC_OID_SET : WRND_FUNC_OID_GET,
                     parm, plen, &plen, buf, dlen, &dlen);
    *status = parm[1];
    *done   = parm[2];
    return (int)rc;
}

static const char *probe_result(unsigned r)
{
    switch (r) {
    case PROBE_OK:              return "OK";
    case PROBE_NO_SELECTOR:     return "no GDT selector (see boot messages)";
    case PROBE_MAP_FAILED:      return "PhysToGDTSelector failed";
    case PROBE_DEAD_BUS:        return "registers read FFFFFFFF (memory decoding off?)";
    case PROBE_FW_OWN_TIMEOUT:  return "firmware-own handshake timed out";
    case PROBE_DRV_OWN_TIMEOUT: return "driver-own handshake timed out";
    case PROBE_BAD_CHIPID:      return "unexpected chip ID";
    }
    return "unknown";
}

static void hw_probe(void)
{
    MT7921_HWPROBE p;
    ULONG st, done;
    int rc;

    memset(&p, 0, sizeof(p));
    rc = oid_req(0, OID_PRIVATE_MT7921_HWPROBE, &p, sizeof(p), &st, &done);
    printf("\nHardware probe: rc=%d status=%08lX len=%lu\n", rc, st, done);
    if (rc || st || p.Magic != MT7921_PROBE_MAGIC) {
        printf("  driver does not support the probe (old MT7921.OS2?)\n");
        return;
    }
    printf("  result          %u = %s\n", p.Result, probe_result(p.Result));
    printf("  PCI command     %04X\n", p.PciCommand);
    printf("  last window     %08lX  (must be 806xxxxx = BAR0 + offset)\n", p.WinPhys);
    printf("  LPCTL before    %08lX\n", p.LpctlBefore);
    printf("  CONN_ON_MISC    %08lX\n", p.ConnOnMisc);
    printf("  HIF_REMAP_L1    %08lX\n", p.RemapL1);
    printf("  fw own          %08lX after %lu polls\n", p.LpctlFwOwn, p.FwOwnPolls);
    printf("  driver own      %08lX after %lu polls\n", p.LpctlDrvOwn, p.DrvOwnPolls);
    printf("  LPCTL trace     %08lX %08lX %08lX %08lX\n",
           p.Trace[0], p.Trace[1], p.Trace[2], p.Trace[3]);
    if (p.Result <= PROBE_BAD_CHIPID && p.Result != PROBE_NO_SELECTOR &&
        p.Result != PROBE_MAP_FAILED && p.Result != PROBE_DEAD_BUS) {
        unsigned chip = (unsigned)(p.ChipId & 0xFFFF);
        if (chip == 0x7961 && (p.HwBound & 0x80))
            chip = 0x7920;
        printf("  chip ID         %08lX\n", p.ChipId);
        printf("  HW rev          %08lX\n", p.HwRev);
        printf("  HW bound        %08lX\n", p.HwBound);
        printf("  => ASIC revision %04X%04lX (Linux prints this as \"ASIC revision\")\n",
               chip, p.HwRev & 0xFF);
    }
}

static const char *fwb_step(unsigned s)
{
    static const char *n[] = {
        "start", "fw/driver own", "chip ID", "WFSYS reset", "bus master",
        "WFDMA init", "MCU driver own", "patch semaphore get",
        "patch download", "patch finish", "patch semaphore release",
        "RAM code download", "firmware start", "wait for N9 ready", "done"
    };
    return s < sizeof(n) / sizeof(n[0]) ? n[s] : "?";
}

static const char *fwb_err(unsigned e)
{
    static const char *n[] = {
        "OK", "patch file not loaded at boot", "no DMA memory",
        "register poll timed out", "unexpected chip ID",
        "DMA did not take the TX descriptor", "no MCU response event",
        "MCU returned an error status", "patch header invalid",
        "RAM code file not loaded at boot", "RAM code trailer invalid"
    };
    return e < sizeof(n) / sizeof(n[0]) ? n[e] : "?";
}

static const char *fws(unsigned f)
{
    static const char *n[] = { "loaded", "not found", "no memory", "read error" };
    return f < 4 ? n[f] : "?";
}

static void st_print(const char *name, unsigned v)
{
    if (v == 0xFFFF)
        printf("  %-16s -\n", name);
    else if (v == 0xFFFE)
        printf("  %-16s not sent (TX timeout)\n", name);
    else
        printf("  %-16s %u\n", name, v);
}

static unsigned fwx_flags;

static void fw_boot(void)
{
    MT7921_FWBOOT b;
    ULONG st, done;
    int rc, i;

    memset(&b, 0, sizeof(b));
    b.Magic = fwx_flags;                    /* experiment flags go in */
    printf("\nFirmware boot, flags %04X (the system may pause for a few seconds) ...\n",
           fwx_flags);
    rc = oid_req(0, OID_PRIVATE_MT7921_FWBOOT, &b, sizeof(b), &st, &done);
    printf("Firmware boot: rc=%d status=%08lX len=%lu\n", rc, st, done);
    if (rc || st || b.Magic != MT7921_FWBOOT_MAGIC) {
        printf("  driver does not support /fw (old MT7921.OS2?)\n");
        return;
    }
    printf("  result          %s at step %u (%s)\n",
           fwb_err(b.Error), b.Step, fwb_step(b.Step));
    {
        static const char *br[] = { "-, firmware started earlier in this session",
            "PCIe bus reset done", "PCIe bus reset FAILED - card did not come back",
            "no root port found", "cold start",
            "firmware from previous session reused (host DMA re-set up)" };
        printf("  start mode      %s", b.BusReset < 6 ? br[b.BusReset] : "?");
        if (b.BusReset == 1 || b.BusReset == 2)
            printf(", card answered after %lu config reads", b.ResetPolls);
        printf("\n");
    }
    printf("  patch file      %s, %lu bytes at %08lX\n",
           fws(b.FwState), b.FwSize, b.FwPhys);
    printf("  patch header    build %.16s  hw/sw %08lX  ver %08lX  regions %lu\n",
           b.BuildDate, b.HwSwVer, b.PatchVer, b.NRegion);
    printf("  DMA block       %08lX\n", b.DmaPhys);
    printf("  chip ID         %08lX\n", b.ChipId);
    printf("  PCI cmd/status  %08lX\n", b.PciCmd);
    printf("  WFDMA GLO_CFG   %08lX\n", b.GloCfg);
    printf("  polls (last)    %lu\n", b.Polls);
    st_print("sem get", b.SemGet);
    printf("  regions done    %u, chunks sent %lu\n", b.RegionsDone, b.ChunksSent);
    st_print("patch finish", b.Finish);
    st_print("sem release", b.SemRel);
    printf("  RAM code        %s, %lu bytes at %08lX\n",
           fws(b.RamState), b.RamSize, b.RamPhys);
    printf("  RAM firmware    version %.10s  build %.14s\n", b.FwVer, b.FwDate);
    printf("  RAM regions     %u, chunks sent %lu\n", b.RamRegionsDone, b.RamChunks);
    st_print("fw start", b.FwStart);
    printf("  CONN_ON_MISC    %08lX after %lu polls (bits 0+1 = N9 ready)\n",
           b.ConnMisc, b.N9Polls);
    printf("  last event ring %u\n", b.EvtRing);
    if (b.NicStatus == 0)
        printf("  NIC capability  %u TLVs (mask %08lX), MAC %02X:%02X:%02X:%02X:%02X:%02X\n",
               b.NicElems, b.CapMask, b.Mac[0], b.Mac[1], b.Mac[2],
               b.Mac[3], b.Mac[4], b.Mac[5]);
    else
        st_print("NIC capability", b.NicStatus);
    printf("  last cmd        0x%02X, last event %lu bytes:", b.LastCmd, b.LastEvtLen);
    for (i = 0; i < 24; i++)
        printf("%s%02X", i % 8 ? " " : "\n    ", b.LastEvt[i]);
    printf("\n");

    if (b.Error == 5) {         /* TX timeout: what did the ring look like */
        printf("  ring regs       base %08lX  cnt %lu  cpu_idx %lu  dma_idx %lu\n",
               b.RingBase, b.RingCnt, b.RingCidx, b.RingDidx);
        printf("  desc ctrl       %08lX  (bit31 = DMA done, len in bits 16..29)\n",
               b.DescCtrl);
        printf("  HOST_INT_STA    %08lX\n", b.IntSta);
        printf("  PCI cmd/status  %08lX  (status bit 29 = received master abort)\n",
               b.PciStat);
        {
            static const char *dn[16] = {
                "CONN_ON_LPCTL", "CONN_ON_MISC", "LPCR_HOST", "WFSYS_RST",
                "WFDMA0_RST", "BUSY_ENA", "MCU_CMD", "GLO_CFG",
                "GLO_CFG_EXT0", "DMASHDL_SW", "DUMMY_CR", "SWDEF_MODE",
                "PCIE_MAC_PM", "TX16 dma_idx", "TX17 EXT_CTRL", "RX0 dma_idx"
            };
            printf("  register dump:\n");
            for (i = 0; i < 16; i += 2)
                printf("    %-14s %08lX    %-14s %08lX\n",
                       dn[i], b.Dump[i], dn[i + 1], b.Dump[i + 1]);
        }
    }
    if (b.DevCap) {
        unsigned ds = (unsigned)(b.DevCtlSta >> 16);
        printf("  PCIe card       cap %02X  DevCtl %04lX  DevSta before %04lX after %04X  LnkSta %04lX\n",
               b.DevCap, b.DevCtlSta & 0xFFFF, b.DevCtlStaBefore >> 16, ds,
               b.DevLnk >> 16);
        printf("                  MPS %u  MRRS %u  -> after kick:%s%s%s%s\n",
               128u << ((b.DevCtlSta >> 5) & 7), 128u << ((b.DevCtlSta >> 12) & 7),
               ds & 1 ? " CorrErr" : "", ds & 2 ? " NonFatal" : "",
               ds & 4 ? " Fatal" : "", ds & 8 ? " UnsupportedRequest" : "");
    }
    if (b.RpDevFn != 0xFF && b.RpCap) {
        unsigned rs = (unsigned)(b.RpCtlSta >> 16);
        unsigned ss = (unsigned)(b.RpSecSta >> 16);
        printf("  root port       00:%02X.%u  DevCtl %04lX  DevSta before %04lX after %04X  SecSta %04X\n",
               b.RpDevFn >> 3, b.RpDevFn & 7, b.RpCtlSta & 0xFFFF,
               b.RpCtlStaBefore >> 16, rs, ss);
        printf("                  MPS %u -> after kick:%s%s%s%s%s%s\n",
               128u << ((b.RpCtlSta >> 5) & 7),
               rs & 1 ? " CorrErr" : "", rs & 2 ? " NonFatal" : "",
               rs & 4 ? " Fatal" : "", rs & 8 ? " UnsupportedRequest" : "",
               ss & 0x2000 ? " RecvMasterAbort" : "", ss & 0x4000 ? " SystemError" : "");
    }
    if (b.DmarFound) {
        for (i = 0; i < 2 && i < (int)b.DmarFound; i++)
            printf("  VT-d unit %d     base %08lX  GSTS %08lX  -> translation %s\n",
                   i, b.DrhdBase[i], b.DrhdGsts[i],
                   b.DrhdGsts[i] == 0xFFFFFFFFUL ? "?" :
                   (b.DrhdGsts[i] & 0x80000000UL) ? "ON (DMA is remapped!)" : "off");
        printf("  DMAR table      %08lX, %u DRHD unit(s)\n", b.DmarPhys, b.DmarFound);
    } else {
        printf("  VT-d            no DMAR table found\n");
    }

    if (b.Error == 0 && b.Step == 14) {
        if (b.SemGet == 0xFFFF)
            printf("\n  => WiFi firmware was already running%s\n",
                   b.NicStatus == 0 ? " and answers commands" :
                   " but did NOT answer the NIC capability command");
        else
            printf("\n  => WiFi firmware is RUNNING: patch %s, RAM code started, N9 ready\n",
                   b.SemGet == 1 ? "was already loaded" : "downloaded");
    }
}

static void scan_print_st(const char *name, unsigned v)
{
    if (v == 0xFFFF)      printf("  %-16s - (no answer)\n", name);
    else if (v == 0xFFFE) printf("  %-16s not sent (TX timeout)\n", name);
    else                  printf("  %-16s answered (%u)\n", name, v);
}

static void do_scan(void)
{
    static MT7921_SCAN s;
    static const char *stp[] = { "start", "EFUSE_BUFFER_MODE", "MAC_INIT_CTRL",
        "SET_CHAN_DOMAIN", "SET_RX_PATH",
        "DEV_INFO_UPDATE", "BSS_INFO_UPDATE", "START_HW_SCAN",
        "collect beacons", "done" };
    static const char *err[] = { "OK", "firmware not running - run /fw first",
        "command not taken by DMA", "no answer from the firmware",
        "firmware returned an error", "no SCAN_DONE event (timeout)" };
    ULONG st, done;
    int rc, i, k;

    memset(&s, 0, sizeof(s));
    printf("\nScan (the system pauses for up to ~10 seconds) ...\n");
    rc = oid_req(0, OID_PRIVATE_MT7921_SCAN, &s, sizeof(s), &st, &done);
    printf("Scan: rc=%d status=%08lX len=%lu\n", rc, st, done);
    if (rc || st || s.Magic != MT7921_SCAN_MAGIC) {
        printf("  driver does not support /scan (old MT7921.OS2?)\n");
        return;
    }
    printf("  result          %s at step %u (%s)\n",
           s.Error < 6 ? err[s.Error] : "?", s.Step, s.Step < 10 ? stp[s.Step] : "?");
    scan_print_st("EFUSE mode", s.StEfuse);
    scan_print_st("MAC enable", s.StMacEn);
    printf("  CHAN_DOMAIN     %s\n", s.StChanDom == 0 ? "sent (NL, 13 + 19 channels)" :
           s.StChanDom == 0xFFFE ? "not sent (TX timeout)" : "-");
    scan_print_st("RX path", s.StRxPath);
    printf("  DEV_INFO        UNI status %08lX (0 = OK)\n", s.DevUni);
    printf("  BSS_INFO        UNI status %08lX (0 = OK)\n", s.BssUni);
    printf("  HW_SCAN         %s\n", s.StScan == 0 ? "sent" :
           s.StScan == 0xFFFE ? "not sent (TX timeout)" : "-");
    printf("  RX frames       %lu on ring 2, %lu on the MCU rings, beacons/probe responses %lu\n",
           s.Frames, s.Events, s.Beacons);
    printf("  SCAN_DONE       %s after %lu polls\n", s.ScanDone ? "yes" : "NO", s.Polls);
    if (s.ScanDone)
        printf("                  seq %u, channels scanned %u, state %u, beacons %lu, country %.3s\n",
               s.DoneEvt[0], s.DoneEvt[4], s.DoneEvt[5],
               (ULONG)s.DoneEvt[8] | ((ULONG)s.DoneEvt[9] << 8) |
               ((ULONG)s.DoneEvt[10] << 16) | ((ULONG)s.DoneEvt[11] << 24),
               (char *)&s.DoneEvt[20]);
    if (s.Events) {
        printf("  other event ids");
        for (i = 0; i < 8 && i < (int)s.Events; i++)
            printf(" %02X", s.EvtIds[i]);
        printf("\n");
    }
    if (s.FirstLen) {
        printf("  first RX frame  %u bytes:", s.FirstLen);
        for (i = 0; i < 64 && i < s.FirstLen; i++)
            printf("%s%02X", i % 16 ? " " : "\n    ", s.FirstFrame[i]);
        printf("\n");
    }
    printf("\n  %u network(s) found:\n", s.NBss);
    for (i = 0; i < s.NBss; i++) {
        MT7921_BSS *b = &s.Bss[i];
        char ssid[33];
        for (k = 0; k < b->SsidLen && k < 32; k++)
            ssid[k] = (b->Ssid[k] >= 32 && b->Ssid[k] < 127) ? b->Ssid[k] : '.';
        ssid[k] = 0;
        printf("    %02X:%02X:%02X:%02X:%02X:%02X  ch %3u  %s\n",
               b->Bssid[0], b->Bssid[1], b->Bssid[2], b->Bssid[3],
               b->Bssid[4], b->Bssid[5], b->Channel,
               b->SsidLen ? ssid : "(hidden)");
    }
}

static void show_u32(const char *name, ULONG oid)
{
    ULONG v = 0, st, done;
    int rc = oid_req(0, oid, &v, 4, &st, &done);
    printf("  %-28s rc=%d status=%08lX len=%lu  value=0x%08lX (%lu)\n",
           name, rc, st, done, v, v);
}

int main(int argc, char **argv)
{
    const char *dev = "WRND32$";
    int do_hw = 0, do_fw = 0, do_sc = 0, i;
    ULONG action, st, done;
    APIRET rc;
    MT7921_INFO info;
    unsigned char mac[6];

    for (i = 1; i < argc; i++) {
        if (!stricmp(argv[i], "/hw") || !stricmp(argv[i], "-hw"))
            do_hw = 1;
        else if (!stricmp(argv[i], "/fw") || !stricmp(argv[i], "-fw"))
            do_fw = 1;
        else if (!stricmp(argv[i], "/scan") || !stricmp(argv[i], "-scan"))
            do_sc = 1;
        else if (!strnicmp(argv[i], "/x:", 3))
            fwx_flags = (unsigned)strtoul(argv[i] + 3, NULL, 16);
        else
            dev = argv[i];
    }

    rc = DosOpen((PSZ)dev, &hDev, &action, 0, 0, OPEN_ACTION_OPEN_IF_EXISTS,
                 OPEN_FLAGS_NOINHERIT | OPEN_ACCESS_READWRITE |
                 OPEN_SHARE_DENYNONE, NULL);
    if (rc) {
        printf("DosOpen(%s) failed, rc=%lu - is MT7921.OS2 loaded?\n", dev, rc);
        return 1;
    }
    printf("%s opened\n\nwpa_supplicant probe OIDs:\n", dev);

    show_u32("ISWIRELESS (must be !=0)",   OID_PRIVATE_WRAPPER_ISWIRELESS);
    show_u32("GENMAC_VERSION (>=1.08)",    OID_PRIVATE_WRAPPER_GENMAC_VERSION);
    show_u32("LANNUMBER",                  OID_PRIVATE_WRAPPER_LANNUMBER);
    show_u32("802_1X_READY",               OID_PRIVATE_WRAPPER_802_1X_READY);
    show_u32("AUTHENTICATION_MODE",        OID_802_11_AUTHENTICATION_MODE);
    show_u32("ENCRYPTION_STATUS",          OID_802_11_ENCRYPTION_STATUS);
    show_u32("BSSID (fail = disconnected)", OID_802_11_BSSID);

    memset(mac, 0, sizeof(mac));
    rc = oid_req(0, OID_802_3_CURRENT_ADDRESS, mac, 6, &st, &done);
    printf("  %-28s rc=%lu status=%08lX  %02X:%02X:%02X:%02X:%02X:%02X\n",
           "CURRENT_ADDRESS", rc, st,
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    memset(&info, 0, sizeof(info));
    rc = oid_req(0, OID_PRIVATE_MT7921_INFO, &info, sizeof(info), &st, &done);
    printf("\nMT7921 info: rc=%lu status=%08lX len=%lu\n", rc, st, done);
    if (!rc && !st && info.Magic == MT7921_INFO_MAGIC) {
        printf("  driver version  %u.%02u\n", info.DrvVersion >> 8,
               info.DrvVersion & 0xFF);
        printf("  PCI             %02X:%02X.%u  %04X:%04X  subsys %04X:%04X\n",
               info.Bus, info.DevFunc >> 3, info.DevFunc & 7,
               info.VendorID, info.DeviceID,
               info.SubVendorID, info.SubDeviceID);
        printf("  BAR0            %08lX%08lX\n", info.Bar0Hi,
               info.Bar0Lo & 0xFFFFFFF0UL);
        printf("  IRQ             %u\n", info.Irq);
        printf("  PCI command     %04X%s\n", info.Command,
               (info.Command & 6) == 6 ? "" :
               "  (memory/bus master not enabled yet - expected in phase 1)");
    }

    if (do_hw)
        hw_probe();
    if (do_fw)
        fw_boot();
    if (do_sc)
        do_scan();

    DosClose(hDev);
    return 0;
}
