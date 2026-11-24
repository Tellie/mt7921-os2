/*
 * wrnd_ioctl.h - IOCtl contract between wpa_supplicant-os2 and a wireless
 *                OS/2 MAC driver (GenMac-compatible "WRND32$" interface).
 *
 * Derived by reading psmedley/wpa_supplicant-os2:
 *   src/drivers/packet32.c       (transport: DosOpen + DosDevIOCtl)
 *   src/drivers/driver_genmac.c  (which OIDs are used, and how)
 *   src/l2_packet/l2_packet_os2.c (EAPOL send/receive)
 *
 * Goal: a new MT7921.OS2 driver that implements this contract can be used
 * by the existing wpa_supplicant (and probably XWLAN) without changes.
 *
 * Items marked [?] are inferred from the caller side only and must be
 * verified against a real GenMac (e.g. with an IOCtl trace).
 */
#ifndef WRND_IOCTL_H
#define WRND_IOCTL_H

/* ------------------------------------------------------------------ */
/* 1. Device names                                                     */
/* ------------------------------------------------------------------ */
/* wpa_supplicant probes these with DosOpen(READWRITE, DENYNONE),
 * in this order. The driver must register one of these names
 * (or add its own name to ANames[] in packet32.c).                    */
#define WRND_DEVNAME_1   "WRND32$"
/* ... "WRND322$" through "WRND328$" for adapters 2..8                 */

/* ------------------------------------------------------------------ */
/* 2. IOCtl category/function                                          */
/* ------------------------------------------------------------------ */
#define WRND_CATEGORY        0x99
#define WRND_FUNC_OID_GET    0x40
#define WRND_FUNC_OID_SET    0x41

/* Parameter packet: always 10 ULONGs (40 bytes), in/out.
 *
 *   IN : Parm[0] = OID
 *        Parm[1] = buffer length the caller has (capped at 10000)
 *   OUT: Parm[1] = NDIS status, 0 = success.
 *                  wpa_supplicant treats ANY non-zero value as failure.
 *        Parm[2] = bytes written (GET) / bytes read (SET).
 *                  For GET this becomes the returned length; if it is
 *                  larger than the caller's buffer, the caller fails.
 *        Parm[3] = bytes needed [?] (only logged by the caller)
 *
 * Data packet: the OID payload.
 *
 * !! PITFALL: for several OIDs, wpa_supplicant passes DataLen = 4 in
 *    DosDevIOCtl while the real buffer is larger (Parm[1] = 2000 for
 *    802_1X_RX, the frame length for 802_1X_TX). So in ring 0, use
 *    Parm[1] as the real buffer size, NOT the IOCtl data length, and
 *    verify access (DevHlp_VerifyAccess / lock) for Parm[1] bytes.
 *
 * DosDevIOCtl rc must be 0 for success; a non-zero rc is also a failure.
 */
#define WRND_PARM_COUNT      10

typedef struct _WRND_PARM {
    unsigned long Oid;          /* in                                 */
    unsigned long LenOrStatus;  /* in: buffer len / out: NDIS status  */
    unsigned long BytesDone;    /* out                                */
    unsigned long BytesNeeded;  /* out [?]                            */
    unsigned long Reserved[6];
} WRND_PARM;

/* ------------------------------------------------------------------ */
/* 3. Private wrapper OIDs (GenMac specific)                           */
/* ------------------------------------------------------------------ */
#define OID_PRIVATE_WRAPPER_LANNUMBER 0xFFFFFF00UL /* GET u32: LAN index n -> "lan<n>"   */
#define OID_PRIVATE_WRAPPER_ISWIRELESS 0xFFFFFF01UL /* GET u32: must be != 0              */
#define OID_PRIVATE_WRAPPER_GENMAC_VERSION 0xFFFFFF09UL /* GET u32: must be >= 0x00010008     */
#define OID_PRIVATE_WRAPPER_802_1X_STATE 0xFFFFFF0BUL /* SET u32: 1 = start EAPOL capture   */
#define OID_PRIVATE_WRAPPER_802_1X_READY 0xFFFFFF0CUL /* GET u32: # EAPOL frames queued     */
#define OID_PRIVATE_WRAPPER_802_1X_RX 0xFFFFFF0DUL /* GET: one frame (<= 2000 bytes),
                                                         Ethernet header included,
                                                         length in Parm[2]                */
#define OID_PRIVATE_WRAPPER_802_1X_TX 0xFFFFFF0EUL /* SET: one Ethernet frame (EAPOL),
                                                         length in Parm[1]                */

#define WRND_MIN_VERSION   0x00010008UL  /* report e.g. 0x00010008 or higher */

/* EAPOL (ethertype 0x888E) frames do NOT go through NDIS2/TCP-IP:
 * the driver must queue received 0x888E frames for 802_1X_RX
 * (reception is polled once per second) and send 802_1X_TX frames
 * unencrypted before the keys are installed.                          */

/* ------------------------------------------------------------------ */
/* 4. Standard NDIS 5.1 OIDs that wpa_supplicant uses                  */
/* ------------------------------------------------------------------ */
/* The driver must behave like a Windows XP NDIS 5.1 WLAN miniport:
 * it does scanning, authentication and association ITSELF ("MLME in
 * the driver"); wpa_supplicant only does WPA/RSN handshake + keys.    */

#define OID_GEN_CURRENT_PACKET_FILTER      0x0001010EUL /* SET u32                         */
#define OID_802_3_CURRENT_ADDRESS          0x01010102UL /* GET 6 bytes MAC                  */
#define OID_802_3_MULTICAST_LIST           0x01010103UL /* SET (wired mode only)            */

#define OID_802_11_BSSID                   0x0D010101UL /* GET: fails/zero = disconnected!
                                                         Polled every 1 s; this is how
                                                         ASSOC/DISASSOC is detected.
                                                         SET: lock to BSSID (ff..ff = any) */
#define OID_802_11_SSID                    0x0D010102UL /* GET/SET NDIS_802_11_SSID;
                                                         SET = start association          */
#define OID_802_11_INFRASTRUCTURE_MODE     0x0D010108UL /* SET u32 (1 = infrastructure).
                                                         MUST succeed, otherwise the
                                                         adapter is treated as wired      */
#define OID_802_11_ADD_WEP                 0x0D010113UL
#define OID_802_11_REMOVE_WEP              0x0D010114UL
#define OID_802_11_DISASSOCIATE            0x0D010115UL /* SET                              */
#define OID_802_11_BSSID_LIST              0x0D010217UL /* GET NDIS_802_11_BSSID_LIST_EX
                                                         incl. IEs (RSN/WPA IE needed!)   */
#define OID_802_11_AUTHENTICATION_MODE     0x0D010118UL /* GET/SET u32 (Open, WPA2PSK, ...) */
#define OID_802_11_PRIVACY_FILTER          0x0D010119UL /* SET u32                          */
#define OID_802_11_BSSID_LIST_SCAN         0x0D01011AUL /* SET: start scan; results are read
                                                         ~7 s later via BSSID_LIST        */
#define OID_802_11_ENCRYPTION_STATUS       0x0D01011BUL /* GET/SET u32 (Encryption3 = CCMP) */
#define OID_802_11_ADD_KEY                 0x0D01011DUL /* SET NDIS_802_11_KEY  -> MCU key  */
#define OID_802_11_REMOVE_KEY              0x0D01011EUL
#define OID_802_11_ASSOCIATION_INFORMATION 0x0D01011FUL /* GET: (Re)Assoc Req/Resp IEs      */
#define OID_802_11_CAPABILITY              0x0D010122UL /* GET, Version must be 2; lists
                                                         auth/encryption pairs (WPA2-PSK/
                                                         AES). Optional but recommended   */
#define OID_802_11_PMKID                   0x0D010123UL /* SET (PMKSA cache), may be no-op  */

/* NOTE: the NDIS_802_11_KEY struct in driver_genmac.c contains an extra
 * ULONG 'Reserved' before KeyRSC for gcc alignment on OS/2. The driver
 * must use exactly that layout.                                       */

/* Do NOT implement OID_DOT11_CURRENT_OPERATION_MODE (0x0D010308):
 * if it succeeds, wpa_supplicant switches to "native 802.11" mode,
 * which the OS/2 port does not fully support.                         */

/* ------------------------------------------------------------------ */
/* 5. NDIS status codes returned in Parm[1]                            */
/* ------------------------------------------------------------------ */
#define NDIS_STATUS_SUCCESS            0x00000000UL
#define NDIS_STATUS_NOT_SUPPORTED      0xC00000BBUL
#define NDIS_STATUS_INVALID_LENGTH     0xC0010014UL
#define NDIS_STATUS_BUFFER_TOO_SHORT   0xC0010016UL

#define WRND_MAX_DATA                  10000UL  /* caller caps Parm[1] */

/* ------------------------------------------------------------------ */
/* 6. MT7921.OS2 private debug OID (not used by wpa_supplicant)        */
/* ------------------------------------------------------------------ */
#define OID_PRIVATE_MT7921_INFO        0xFFFFFF80UL
#define MT7921_INFO_MAGIC              0x31323937UL     /* "7921" */

#pragma pack(1)
typedef struct _MT7921_INFO {      /* 32 bytes, same in 16- and 32-bit */
    unsigned long  Magic;
    unsigned short DrvVersion;
    unsigned short LanNumber;
    unsigned char  Bus;
    unsigned char  DevFunc;
    unsigned char  Irq;
    unsigned char  Pad;
    unsigned short VendorID;
    unsigned short DeviceID;
    unsigned short SubVendorID;
    unsigned short SubDeviceID;
    unsigned short Command;
    unsigned short Reserved;
    unsigned long  Bar0Lo;
    unsigned long  Bar0Hi;
} MT7921_INFO;
/* Phase 2: GET runs the fw-own / driver-own handshake and reads the
 * chip ID, exactly like mt7921_pci_probe() in Linux mt76.            */
#define OID_PRIVATE_MT7921_HWPROBE     0xFFFFFF81UL
#define MT7921_PROBE_MAGIC             0x424F5250UL     /* "PROB" */

/* MT7921_HWPROBE.Result */
#define PROBE_OK                0
#define PROBE_NO_SELECTOR       1   /* GDT selector not allocated at INIT */
#define PROBE_MAP_FAILED        2   /* PhysToGDTSelector failed           */
#define PROBE_DEAD_BUS          3   /* register reads 0xFFFFFFFF           */
#define PROBE_FW_OWN_TIMEOUT    4
#define PROBE_DRV_OWN_TIMEOUT   5
#define PROBE_BAD_CHIPID        6   /* handshake ok, chip ID != 7961/7920 */
/* v0.03: after a fw-own timeout the probe continues (driver own +
 * chip ID); Result keeps the FIRST problem seen.                     */

typedef struct _MT7921_HWPROBE {   /* 68 bytes */
    unsigned long  Magic;
    unsigned short Result;          /* PROBE_xxx                      */
    unsigned short PciCommand;      /* as seen by the driver          */
    unsigned long  LpctlBefore;     /* CONN_ON_LPCTL (BAR0+0xE0010)   */
    unsigned long  LpctlFwOwn;      /* after SET_OWN                  */
    unsigned long  LpctlDrvOwn;     /* after CLR_OWN                  */
    unsigned long  FwOwnPolls;
    unsigned long  DrvOwnPolls;
    unsigned long  ConnOnMisc;      /* 0x7C0600F0 (BAR0+0xE00F0)      */
    unsigned long  RemapL1;         /* HIF_REMAP_L1 (BAR0+0xFE24C)    */
    unsigned long  ChipId;          /* 0x70010200 via L1 remap        */
    unsigned long  HwRev;           /* 0x70010204                     */
    unsigned long  HwBound;         /* 0x70010020 (bit 7 => MT7920)   */
    unsigned long  Trace[4];        /* first distinct LPCTL values seen
                                       while polling after SET_OWN    */
    unsigned long  WinPhys;         /* physical base of the last BAR0
                                       window mapped (sanity check)   */
} MT7921_HWPROBE;

/* Phase 3a: GET runs WFSYS reset, DMA init and the ROM patch download
 * (mt76_connac2_load_patch). Steps/errors below.                     */
#define OID_PRIVATE_MT7921_FWBOOT      0xFFFFFF82UL
#define MT7921_FWBOOT_MAGIC            0x544F4F42UL     /* "BOOT" */

#define FWB_STEP_START          0
#define FWB_STEP_OWN            1   /* fw own / driver own handshake  */
#define FWB_STEP_CHIPID         2
#define FWB_STEP_WFSYS          3   /* WFSYS software reset           */
#define FWB_STEP_BUSMASTER      4
#define FWB_STEP_DMA            5   /* WFDMA disable / rings / enable */
#define FWB_STEP_MCU_OWN        6   /* mt7921e_driver_own (LPCR_HOST) */
#define FWB_STEP_SEM_GET        7   /* PATCH_SEM_CONTROL get          */
#define FWB_STEP_DOWNLOAD       8   /* init download + FW_SCATTER     */
#define FWB_STEP_FINISH         9   /* PATCH_FINISH_REQ               */
#define FWB_STEP_SEM_REL        10  /* PATCH_SEM_CONTROL release      */
#define FWB_STEP_RAM_DL         11  /* RAM code regions (v0.11)       */
#define FWB_STEP_FW_START       12  /* FW_START_REQ                   */
#define FWB_STEP_N9_READY       13  /* wait CONN_ON_MISC N9 ready     */
#define FWB_STEP_DONE           14

#define FWB_OK                  0
#define FWB_ERR_NO_FW           1   /* patch file not loaded at boot  */
#define FWB_ERR_NO_DMA          2   /* no DMA memory                  */
#define FWB_ERR_TIMEOUT         3   /* register poll timed out        */
#define FWB_ERR_CHIPID          4
#define FWB_ERR_TX_TIMEOUT      5   /* DMA did not consume a TX desc  */
#define FWB_ERR_NO_EVENT        6   /* no MCU response event          */
#define FWB_ERR_BAD_STATUS      7   /* MCU answered with an error     */
#define FWB_ERR_BAD_PATCH       8   /* patch header invalid           */
#define FWB_ERR_NO_RAM          9   /* RAM code file not loaded       */
#define FWB_ERR_BAD_RAM         10  /* RAM code trailer invalid       */

/* FwState values (set at boot) */
#define FWS_LOADED              0
#define FWS_NOT_FOUND           1
#define FWS_NO_MEMORY           2
#define FWS_READ_ERROR          3

typedef struct _MT7921_FWBOOT {     /* 304 bytes */
    unsigned long  Magic;
    unsigned short Step;            /* last step reached (FWB_STEP_x) */
    unsigned short Error;           /* FWB_ERR_x                      */
    unsigned short FwState;         /* FWS_x (boot-time file load)    */
    unsigned short RegionsDone;
    unsigned long  FwSize;
    unsigned long  FwPhys;
    unsigned long  DmaPhys;
    unsigned long  HwSwVer;         /* patch header (big endian -> host) */
    unsigned long  PatchVer;
    unsigned long  NRegion;
    char           BuildDate[16];
    unsigned long  ChipId;
    unsigned long  PciCmd;          /* config dword 0x04 after BM on  */
    unsigned long  GloCfg;          /* WFDMA0_GLO_CFG after enable    */
    unsigned long  Polls;           /* polls of the step that ended   */
    unsigned short SemGet;          /* status byte, 0xFFFF = none     */
    unsigned short SemRel;
    unsigned short Finish;
    unsigned short LastCmd;
    unsigned long  ChunksSent;
    unsigned long  LastEvtLen;
    unsigned char  LastEvt[24];     /* event bytes 24..47 (after RXD) */
    /* v0.06 diagnostics: state of the ring that timed out           */
    unsigned long  RingBase;        /* desc_base register             */
    unsigned long  RingCnt;         /* ring_size                      */
    unsigned long  RingCidx;        /* cpu_idx                        */
    unsigned long  RingDidx;        /* dma_idx                        */
    unsigned long  DescCtrl;        /* ctrl word of the descriptor    */
    unsigned long  IntSta;          /* WFDMA0_HOST_INT_STA            */
    unsigned long  PciStat;         /* config 0x04 after the timeout  */
    /* Intel VT-d (from the ACPI DMAR table)                         */
    unsigned short DmarFound;       /* 0 = no DMAR, else # of DRHD    */
    unsigned short Flags;           /* FWX_ flags that were used      */
    unsigned long  DmarPhys;
    unsigned long  DrhdBase[2];     /* register base of DRHD 0 and 1  */
    unsigned long  DrhdGsts[2];     /* global status (bit 31 = TES)   */
    /* v0.07: register dump taken at the TX timeout, see DUMP_xxx    */
    unsigned long  Dump[16];
    /* v0.09: PCIe error status around the first TX kick. Each value is
     * config dword cap+8 (DevCtl | DevSta << 16) or cap+0x10 (LnkCtl |
     * LnkSta << 16); error bits were cleared just before the kick.   */
    unsigned char  RpBus, RpDevFn;  /* root port above the card        */
    unsigned char  DevCap, RpCap;   /* PCIe capability offsets         */
    unsigned long  DevCtlSta;       /* card:      after the kick       */
    unsigned long  DevLnk;
    unsigned long  RpCtlSta;        /* root port: after the kick       */
    unsigned long  RpSecSta;        /* root port config 0x1C (sec. status hi) */
    unsigned long  DevCtlStaBefore; /* card: before clearing           */
    unsigned long  RpCtlStaBefore;
    /* v0.11: RAM code (WIFI_RAM_CODE_MT7961_1.bin)                  */
    unsigned short RamState;        /* FWS_x                          */
    unsigned short RamRegionsDone;
    unsigned long  RamSize;
    unsigned long  RamPhys;
    unsigned long  RamChunks;
    unsigned short FwStart;         /* FW_START_REQ status, FFFF none */
    unsigned short EvtRing;         /* RX ring of the last event      */
    unsigned long  ConnMisc;        /* CONN_ON_MISC at the end        */
    unsigned long  N9Polls;
    char           FwVer[10];       /* from the trailer               */
    char           FwDate[14];
} MT7921_FWBOOT;

/* v0.08: experiment flags, passed IN the Magic field of the buffer   */
#define FWX_KEEP_DMASHDL_BYPASS 0x0001  /* set DMASHDL bypass after RST  */
#define FWX_CLR_DMASHDL_LITE    0x0002  /* clear GLO_CFG_EXT0 bit 7      */
#define FWX_CLR_LBK_RX_Q_SEL    0x0004  /* clear GLO_CFG bit 20          */
#define FWX_DO_WFSYS_RESET      0x0008  /* v0.10: reset is OFF by default;
                                           with it WFDMA never starts  */
#define FWX_NO_WFDMA_RST        0x0010  /* skip the WFDMA0_RST pulse     */
#define FWX_BM_LATE             0x0020  /* v0.07 order: BM after reset   */

/* order of MT7921_FWBOOT.Dump[] */
#define DUMP_LPCTL          0   /* 0x7C060010 CONN_ON_LPCTL          */
#define DUMP_CONN_MISC      1   /* 0x7C0600F0 CONN_ON_MISC           */
#define DUMP_LPCR_HOST      2   /* 0x18060010 TOP_LPCR_HOST_BAND0    */
#define DUMP_WFSYS_RST      3   /* 0x18000140 WFSYS_SW_RST           */
#define DUMP_WFDMA_RST      4   /* WFDMA0 +0x100 RST                 */
#define DUMP_BUSY_ENA       5   /* WFDMA0 +0x13C BUSY_ENA            */
#define DUMP_MCU_CMD        6   /* WFDMA0 +0x1F0 MCU_CMD             */
#define DUMP_GLO_CFG        7   /* WFDMA0 +0x208                     */
#define DUMP_GLO_EXT0       8   /* WFDMA0 +0x2B0                     */
#define DUMP_DMASHDL        9   /* 0x7C026004 DMASHDL_SW_CONTROL     */
#define DUMP_DUMMY_CR       10  /* 0x54000120                        */
#define DUMP_SWDEF_MODE     11  /* 0x0041F23C                        */
#define DUMP_PCIE_MAC_PM    12  /* 0x74030194 (BAR 0x10194)          */
#define DUMP_TX16_DIDX      13  /* FWDL ring dma_idx                 */
#define DUMP_TX17_EXT       14  /* TX ring 17 prefetch (EXT_CTRL)    */
#define DUMP_RX0_DIDX       15  /* RX ring 0 dma_idx                 */
#pragma pack()

/* ------------------------------------------------------------------ */
/* 7. Data path                                                      */
/* ------------------------------------------------------------------ */
/* Normal IP traffic goes, as with any NIC, via NDIS 2.0.1 to
 * PROTOCOL.INI/TCP-IP: the driver presents itself to the protocol
 * stack as an Ethernet MAC (802.3 frames) and converts internally
 * between 802.3 <-> 802.11 + LLC/SNAP.                                */

#endif /* WRND_IOCTL_H */
