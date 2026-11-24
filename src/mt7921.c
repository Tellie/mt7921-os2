/*
 * mt7921.c - MediaTek MT7921 (14C3:7961) wireless driver for OS/2
 *
 * Phase 1: detection + wpa_supplicant-os2 IOCtl interface.
 * Phase 2: PCI memory enable at INIT, BAR0 mapping, fw/driver-own
 *          handshake + chip ID on demand (OID_PRIVATE_MT7921_HWPROBE).
 * Phase 3a: ROM patch file read at INIT; on demand WFSYS reset, WFDMA
 *          rings, MCU messages and the ROM patch download
 *          (OID_PRIVATE_MT7921_FWBOOT), following Linux mt76.
 *   - finds the card through OEMHLP$ PCI services at INIT
 *   - registers as WRND32$ (the GenMac name wpa_supplicant-os2 probes)
 *   - answers category 0x99, function 0x40 (OID GET) / 0x41 (OID SET)
 *   - no firmware, no DMA, no interrupts, no radio yet
 *
 * 16-bit PDD, OpenWatcom (wcc, small model, SS != DGROUP).
 * See wrnd_ioctl.h for the interface contract.
 *
 * Copyright (c) 2026 Elbert Pol
 */

#include "wrnd_ioctl.h"

#pragma pack(1)        /* after the include: it ends with pack() */

typedef unsigned char  UCHAR;
typedef unsigned short USHORT;
typedef unsigned long  ULONG;
#define FAR __far

#define DRV_VERSION     0x0013          /* 0.19 */

/* ------------------------------------------------------------------ */
/* Request packets                                                     */
/* ------------------------------------------------------------------ */
typedef struct {
    UCHAR  Len;
    UCHAR  Unit;
    UCHAR  Cmd;
    USHORT Status;
    ULONG  Reserved;
    ULONG  Link;
} RPH;

typedef struct {                /* INIT, input view */
    RPH    rph;
    UCHAR  Unit;
    ULONG  DevHlpEP;
    char FAR *Args;
    UCHAR  Drive;
} RP_INIT_IN;

typedef struct {                /* INIT, output view */
    RPH    rph;
    UCHAR  Units;
    USHORT CodeEnd;
    USHORT DataEnd;
    ULONG  BPBArray;
    UCHAR  Drive;
} RP_INIT_OUT;

typedef struct {                /* GENIOCTL */
    RPH    rph;
    UCHAR  Category;
    UCHAR  Function;
    void FAR *ParmPacket;
    void FAR *DataPacket;
    USHORT Sfn;
    USHORT ParmLen;
    USHORT DataLen;
} RP_IOCTL;

#define CMD_INIT        0x00
#define CMD_OPEN        0x0D
#define CMD_CLOSE       0x0E
#define CMD_GENIOCTL    0x10
#define CMD_SHUTDOWN    0x1C

#define STDON           0x0100
#define STERR           0x8000
#define ERR_BAD_COMMAND 0x0003
#define ERR_GEN_FAILURE 0x000C
#define ERR_INVALID_PARM 0x0013

/* ------------------------------------------------------------------ */
/* Assembler glue (header.asm)                                         */
/* ------------------------------------------------------------------ */
extern ULONG Device_Help;
#pragma aux Device_Help "*";

void StrategyC(RPH FAR *rp);
#pragma aux StrategyC "*" parm [es bx];

USHORT DH_VerifyAccess(USHORT sel, USHORT off, USHORT len, USHORT type);
#pragma aux DH_VerifyAccess "*" parm [ax] [di] [cx] [dx] value [ax] \
        modify [bx cx dx si di es];

USHORT DH_AllocGDTSel(USHORT FAR *sels, USHORT count);
#pragma aux DH_AllocGDTSel "*" parm [es di] [cx] value [ax] \
        modify [bx cx dx si di es];

/* v0.04: 32-bit values are passed as two explicit 16-bit halves.
 * v0.03 used register pairs ([ax bx], [dx cx]) and Watcom filled them
 * with the words the other way round: writes arrived as hi<->lo and
 * the BAR0 window was mapped at the wrong physical address.          */
USHORT DH_PhysToGDTSel(USHORT physHi, USHORT physLo, USHORT len, USHORT sel);
#pragma aux DH_PhysToGDTSel "*" parm [ax] [bx] [cx] [si] value [ax] \
        modify [bx cx dx si di es];

extern ULONG MmioResult;
#pragma aux MmioResult "*";

void MmioRd(USHORT sel, USHORT off);
#pragma aux MmioRd "*" parm [ax] [bx];

void MmioWr(USHORT sel, USHORT off, USHORT lo, USHORT hi);
#pragma aux MmioWr "*" parm [ax] [bx] [cx] [dx];

USHORT DH_AllocPhys(USHORT sizeHi, USHORT sizeLo);
#pragma aux DH_AllocPhys "*" parm [ax] [bx] value [ax] \
        modify [bx cx dx si di es];
extern ULONG PhysResult;
#pragma aux PhysResult "*";

USHORT DH_PhysToVirt(USHORT physHi, USHORT physLo, USHORT len);
#pragma aux DH_PhysToVirt "*" parm [ax] [bx] [cx] value [ax] \
        modify [bx cx dx si di];
extern void FAR *VirtResult;
#pragma aux VirtResult "*";

void DH_Sleep(USHORT ms);
#pragma aux DH_Sleep "*" parm [cx] modify [ax bx cx dx di si es];

void PciCfgRd(USHORT addrHi, USHORT addrLo);
#pragma aux PciCfgRd "*" parm [ax] [bx];
void PciCfgWr(USHORT addrHi, USHORT addrLo, USHORT valLo, USHORT valHi);
#pragma aux PciCfgWr "*" parm [ax] [bx] [cx] [dx];

USHORT GetCodeEnd(void);
#pragma aux GetCodeEnd "*" value [ax];
USHORT GetDataEnd(void);
#pragma aux GetDataEnd "*" value [ax];

/* ------------------------------------------------------------------ */
/* 16-bit DOSCALLS (only allowed during INIT)                          */
/* ------------------------------------------------------------------ */
extern USHORT FAR __pascal DOSOPEN(char FAR *, USHORT FAR *, USHORT FAR *,
                                   ULONG, USHORT, USHORT, USHORT, ULONG);
extern USHORT FAR __pascal DOSDEVIOCTL(void FAR *pData, void FAR *pParm,
                                       USHORT func, USHORT cat, USHORT hdev);
extern USHORT FAR __pascal DOSCLOSE(USHORT);
extern USHORT FAR __pascal DOSREAD(USHORT hf, void FAR *buf, USHORT cb,
                                   USHORT FAR *pcbRead);
extern USHORT FAR __pascal DOSCHGFILEPTR(USHORT hf, long dist, USHORT method,
                                         ULONG FAR *newpos);
extern USHORT FAR __pascal DOSPUTMESSAGE(USHORT hf, USHORT cb, char FAR *msg);

/* ------------------------------------------------------------------ */
/* OEMHLP$ PCI services (category 0x80, function 0x0B)                 */
/* ------------------------------------------------------------------ */
typedef struct { UCHAR SubFunc; USHORT DeviceID; USHORT VendorID; UCHAR Index; } PCI_FIND_PARM;
typedef struct { UCHAR rc; UCHAR Bus; UCHAR DevFunc; } PCI_FIND_DATA;
typedef struct { UCHAR SubFunc; UCHAR Bus; UCHAR DevFunc; UCHAR Reg; UCHAR Size; } PCI_READ_PARM;
typedef struct { UCHAR rc; ULONG Data; } PCI_READ_DATA;
typedef struct { UCHAR SubFunc; UCHAR Bus; UCHAR DevFunc; UCHAR Reg; UCHAR Size; ULONG Data; } PCI_WRITE_PARM;
typedef struct { UCHAR rc; } PCI_WRITE_DATA;

#define OEMHLP_CAT      0x80
#define OEMHLP_PCI      0x0B
#define PCI_FIND_DEVICE 1
#define PCI_READ_CONFIG 3
#define PCI_WRITE_CONFIG 4

#define PCI_CMD_MEMORY  0x0002

#define MT_VENDOR       0x14C3
#define MT_DEVICE       0x7961

/* ------------------------------------------------------------------ */
/* Driver state. Everything whose address is taken lives here (DGROUP),
 * never on the stack: SS != DS in a PDD.                              */
/* ------------------------------------------------------------------ */
static MT7921_INFO  gInfo;
static USHORT       gLanNumber = 1;
static int          gQuiet = 0;
static int          gNoMem = 0;         /* /NOMEM: leave PCI command alone */
static USHORT       gSels[4];           /* GDT selectors: BAR0, DMA, FW, DMA2 */
#define gDma2Sel gSels[3]               /* 2nd 64 KB DMA block: RX ring 4  */
#define gSel    gSels[0]                /* BAR0 64 KB window (remapped)    */
#define gDmaSel gSels[1]                /* the 64 KB DMA block             */
#define gFwSel  gSels[2]                /* window into the firmware image  */
static ULONG        gWinBase = 0xFFFFFFFFUL; /* BAR0 offset now mapped  */
static MT7921_HWPROBE gProbe;

static ULONG gAuthMode   = 0;           /* Ndis802_11AuthModeOpen       */
static ULONG gEncStatus  = 1;           /* Ndis802_11EncryptionDisabled */
static ULONG gInfraMode  = 1;           /* Ndis802_11Infrastructure     */
static ULONG g8021xState = 0;

/* Placeholder MAC (locally administered) until the real one is read
 * from the chip EEPROM via the MCU in a later phase.                  */
static UCHAR gMac[6] = { 0x02, 0x00, 0x00, 0x79, 0x21, 0x01 };

/* INIT-time scratch buffers */
static USHORT        gHf, gAction;
static PCI_FIND_PARM gFindParm;
static PCI_FIND_DATA gFindData;
static PCI_READ_PARM gReadParm;
static PCI_READ_DATA gReadData;
static PCI_WRITE_PARM gWriteParm;
static PCI_WRITE_DATA gWriteData;
static char          gMsg[160];
static char          gOemhlp[] = "OEMHLP$";

/* ------------------------------------------------------------------ */
/* Small helpers (no C runtime in a PDD)                               */
/* ------------------------------------------------------------------ */
typedef union { void FAR *p; struct { USHORT off, sel; } s; } FARPTR;

static int VerifyFar(void FAR *p, USHORT len, USHORT type)
{
    FARPTR fp;
    fp.p = p;
    if (fp.s.sel == 0)
        return 1;
    return DH_VerifyAccess(fp.s.sel, fp.s.off, len, type) != 0;
}

static void FarCopy(UCHAR FAR *d, const UCHAR FAR *s, USHORT n)
{
    while (n--) *d++ = *s++;
}

static void FarZero(UCHAR FAR *d, USHORT n)
{
    while (n--) *d++ = 0;
}

static USHORT gMsgLen;

static void MsgStr(const char *s)
{
    while (*s && gMsgLen < sizeof(gMsg) - 1)
        gMsg[gMsgLen++] = *s++;
}

static void MsgHex(ULONG v, int digits)
{
    static const char hex[] = "0123456789ABCDEF";
    union { ULONG l; UCHAR b[4]; } u;   /* byte access: no long shifts */
    int i;
    u.l = v;
    for (i = digits - 1; i >= 0; i--) {
        UCHAR b = u.b[i >> 1];
        UCHAR n = (i & 1) ? (UCHAR)(b >> 4) : (UCHAR)(b & 0x0F);
        if (gMsgLen < sizeof(gMsg) - 1)
            gMsg[gMsgLen++] = hex[n];
    }
}

static void MsgDec(UCHAR v)
{
    char d[3];
    int n = 0;
    do { d[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n && gMsgLen < sizeof(gMsg) - 1)
        gMsg[gMsgLen++] = d[--n];
}

static void MsgFlush(void)
{
    if (!gQuiet && gMsgLen)
        DOSPUTMESSAGE(1, gMsgLen, (char FAR *)gMsg);
    gMsgLen = 0;
}

/* ------------------------------------------------------------------ */
/* INIT                                                                */
/* ------------------------------------------------------------------ */
static void ParseArgs(char FAR *a)
{
    if (!a) return;
    for (; *a; a++) {
        if (*a != '/' && *a != '-')
            continue;
        a++;
        if ((a[0] | 0x20) == 'q') {
            gQuiet = 1;
        } else if ((a[0] | 0x20) == 'n' && (a[1] | 0x20) == 'o' &&
                   (a[2] | 0x20) == 'm' && (a[3] | 0x20) == 'e' &&
                   (a[4] | 0x20) == 'm') {
            gNoMem = 1;
        } else if ((a[0] | 0x20) == 'l' && (a[1] | 0x20) == 'a' &&
                   (a[2] | 0x20) == 'n' && a[3] == ':' &&
                   a[4] >= '0' && a[4] <= '9') {
            gLanNumber = (USHORT)(a[4] - '0');
        }
        if (!*a) break;
    }
}

static int PciRead(USHORT h, UCHAR reg, UCHAR size, ULONG *val)
{
    gReadParm.SubFunc = PCI_READ_CONFIG;
    gReadParm.Bus     = gInfo.Bus;
    gReadParm.DevFunc = gInfo.DevFunc;
    gReadParm.Reg     = reg;
    gReadParm.Size    = size;
    if (DOSDEVIOCTL((void FAR *)&gReadData, (void FAR *)&gReadParm,
                    OEMHLP_PCI, OEMHLP_CAT, h) || gReadData.rc)
        return 1;
    *val = gReadData.Data;
    return 0;
}

static int PciWrite(USHORT h, UCHAR reg, UCHAR size, ULONG val)
{
    gWriteParm.SubFunc = PCI_WRITE_CONFIG;
    gWriteParm.Bus     = gInfo.Bus;
    gWriteParm.DevFunc = gInfo.DevFunc;
    gWriteParm.Reg     = reg;
    gWriteParm.Size    = size;
    gWriteParm.Data    = val;
    if (DOSDEVIOCTL((void FAR *)&gWriteData, (void FAR *)&gWriteParm,
                    OEMHLP_PCI, OEMHLP_CAT, h) || gWriteData.rc)
        return 1;
    return 0;
}

static int FindCard(void)
{
    static ULONG v;
    USHORT h;

    if (DOSOPEN((char FAR *)gOemhlp, (USHORT FAR *)&gHf,
                (USHORT FAR *)&gAction, 0L, 0, 0x0001, 0x0040, 0L))
        return 1;
    h = gHf;

    gFindParm.SubFunc  = PCI_FIND_DEVICE;
    gFindParm.DeviceID = MT_DEVICE;
    gFindParm.VendorID = MT_VENDOR;
    gFindParm.Index    = 0;
    if (DOSDEVIOCTL((void FAR *)&gFindData, (void FAR *)&gFindParm,
                    OEMHLP_PCI, OEMHLP_CAT, h) || gFindData.rc) {
        DOSCLOSE(h);
        return 2;
    }
    gInfo.Bus     = gFindData.Bus;
    gInfo.DevFunc = gFindData.DevFunc;

    if (!PciRead(h, 0x00, 4, &v)) {
        gInfo.VendorID = (USHORT)v;
        gInfo.DeviceID = ((USHORT FAR *)&v)[1];
    }
    if (!PciRead(h, 0x04, 2, &v)) gInfo.Command = (USHORT)v;
    if (!PciRead(h, 0x10, 4, &v)) gInfo.Bar0Lo  = v;
    if (!PciRead(h, 0x14, 4, &v)) gInfo.Bar0Hi  = v;
    if (!PciRead(h, 0x2C, 4, &v)) {
        gInfo.SubVendorID = (USHORT)v;
        gInfo.SubDeviceID = ((USHORT FAR *)&v)[1];
    }
    if (!PciRead(h, 0x3C, 1, &v)) gInfo.Irq = (UCHAR)v;

    /* Phase 2: enable memory decoding (not bus master: no DMA yet),
     * like mt7921_pci_probe() does.                                  */
    if (!gNoMem && !(gInfo.Command & PCI_CMD_MEMORY)) {
        if (!PciWrite(h, 0x04, 2, (ULONG)(gInfo.Command | PCI_CMD_MEMORY)) &&
            !PciRead(h, 0x04, 2, &v))
            gInfo.Command = (USHORT)v;
    }

    DOSCLOSE(h);
    return 0;
}

typedef union { ULONG l; struct { USHORT lo, hi; } w; } U32W;

static void InitDma(void);
static void LoadFirmware(char FAR *args);


/* ------------------------------------------------------------------ */
/* Phase 3a: DMA memory, firmware image, WFDMA rings, MCU messages     */
/* ------------------------------------------------------------------ */

/* One 64 KB AllocPhys block holds all rings and buffers:             */
#define OFS_TX0     0x0000      /* TX ring 0  (data)    4 desc        */
#define OFS_FWDL    0x0040      /* TX ring 16 (FWDL)   16 desc        */
#define OFS_WM      0x0140      /* TX ring 17 (MCU WM) 16 desc        */
#define OFS_RX0     0x0240      /* RX ring 0  (MCU events) 8 desc     */
#define OFS_RX2     0x0600      /* RX ring 2  (data)    8 desc        */
#define OFS_RX4     0x0300      /* RX ring 4 (events + frames) 32 desc */
#define OFS_CMD     0x1000      /* MCU command buffer (4 KB)          */
#define OFS_RXB0    0x3000      /* RX0 buffers 8 x 2 KB               */
#define OFS_RXB2    0xB000      /* RX2 buffers 8 x 2 KB (to 0xF000)   */
#define OFS_RXB4    0x0000      /* RX4 buffers 32 x 2 KB, in DMA block 2 */
#define RX_BUF      2048

#define PATCH_NAME  "WIFI_MT7961_patch_mcu_1_2_hdr.bin"
#define RAM_NAME    "WIFI_RAM_CODE_MT7961_1.bin"
#define FW_MAX_SIZE 0x00400000UL                /* sanity: 4 MB */

static ULONG  gDmaPhys;
static ULONG  gDma2Phys;
static ULONG  gFwPhys, gFwSize;
static USHORT gFwState = FWS_NOT_FOUND;
static char   gFwPath[160];
static USHORT gFwDirLen;
static ULONG  gFileSize;
static ULONG  gRamPhys, gRamSize;
static USHORT gRamState = FWS_NOT_FOUND;
static UCHAR  gBounce[4096];
static ULONG  gFwWin = 0xFFFFFFFFUL;
static USHORT gBounceRead;
static ULONG  gFilePos;
static MT7921_FWBOOT gBoot;

static void FAR *MkFar(USHORT sel, USHORT off)
{
    FARPTR fp;
    fp.s.sel = sel;
    fp.s.off = off;
    return fp.p;
}

/* far pointer to byte 'ofs' of the firmware image (64 KB windows)    */
/* gFwSel maps the 64 KB-aligned physical window gFwWin            */
static UCHAR FAR *PhysPtr(ULONG phys)
{
    U32W p;
    p.l = phys;
    if (p.w.hi != (USHORT)(gFwWin >> 16) || gFwWin == 0xFFFFFFFFUL) {
        if (DH_PhysToGDTSel(p.w.hi, 0, 0, gFwSel)) {
            gFwWin = 0xFFFFFFFFUL;
            return (UCHAR FAR *)0;
        }
        gFwWin = phys & 0xFFFF0000UL;
    }
    return (UCHAR FAR *)MkFar(gFwSel, p.w.lo);
}

static UCHAR FAR *FwPtr(ULONG ofs)
{
    return PhysPtr(gFwPhys + ofs);
}

static ULONG Be32(const UCHAR FAR *p)
{
    union { ULONG l; UCHAR b[4]; } u;
    u.b[0] = p[3]; u.b[1] = p[2]; u.b[2] = p[1]; u.b[3] = p[0];
    return u.l;
}

static void InitDma(void)
{
    U32W p;
    if (DH_AllocPhys(1, 0)) {                   /* 0x00010000 bytes */
        MsgStr("MT7921.OS2: cannot allocate DMA memory\r\n");
        MsgFlush();
        return;
    }
    p.l = PhysResult;
    if (DH_PhysToGDTSel(p.w.hi, p.w.lo, 0, gDmaSel)) {
        MsgStr("MT7921.OS2: cannot map DMA memory\r\n");
        MsgFlush();
        return;
    }
    gDmaPhys = p.l;

    /* v0.17: second 64 KB block for the 32 buffers of RX ring 4 */
    if (DH_AllocPhys(1, 0)) {
        MsgStr("MT7921.OS2: cannot allocate DMA memory (2)\r\n");
        MsgFlush();
        gDmaPhys = 0;
        return;
    }
    p.l = PhysResult;
    if (DH_PhysToGDTSel(p.w.hi, p.w.lo, 0, gDma2Sel)) {
        gDmaPhys = 0;
        return;
    }
    gDma2Phys = p.l;
    /* zeroed at task time (FwBoot): a GDT selector is ring 0 only */
}

/* INIT time: read a firmware file from <driver dir>\MT7921\ into
 * physically contiguous memory, so FW_SCATTER can DMA it directly.  */
static USHORT LoadOne(const char *name, ULONG *phys, ULONG *size)
{
    USHORT i, n, state;
    U32W sz;
    ULONG ofs;

    n = gFwDirLen;
    for (i = 0; name[i] && n < sizeof(gFwPath) - 1; i++) gFwPath[n++] = name[i];
    gFwPath[n] = 0;
    *phys = 0;
    *size = 0;

    state = FWS_NOT_FOUND;
    if (DOSOPEN((char FAR *)gFwPath, (USHORT FAR *)&gHf, (USHORT FAR *)&gAction,
                0L, 0, 0x0001, 0x0040, 0L))
        goto report;

    if (DOSCHGFILEPTR(gHf, 0L, 2, (ULONG FAR *)&gFileSize) ||
        DOSCHGFILEPTR(gHf, 0L, 0, (ULONG FAR *)&gFilePos) ||
        gFileSize < 96 || gFileSize > FW_MAX_SIZE) {
        state = FWS_READ_ERROR;
        goto close;
    }

    sz.l = (gFileSize + 0xFFFUL) & 0xFFFFF000UL;
    if (DH_AllocPhys(sz.w.hi, sz.w.lo)) {
        state = FWS_NO_MEMORY;
        goto close;
    }
    *phys = PhysResult;
    *size = gFileSize;

    for (ofs = 0; ofs < gFileSize; ofs += 4096) {
        UCHAR FAR *dst;
        U32W ph;
        if (DOSREAD(gHf, (void FAR *)gBounce, 4096,
                    (USHORT FAR *)&gBounceRead) || gBounceRead == 0) {
            state = FWS_READ_ERROR;
            goto close;
        }
        /* INIT runs at ring 3: use PhysToVirt, not the GDT selector */
        ph.l = *phys + ofs;
        if (DH_PhysToVirt(ph.w.hi, ph.w.lo, 4096)) {
            state = FWS_NO_MEMORY;
            goto close;
        }
        dst = (UCHAR FAR *)VirtResult;
        FarCopy(dst, (UCHAR FAR *)gBounce, gBounceRead);
    }
    state = FWS_LOADED;

close:
    DOSCLOSE(gHf);
report:
    MsgStr("MT7921.OS2: ");
    MsgStr(gFwPath + gFwDirLen);
    if (state == FWS_LOADED) {
        MsgStr(" loaded (0x"); MsgHex(gFileSize, 6); MsgStr(" bytes)\r\n");
    } else if (state == FWS_NOT_FOUND) {
        MsgStr(" not found in "); gFwPath[gFwDirLen] = 0; MsgStr(gFwPath);
        MsgStr("\r\n");
    } else if (state == FWS_NO_MEMORY) {
        MsgStr(" - no memory\r\n");
    } else {
        MsgStr(" - read error\r\n");
    }
    MsgFlush();
    return state;
}

static void LoadFirmware(char FAR *args)
{
    USHORT n = 0, last = 0;
    const char *sub = "MT7921\\";

    /* args = "C:\IBMCOM\MACS\MT7921.OS2 /LAN:1" -> directory part */
    if (args) {
        while (args[n] && args[n] != ' ' && n < sizeof(gFwPath) - 56) {
            gFwPath[n] = args[n];
            if (args[n] == '\\') last = n + 1;
            n++;
        }
    }
    n = last;
    while (*sub) gFwPath[n++] = *sub++;
    gFwDirLen = n;

    gFwState  = LoadOne(PATCH_NAME, &gFwPhys, &gFwSize);
    gRamState = LoadOne(RAM_NAME, &gRamPhys, &gRamSize);
}

static USHORT DoInit(RPH FAR *rph)
{
    RP_INIT_IN  FAR *in  = (RP_INIT_IN FAR *)rph;
    RP_INIT_OUT FAR *out = (RP_INIT_OUT FAR *)rph;
    int rc;

    Device_Help = in->DevHlpEP;
    ParseArgs(in->Args);

    gInfo.Magic      = MT7921_INFO_MAGIC;
    gInfo.DrvVersion = DRV_VERSION;
    gInfo.LanNumber  = gLanNumber;

    MsgStr("\r\nMT7921.OS2 v0.19 - MediaTek MT7921 wireless (phase 5a)\r\n");
    MsgFlush();

    rc = FindCard();
    if (rc) {
        MsgStr(rc == 1 ? "MT7921.OS2: cannot open OEMHLP$\r\n"
                       : "MT7921.OS2: no MT7921 (14C3:7961) found - not installed\r\n");
        MsgFlush();
        out->Units   = 0;
        out->CodeEnd = 0;
        out->DataEnd = 0;
        return STDON | STERR | ERR_GEN_FAILURE;
    }

    MsgStr("MT7921.OS2: found at PCI ");
    MsgHex(gInfo.Bus, 2);  MsgStr(":");
    MsgHex(gInfo.DevFunc >> 3, 2); MsgStr(".");
    MsgHex(gInfo.DevFunc & 7, 1);
    MsgStr("  subsys ");
    MsgHex(gInfo.SubVendorID, 4); MsgStr(":"); MsgHex(gInfo.SubDeviceID, 4);
    MsgStr("\r\nMT7921.OS2: BAR0=");
    MsgHex(gInfo.Bar0Hi, 8); MsgHex(gInfo.Bar0Lo & 0xFFFFFFF0UL, 8);
    MsgStr(" IRQ=");  MsgDec(gInfo.Irq);
    MsgStr(" CMD=");  MsgHex(gInfo.Command, 4);
    MsgStr(" as lan"); MsgHex(gLanNumber, 1);
    MsgStr("\r\n");

    if (DH_AllocGDTSel((USHORT FAR *)gSels, 4)) {
        gSels[0] = gSels[1] = gSels[2] = gSels[3] = 0;
        MsgStr("MT7921.OS2: no GDT selectors - hardware access disabled\r\n");
    }
    MsgFlush();

    if (gSel) {
        InitDma();
        LoadFirmware(in->Args);
    }

    out->Units   = 0;
    out->CodeEnd = GetCodeEnd();
    out->DataEnd = GetDataEnd();
    out->BPBArray = 0;
    return STDON;
}

/* ------------------------------------------------------------------ */
/* BAR0 register access (task time)                                    */
/* BAR0 is 1 MB; one GDT selector maps a 64 KB window, remapped on     */
/* demand. Offsets below are BAR0 offsets after the mt76 fixed_map.    */
/* ------------------------------------------------------------------ */
#define REG_CONN_ON_LPCTL   0x000E0010UL    /* 0x7C060010 */
#define REG_CONN_ON_MISC    0x000E00F0UL    /* 0x7C0600F0 */
#define REG_HIF_REMAP_L1    0x000FE24CUL    /* MT_INFRA(0x24c)          */
#define REG_REMAP_BASE_L1   0x00040000UL    /* L1 window in BAR0        */

#define LPCR_SET_OWN        0x00000001UL    /* give the chip to the FW  */
#define LPCR_CLR_OWN        0x00000002UL    /* take it for the driver   */
#define LPCR_OWN_SYNC       0x00000004UL

#define HW_CHIPID           0x70010200UL
#define HW_REV              0x70010204UL
#define HW_BOUND            0x70010020UL

#define POLL_LIMIT          100000UL        /* ~50-100 ms of PCIe reads */
#define OWN_RETRIES         10              /* MT792x_DRV_OWN_RETRY_COUNT */

static int MapWindow(ULONG ofs)
{
    U32W base, phys;
    base.l = ofs;
    base.w.lo = 0;                          /* 64 KB aligned window */
    if (base.l == gWinBase)
        return 0;
    phys.l = (gInfo.Bar0Lo & 0xFFFFFFF0UL) + base.l;
    gProbe.WinPhys = phys.l;
    if (DH_PhysToGDTSel(phys.w.hi, phys.w.lo, 0, gSel))
        return 1;
    gWinBase = base.l;
    return 0;
}

static ULONG Rd(ULONG ofs)
{
    U32W o;
    if (MapWindow(ofs)) return 0xFFFFFFFFUL;
    o.l = ofs;
    MmioRd(gSel, o.w.lo);
    return MmioResult;
}

static void Wr(ULONG ofs, ULONG val)
{
    U32W o, v;
    if (MapWindow(ofs)) return;
    o.l = ofs;
    v.l = val;
    MmioWr(gSel, o.w.lo, v.w.lo, v.w.hi);
}

/* mt7921_l1_rr(): point HIF_REMAP_L1 at the upper 16 bits, read via
 * the L1 window at BAR0+0x40000.                                      */
static ULONG L1Rd(ULONG addr)
{
    U32W a;
    ULONG r;
    a.l = addr;
    r = Rd(REG_HIF_REMAP_L1);
    r = (r & 0xFFFF0000UL) | a.w.hi;
    Wr(REG_HIF_REMAP_L1, r);
    (void)Rd(REG_HIF_REMAP_L1);             /* read to push the write */
    return Rd(REG_REMAP_BASE_L1 + a.w.lo);
}

static int gTraceN;

static void TraceVal(ULONG v)
{
    int i;
    for (i = 0; i < gTraceN; i++)
        if (gProbe.Trace[i] == v) return;
    if (gTraceN < 4) gProbe.Trace[gTraceN++] = v;
}

/* poll until (reg & mask) == val; returns number of polls or 0.
 * With trace != 0 every distinct value read is recorded.             */
static ULONG Poll(ULONG ofs, ULONG mask, ULONG val, int trace)
{
    ULONG n, v;
    for (n = 1; n <= POLL_LIMIT; n++) {
        v = Rd(ofs);
        if (trace) TraceVal(v);
        if ((v & mask) == val)
            return n;
    }
    return 0;
}

static void Fail(USHORT r)
{
    if (gProbe.Result == PROBE_OK) gProbe.Result = r;
}

static void HwProbe(void)
{
    int i;
    ULONG n;

    FarZero((UCHAR FAR *)&gProbe, sizeof(gProbe));
    gTraceN = 0;
    gProbe.Magic = MT7921_PROBE_MAGIC;
    gProbe.PciCommand = gInfo.Command;

    if (!gSel) { gProbe.Result = PROBE_NO_SELECTOR; return; }
    gWinBase = 0xFFFFFFFFUL;                /* force a fresh mapping */
    if (MapWindow(REG_CONN_ON_LPCTL)) { gProbe.Result = PROBE_MAP_FAILED; return; }

    gProbe.LpctlBefore = Rd(REG_CONN_ON_LPCTL);
    gProbe.ConnOnMisc  = Rd(REG_CONN_ON_MISC);
    gProbe.RemapL1     = Rd(REG_HIF_REMAP_L1);
    if (gProbe.LpctlBefore == 0xFFFFFFFFUL) {
        gProbe.Result = PROBE_DEAD_BUS;
        return;
    }

    /* mt792xe_mcu_fw_pmctrl(): SET_OWN, wait OWN_SYNC == 4 */
    for (i = 0, n = 0; i < OWN_RETRIES && !n; i++) {
        Wr(REG_CONN_ON_LPCTL, LPCR_SET_OWN);
        n = Poll(REG_CONN_ON_LPCTL, LPCR_OWN_SYNC, LPCR_OWN_SYNC, 1);
    }
    gProbe.FwOwnPolls = n;
    gProbe.LpctlFwOwn = Rd(REG_CONN_ON_LPCTL);
    if (!n) Fail(PROBE_FW_OWN_TIMEOUT);     /* keep going: diagnostics */

    /* __mt792xe_mcu_drv_pmctrl(): CLR_OWN, wait OWN_SYNC == 0 */
    for (i = 0, n = 0; i < OWN_RETRIES && !n; i++) {
        Wr(REG_CONN_ON_LPCTL, LPCR_CLR_OWN);
        n = Poll(REG_CONN_ON_LPCTL, LPCR_OWN_SYNC, 0, 0);
    }
    gProbe.DrvOwnPolls = n;
    gProbe.LpctlDrvOwn = Rd(REG_CONN_ON_LPCTL);
    if (!n) Fail(PROBE_DRV_OWN_TIMEOUT);

    /* read the chip ID regardless: it also proves the 32-bit word
     * order of MmioRd/MmioWr and the L1 remap path                    */
    gProbe.ChipId  = L1Rd(HW_CHIPID);
    gProbe.HwRev   = L1Rd(HW_REV);
    gProbe.HwBound = L1Rd(HW_BOUND);
    {
        U32W c;
        c.l = gProbe.ChipId;
        if (c.w.lo != 0x7961 && c.w.lo != 0x7920)
            Fail(PROBE_BAD_CHIPID);
    }
}


/* ------------------------------------------------------------------ */
/* Phase 3a: FW boot (OID_PRIVATE_MT7921_FWBOOT)                       */
/* Register offsets are BAR0 offsets after the mt76 fixed_map.        */
/* ------------------------------------------------------------------ */
#define REG_WFDMA0_RST          0x000D4100UL
#define REG_MCU2HOST_SW_INT_ENA 0x000D41F4UL
#define REG_HOST_INT_ENA        0x000D4204UL
#define REG_GLO_CFG             0x000D4208UL
#define REG_RST_DTX_PTR         0x000D420CUL
#define REG_GLO_CFG_EXT0        0x000D42B0UL
#define REG_PRI_DLY_INT_CFG0    0x000D42F0UL
#define REG_TX_RING_BASE        0x000D4300UL    /* + 0x10 * ring    */
#define REG_RX_RING_BASE        0x000D4500UL
#define REG_TX_EXT_CTRL         0x000D4600UL    /* + 4 * ring       */
#define REG_RX_EXT_CTRL         0x000D4680UL
#define REG_DMASHDL_SW_CONTROL  0x000D6004UL    /* 0x7C026004       */
#define REG_WFDMA_DUMMY_CR      0x00002120UL    /* 0x54000120       */
#define REG_SWDEF_MODE          0x0009F23CUL    /* 0x0041F23C       */
#define REG_PCIE_MAC_PM         0x00010194UL

#define L1_WFSYS_SW_RST         0x18000140UL    /* via L1 remap     */
#define L1_TOP_LPCR_HOST_BAND0  0x18060010UL

#define GLO_TX_DMA_EN           0x00000001UL
#define GLO_TX_DMA_BUSY         0x00000002UL
#define GLO_RX_DMA_EN           0x00000004UL
#define GLO_RX_DMA_BUSY         0x00000008UL
#define GLO_DMA_SIZE_3          0x00000030UL
#define GLO_TX_WB_DDONE         0x00000040UL
#define GLO_FIFO_DIS_CHECK      0x00000800UL
#define GLO_FIFO_LITTLE_ENDIAN  0x00001000UL
#define GLO_RX_WB_DDONE         0x00002000UL
#define GLO_CHAIN_EN            0x00008000UL
#define GLO_OMIT_RX_INFO_PFET2  0x00200000UL
#define GLO_OMIT_RX_INFO        0x08000000UL
#define GLO_OMIT_TX_INFO        0x10000000UL
#define GLO_CLK_GAT_DIS         0x40000000UL

#define DESC_DMA_DONE_HI        0x8000          /* ctrl bit 31        */
#define DESC_LAST_SEC0_HI       0x4000          /* ctrl bit 30        */

#define MCU_CMD_TARGET_ADDR_LEN 0x01
#define MCU_CMD_FW_START        0x02
#define MCU_CE_CMD_GET_NIC_CAPAB 0x8A
#define MCU_Q_SET               1
#define MCU_Q_NA                3
#define MCU_CMD_PATCH_START     0x05
#define MCU_CMD_PATCH_FINISH    0x07
#define MCU_CMD_PATCH_SEM       0x10

#define TX_LIMIT                1000000UL       /* polls per TX desc  */
#define EVT_LIMIT               3000000UL       /* polls per event    */

typedef struct {
    ULONG  regs;            /* ring register block in BAR0          */
    USHORT desc;            /* descriptor offset in the DMA block   */
    USHORT n;
    USHORT bufs;            /* RX: buffer offset in its block       */
    USHORT head;
    USHORT tail;
    UCHAR  blk2;            /* RX buffers in DMA block 2 (v0.17)    */
} RING;

#define RING_BSEL(r)  ((r)->blk2 ? gDma2Sel : gDmaSel)
#define RING_BPHYS(r) ((r)->blk2 ? gDma2Phys : gDmaPhys)
#define RX_BUF_PTR(r, i) ((UCHAR FAR *)MkFar(RING_BSEL(r), (r)->bufs + (i) * RX_BUF))

static RING rTx0  = { REG_TX_RING_BASE + 0x000, OFS_TX0,  4,  0 };
static RING rFwdl = { REG_TX_RING_BASE + 0x100, OFS_FWDL, 16, 0 };
static RING rWm   = { REG_TX_RING_BASE + 0x110, OFS_WM,   16, 0 };
static RING rRx0  = { REG_RX_RING_BASE + 0x000, OFS_RX0,  8,  OFS_RXB0 };
static RING rRx2  = { REG_RX_RING_BASE + 0x020, OFS_RX2,  8,  OFS_RXB2 };
static RING rRx4  = { REG_RX_RING_BASE + 0x040, OFS_RX4,  32, OFS_RXB4, 0, 0, 1 };

static UCHAR gSeq;
static UCHAR gPayload[1200];                    /* hw_scan_req is 1186 bytes */
static UCHAR gSetQuery = MCU_Q_NA;              /* set_query of next McuSend */
static UCHAR gEvt[1024];                        /* last matching event       */
static USHORT gEvtLen;
static int   gFwRunning;                        /* RAM firmware started by us */

static ULONG DmaRd(USHORT off)
{
    MmioRd(gDmaSel, off);
    return MmioResult;
}

static void DmaWr(USHORT off, ULONG val)
{
    U32W v;
    v.l = val;
    MmioWr(gDmaSel, off, v.w.lo, v.w.hi);
}

static void DmaWrHL(USHORT off, USHORT hi, USHORT lo)
{
    MmioWr(gDmaSel, off, lo, hi);
}

static void L1Wr(ULONG addr, ULONG val)
{
    U32W a;
    ULONG r;
    a.l = addr;
    r = Rd(REG_HIF_REMAP_L1);
    r = (r & 0xFFFF0000UL) | a.w.hi;
    Wr(REG_HIF_REMAP_L1, r);
    (void)Rd(REG_HIF_REMAP_L1);
    Wr(REG_REMAP_BASE_L1 + a.w.lo, val);
}

static void Delay(ULONG n)              /* ~1 us per PCIe read */
{
    while (n--)
        (void)Rd(REG_GLO_CFG);
}

static void Set(ULONG ofs, ULONG bits)   { Wr(ofs, Rd(ofs) | bits); }
static void Clear(ULONG ofs, ULONG bits) { Wr(ofs, Rd(ofs) & ~bits); }

/* ---- rings -------------------------------------------------------- */
static void RingInitTx(RING *r)
{
    USHORT i, d;
    for (i = 0; i < r->n; i++) {
        d = r->desc + i * 16;
        DmaWr(d + 0, 0);
        DmaWrHL(d + 4, DESC_DMA_DONE_HI, 0);
        DmaWr(d + 8, 0);
        DmaWr(d + 12, 0);
    }
    Wr(r->regs + 0x8, 0);                       /* cpu_idx   */
    Wr(r->regs + 0xC, 0);                       /* dma_idx   */
    Wr(r->regs + 0x4, r->n);                    /* ring_size */
    Wr(r->regs + 0x0, gDmaPhys + r->desc);      /* desc_base */
    r->head = r->tail = 0;
}

static void RingInitRx(RING *r)
{
    USHORT i, d;
    for (i = 0; i < r->n; i++) {
        d = r->desc + i * 16;
        DmaWr(d + 0, RING_BPHYS(r) + r->bufs + i * RX_BUF);
        DmaWrHL(d + 4, RX_BUF, 0);              /* SD_LEN0 = 2048 */
        DmaWr(d + 8, 0);
        DmaWr(d + 12, 0);
    }
    Wr(r->regs + 0x8, 0);
    Wr(r->regs + 0xC, 0);
    Wr(r->regs + 0x4, r->n);
    Wr(r->regs + 0x0, gDmaPhys + r->desc);
    r->head = r->n - 1;                         /* n-1 buffers given */
    r->tail = 0;
    Wr(r->regs + 0x8, r->head);
}

static void DumpRegs(void)
{
    gBoot.Dump[DUMP_LPCTL]       = Rd(REG_CONN_ON_LPCTL);
    gBoot.Dump[DUMP_CONN_MISC]   = Rd(REG_CONN_ON_MISC);
    gBoot.Dump[DUMP_LPCR_HOST]   = L1Rd(L1_TOP_LPCR_HOST_BAND0);
    gBoot.Dump[DUMP_WFSYS_RST]   = L1Rd(L1_WFSYS_SW_RST);
    gBoot.Dump[DUMP_WFDMA_RST]   = Rd(REG_WFDMA0_RST);
    gBoot.Dump[DUMP_BUSY_ENA]    = Rd(0x000D413CUL);
    gBoot.Dump[DUMP_MCU_CMD]     = Rd(0x000D41F0UL);
    gBoot.Dump[DUMP_GLO_CFG]     = Rd(REG_GLO_CFG);
    gBoot.Dump[DUMP_GLO_EXT0]    = Rd(REG_GLO_CFG_EXT0);
    gBoot.Dump[DUMP_DMASHDL]     = Rd(REG_DMASHDL_SW_CONTROL);
    gBoot.Dump[DUMP_DUMMY_CR]    = Rd(REG_WFDMA_DUMMY_CR);
    gBoot.Dump[DUMP_SWDEF_MODE]  = Rd(REG_SWDEF_MODE);
    gBoot.Dump[DUMP_PCIE_MAC_PM] = Rd(REG_PCIE_MAC_PM);
    gBoot.Dump[DUMP_TX16_DIDX]   = Rd(REG_TX_RING_BASE + 0x100 + 0xC);
    gBoot.Dump[DUMP_TX17_EXT]    = Rd(REG_TX_EXT_CTRL + 0x44);
    gBoot.Dump[DUMP_RX0_DIDX]    = Rd(REG_RX_RING_BASE + 0xC);
}

/* queue one buffer and wait until the DMA engine has taken it        */
static int TxSend(RING *r, ULONG phys, USHORT len)
{
    USHORT d = r->desc + r->head * 16;
    ULONG n;

    DmaWr(d + 0, phys);
    DmaWr(d + 8, 0);
    DmaWr(d + 12, 0);
    DmaWrHL(d + 4, (USHORT)(len | DESC_LAST_SEC0_HI), 0);
    r->head = (r->head + 1) % r->n;
    Wr(r->regs + 0x8, r->head);                 /* kick */

    for (n = 1; n <= TX_LIMIT; n++)
        if (Rd(r->regs + 0xC) == r->head) {
            gBoot.Polls = n;
            return 0;
        }
    gBoot.Polls = n;

    /* timeout: record what the hardware sees */
    gBoot.RingBase = Rd(r->regs + 0x0);
    gBoot.RingCnt  = Rd(r->regs + 0x4);
    gBoot.RingCidx = Rd(r->regs + 0x8);
    gBoot.RingDidx = Rd(r->regs + 0xC);
    gBoot.DescCtrl = DmaRd(d + 4);
    gBoot.IntSta   = Rd(0x000D4200UL);
    {
        U32W a;
        a.w.hi = (USHORT)(0x8000 | gInfo.Bus);
        a.w.lo = (USHORT)(((USHORT)gInfo.DevFunc << 8) | 0x04);
        PciCfgRd(a.w.hi, a.w.lo);
        gBoot.PciStat = MmioResult;
    }
    DumpRegs();
    return 1;
}

/* wait for the next event on RX ring 0 (ROM) or 4 (firmware, after
 * download); returns its length (0 = timeout) and the ring in *which */
static USHORT RxWait(RING **which)
{
    static RING *rings[2] = { &rRx0, &rRx4 };
    U32W c;
    ULONG n;
    int k;

    for (n = 1; n <= EVT_LIMIT; n++) {
        for (k = 0; k < 2; k++) {
            RING *r = rings[k];
            c.l = DmaRd(r->desc + r->tail * 16 + 4);
            if (c.w.hi & DESC_DMA_DONE_HI) {
                gBoot.Polls = n;
                gBoot.EvtRing = k ? 4 : 0;
                *which = r;
                return (USHORT)(c.w.hi & 0x3FFF);  /* SD_LEN0 */
            }
        }
        (void)Rd(REG_GLO_CFG);                 /* pace the loop */
    }
    gBoot.Polls = n;
    return 0;
}

static void RxRelease(RING *r)
{
    r->tail = (r->tail + 1) % r->n;
    DmaWrHL(r->desc + r->head * 16 + 4, RX_BUF, 0);   /* re-arm */
    r->head = (r->head + 1) % r->n;
    Wr(r->regs + 0x8, r->head);
}

/* ---- MCU messages (mt76_connac2_mcu_fill_message, legacy format) -- */
/* returns the status byte of the response (0..255), 0xFFFF if none,
 * 0xFFFE if the command could not be sent.                           */
/* message kinds (mt76_connac2_mcu_fill_message) */
#define MK_LEGACY   0           /* mcu_txd, 64-byte header (CMD / CE)    */
#define MK_EXT      1           /* mcu_txd with cid 0xED + ext_cid       */
#define MK_UNI      2           /* uni_txd, 48-byte header               */

static UCHAR gKind = MK_LEGACY;                 /* kind of the next McuSend */
static UCHAR gExtCid;                           /* ext_cid for MK_EXT       */

static USHORT McuSend(UCHAR cid, USHORT plen, int wait)
{
    USHORT hdr = (gKind == MK_UNI) ? 48 : 64;
    USHORT total = hdr + plen, i, len, tries;
    UCHAR FAR *buf = (UCHAR FAR *)MkFar(gDmaSel, OFS_CMD);
    UCHAR seq;

    seq = (UCHAR)(++gSeq & 0x0F);
    if (!seq) seq = (UCHAR)(++gSeq & 0x0F);
    gBoot.LastCmd = cid;

    FarZero(buf, 64);
    DmaWrHL(OFS_CMD + 0, 0x4100, total);        /* Q_IDX 0x20, PKT_FMT CMD */
    DmaWrHL(OFS_CMD + 4, 0x8001, 0);            /* LONG_FORMAT, HDR CMD    */
    if (gKind == MK_UNI) {
        /* uni_txd: len, cid (le16) | rsv, pkt_type, frag_n, seq |
         * checksum, s2d_index, option (UNI_EXT_ACK = ACK|UNI|SET)      */
        DmaWrHL(OFS_CMD + 32, cid, total - 32);
        DmaWrHL(OFS_CMD + 36, (USHORT)((USHORT)seq << 8), 0xA000);
        DmaWrHL(OFS_CMD + 40, 0x0700, 0);
    } else {
        UCHAR c = (gKind == MK_EXT) ? 0xED : cid;
        UCHAR sq = (gKind == MK_EXT) ? MCU_Q_SET : gSetQuery;
        DmaWrHL(OFS_CMD + 32, 0x8000, total - 32);  /* pq_id : len        */
        DmaWrHL(OFS_CMD + 36, (USHORT)(((USHORT)seq << 8) | sq), /* seq, set_query */
                (USHORT)(0xA000 | c));          /* pkt_type 0xa0, cid      */
        if (gKind == MK_EXT)                    /* ext_cid, ext_cid_ack    */
            DmaWrHL(OFS_CMD + 40, 0x0100, (USHORT)((USHORT)gExtCid << 8));
    }
    gSetQuery = MCU_Q_NA;                       /* back to the defaults    */
    gKind = MK_LEGACY;
    for (i = 0; i < plen; i++)
        buf[hdr + i] = gPayload[i];

    if (TxSend(&rWm, gDmaPhys + OFS_CMD, total))
        return 0xFFFE;
    if (!wait)
        return 0;

    for (tries = 0; tries < 8; tries++) {
        static RING *rr;                    /* static: SS != DS in a PDD */
        UCHAR FAR *ev;
        len = RxWait(&rr);
        if (!len)
            return 0xFFFF;
        ev = RX_BUF_PTR(rr, rr->tail);
        gBoot.LastEvtLen = len;
        FarCopy((UCHAR FAR *)gBoot.LastEvt, ev + 24, 24);
        if (len >= 33 && ev[29] == seq) {
            USHORT st = ev[32];
            gEvtLen = len > sizeof(gEvt) ? sizeof(gEvt) : len;
            FarCopy((UCHAR FAR *)gEvt, ev, gEvtLen);
            RxRelease(rr);
            return st;
        }
        RxRelease(rr);                          /* other event: skip */
    }
    return 0xFFFF;
}

static void PutLe32(USHORT at, ULONG v)
{
    union { ULONG l; UCHAR b[4]; } u;
    u.l = v;
    gPayload[at] = u.b[0]; gPayload[at + 1] = u.b[1];
    gPayload[at + 2] = u.b[2]; gPayload[at + 3] = u.b[3];
}

static int Fwb(USHORT step, USHORT err)
{
    gBoot.Step = step;
    gBoot.Error = err;
    return err;
}

static int OwnHandshake(void)
{
    int i;
    ULONG n = 0;
    for (i = 0; i < OWN_RETRIES && !n; i++) {
        Wr(REG_CONN_ON_LPCTL, LPCR_SET_OWN);
        n = Poll(REG_CONN_ON_LPCTL, LPCR_OWN_SYNC, LPCR_OWN_SYNC, 0);
    }
    if (!n) return 1;
    for (i = 0, n = 0; i < OWN_RETRIES && !n; i++) {
        Wr(REG_CONN_ON_LPCTL, LPCR_CLR_OWN);
        n = Poll(REG_CONN_ON_LPCTL, LPCR_OWN_SYNC, 0, 0);
    }
    gBoot.Polls = n;
    return n ? 0 : 1;
}

static USHORT gFwx;                             /* FWX_ experiment flags */

/* ---- PCI config helpers (mechanism #1, ring 0) -------------------- */
static ULONG CfgRd(UCHAR bus, UCHAR devfn, UCHAR reg)
{
    PciCfgRd((USHORT)(0x8000 | bus), (USHORT)(((USHORT)devfn << 8) | (reg & 0xFC)));
    return MmioResult;
}

static void CfgWr(UCHAR bus, UCHAR devfn, UCHAR reg, ULONG val)
{
    U32W v;
    v.l = val;
    PciCfgWr((USHORT)(0x8000 | bus), (USHORT)(((USHORT)devfn << 8) | (reg & 0xFC)),
             v.w.lo, v.w.hi);
}

static UCHAR FindPcieCap(UCHAR bus, UCHAR devfn)
{
    U32W r;
    UCHAR p;
    int guard = 48;
    r.l = CfgRd(bus, devfn, 0x34);
    p = (UCHAR)(r.w.lo & 0xFC);
    while (p && guard--) {
        r.l = CfgRd(bus, devfn, p);
        if ((r.w.lo & 0xFF) == 0x10) return p;  /* PCI Express */
        p = (UCHAR)((r.w.lo >> 8) & 0xFC);
    }
    return 0;
}

/* find the bridge on bus 0 whose secondary bus is the card's bus */
static void FindRootPort(void)
{
    USHORT df;
    U32W r;
    for (df = 0; df < 256; df++) {
        r.l = CfgRd(0, (UCHAR)df, 0x00);
        if (r.w.lo == 0xFFFF) continue;
        r.l = CfgRd(0, (UCHAR)df, 0x0C);
        if ((r.w.hi & 0x7F) != 0x01) continue;  /* header type 1 */
        r.l = CfgRd(0, (UCHAR)df, 0x18);
        if ((UCHAR)(r.w.lo >> 8) == gInfo.Bus) {
            gBoot.RpBus = 0;
            gBoot.RpDevFn = (UCHAR)df;
            gBoot.RpCap = FindPcieCap(0, (UCHAR)df);
            return;
        }
    }
    gBoot.RpDevFn = 0xFF;
}

/* clear the PCIe error status (RW1C) of card and root port */
static void PcieErrClear(void)
{
    if (gBoot.DevCap) {
        gBoot.DevCtlStaBefore = CfgRd(gInfo.Bus, gInfo.DevFunc, gBoot.DevCap + 8);
        CfgWr(gInfo.Bus, gInfo.DevFunc, gBoot.DevCap + 8,
              (gBoot.DevCtlStaBefore & 0x0000FFFFUL) | 0x000F0000UL);
    }
    if (gBoot.RpCap && gBoot.RpDevFn != 0xFF) {
        ULONG v;
        gBoot.RpCtlStaBefore = CfgRd(0, gBoot.RpDevFn, gBoot.RpCap + 8);
        CfgWr(0, gBoot.RpDevFn, gBoot.RpCap + 8,
              (gBoot.RpCtlStaBefore & 0x0000FFFFUL) | 0x000F0000UL);
        v = CfgRd(0, gBoot.RpDevFn, 0x1C);      /* secondary status RW1C */
        CfgWr(0, gBoot.RpDevFn, 0x1C, (v & 0x0000FFFFUL) | 0xF9000000UL);
    }
}

static void PcieErrRead(void)
{
    if (gBoot.DevCap) {
        gBoot.DevCtlSta = CfgRd(gInfo.Bus, gInfo.DevFunc, gBoot.DevCap + 8);
        gBoot.DevLnk    = CfgRd(gInfo.Bus, gInfo.DevFunc, gBoot.DevCap + 0x10);
    }
    if (gBoot.RpCap && gBoot.RpDevFn != 0xFF) {
        gBoot.RpCtlSta = CfgRd(0, gBoot.RpDevFn, gBoot.RpCap + 8);
        gBoot.RpSecSta = CfgRd(0, gBoot.RpDevFn, 0x1C);
    }
}

static void EnableBusMaster(void)
{
    U32W a;
    ULONG v;
    a.w.hi = (USHORT)(0x8000 | gInfo.Bus);
    a.w.lo = (USHORT)(((USHORT)gInfo.DevFunc << 8) | 0x04);
    PciCfgRd(a.w.hi, a.w.lo);
    v = (MmioResult & 0x0000FFFFUL) | 0x0006;   /* MEM + BUS MASTER */
    PciCfgWr(a.w.hi, a.w.lo, (USHORT)v, 0);
    PciCfgRd(a.w.hi, a.w.lo);
    gBoot.PciCmd = MmioResult;
}

static void DmaDisable(void)
{
    ULONG n;
    Clear(REG_GLO_CFG, GLO_TX_DMA_EN | GLO_RX_DMA_EN | GLO_CHAIN_EN |
                       GLO_OMIT_TX_INFO | GLO_OMIT_RX_INFO |
                       GLO_OMIT_RX_INFO_PFET2);
    for (n = 0; n < POLL_LIMIT; n++)
        if (!(Rd(REG_GLO_CFG) & (GLO_TX_DMA_BUSY | GLO_RX_DMA_BUSY)))
            break;
    Clear(REG_GLO_CFG_EXT0, 0x00000040UL);      /* TX_DMASHDL_EN    */
    Set(REG_DMASHDL_SW_CONTROL, 0x10000000UL);  /* DMASHDL_BYPASS   */
    if (!(gFwx & FWX_NO_WFDMA_RST)) {
        Clear(REG_WFDMA0_RST, 0x00000030UL);    /* LOGIC + DMASHDL  */
        Set(REG_WFDMA0_RST, 0x00000030UL);
    }
    if (gFwx & FWX_KEEP_DMASHDL_BYPASS)
        Set(REG_DMASHDL_SW_CONTROL, 0x10000000UL);
    if (gFwx & FWX_CLR_DMASHDL_LITE)
        Clear(REG_GLO_CFG_EXT0, 0x00000080UL);
}

static void DmaEnable(void)
{
    /* mt792x_dma_prefetch(), MT7921 values */
    Wr(REG_RX_EXT_CTRL + 0x00, 0x00000004UL);   /* RX0  */
    Wr(REG_RX_EXT_CTRL + 0x08, 0x00400004UL);   /* RX2  */
    Wr(REG_RX_EXT_CTRL + 0x0C, 0x00800004UL);   /* RX3  */
    Wr(REG_RX_EXT_CTRL + 0x10, 0x00C00004UL);   /* RX4  */
    Wr(REG_RX_EXT_CTRL + 0x14, 0x01000004UL);   /* RX5  */
    Wr(REG_TX_EXT_CTRL + 0x00, 0x01400004UL);   /* TX0  */
    Wr(REG_TX_EXT_CTRL + 0x04, 0x01800004UL);
    Wr(REG_TX_EXT_CTRL + 0x08, 0x01C00004UL);
    Wr(REG_TX_EXT_CTRL + 0x0C, 0x02000004UL);
    Wr(REG_TX_EXT_CTRL + 0x10, 0x02400004UL);
    Wr(REG_TX_EXT_CTRL + 0x14, 0x02800004UL);
    Wr(REG_TX_EXT_CTRL + 0x18, 0x02C00004UL);   /* TX6  */
    Wr(REG_TX_EXT_CTRL + 0x40, 0x03400004UL);   /* TX16 */
    Wr(REG_TX_EXT_CTRL + 0x44, 0x03800004UL);   /* TX17 */

    Wr(REG_RST_DTX_PTR, 0xFFFFFFFFUL);
    Wr(REG_PRI_DLY_INT_CFG0, 0);
    Set(REG_GLO_CFG, GLO_TX_WB_DDONE | GLO_FIFO_LITTLE_ENDIAN |
                     GLO_CLK_GAT_DIS | GLO_OMIT_TX_INFO | GLO_DMA_SIZE_3 |
                     GLO_FIFO_DIS_CHECK | GLO_RX_WB_DDONE | GLO_CHAIN_EN |
                     GLO_OMIT_RX_INFO_PFET2);
    if (gFwx & FWX_CLR_LBK_RX_Q_SEL)
        Clear(REG_GLO_CFG, 0x00100000UL);
    Set(REG_GLO_CFG, GLO_TX_DMA_EN | GLO_RX_DMA_EN);
    Set(REG_WFDMA_DUMMY_CR, 0x00000002UL);      /* NEED_REINIT */
    /* no host interrupts: everything is polled for now */
    Set(REG_MCU2HOST_SW_INT_ENA, 0x00000001UL); /* WAKE_RX_PCIE */
}

/* ---- Intel VT-d check -------------------------------------------- */
/* read any physical dword through the FW selector (task time)       */
static ULONG PhysRd(ULONG phys)
{
    U32W p;
    p.l = phys;
    if (!PhysPtr(phys))
        return 0xFFFFFFFFUL;
    MmioRd(gFwSel, p.w.lo);
    return MmioResult;
}

static int SigIs(ULONG phys, const char *sig)   /* 4 char signature */
{
    union { ULONG l; char c[4]; } u;
    u.l = PhysRd(phys);
    return u.c[0] == sig[0] && u.c[1] == sig[1] &&
           u.c[2] == sig[2] && u.c[3] == sig[3];
}

static ULONG FindRsdp(void)
{
    ULONG a, ebda;
    U32W w;
    w.l = PhysRd(0x40CUL);                      /* BDA: EBDA segment @ 40E */
    ebda = (ULONG)w.w.hi << 4;
    if (ebda >= 0x80000UL && ebda < 0xA0000UL)
        for (a = ebda; a < ebda + 1024; a += 16)
            if (SigIs(a, "RSD ") && SigIs(a + 4, "PTR "))
                return a;
    for (a = 0xE0000UL; a < 0x100000UL; a += 16)
        if (SigIs(a, "RSD ") && SigIs(a + 4, "PTR "))
            return a;
    return 0;
}

static void CheckVtd(void)
{
    ULONG rsdp, xsdt, len, e, t, end, p;
    USHORT n = 0;
    U32W rev;

    rsdp = FindRsdp();
    if (!rsdp) return;
    rev.l = PhysRd(rsdp + 12);                  /* byte 15 = revision */
    if ((rev.w.hi >> 8) >= 2 && PhysRd(rsdp + 28) == 0) {
        xsdt = PhysRd(rsdp + 24);               /* XSDT, 8-byte entries */
        len  = PhysRd(xsdt + 4);
        for (e = xsdt + 36; e + 8 <= xsdt + len; e += 8) {
            t = PhysRd(e);
            if (PhysRd(e + 4) == 0 && SigIs(t, "DMAR")) { gBoot.DmarPhys = t; break; }
        }
    } else {
        xsdt = PhysRd(rsdp + 16);               /* RSDT, 4-byte entries */
        len  = PhysRd(xsdt + 4);
        for (e = xsdt + 36; e + 4 <= xsdt + len; e += 4) {
            t = PhysRd(e);
            if (SigIs(t, "DMAR")) { gBoot.DmarPhys = t; break; }
        }
    }
    if (!gBoot.DmarPhys) return;

    end = gBoot.DmarPhys + PhysRd(gBoot.DmarPhys + 4);
    for (p = gBoot.DmarPhys + 48; p + 16 <= end; ) {
        U32W h;
        h.l = PhysRd(p);                        /* type (lo), length (hi) */
        if (h.w.hi < 4) break;
        if (h.w.lo == 0) {                      /* DRHD */
            if (n < 2 && PhysRd(p + 12) == 0) {
                gBoot.DrhdBase[n] = PhysRd(p + 8);
                gBoot.DrhdGsts[n] = PhysRd(gBoot.DrhdBase[n] + 0x1C);
            }
            n++;
        }
        p += h.w.hi;
    }
    gBoot.DmarFound = n;
}

static void NicCap(void);

/* ---- RAM code (mt76_connac2_load_ram / send_ram_firmware) -------- */
static UCHAR RamByte(ULONG ofs)
{
    UCHAR FAR *p = PhysPtr(gRamPhys + ofs);
    return p ? *p : 0;
}

static ULONG RamLe32(ULONG ofs)
{
    union { ULONG l; UCHAR b[4]; } u;
    u.b[0] = RamByte(ofs);     u.b[1] = RamByte(ofs + 1);
    u.b[2] = RamByte(ofs + 2); u.b[3] = RamByte(ofs + 3);
    return u.l;
}

static void RamBoot(void)
{
    ULONG tr, base, offset = 0, override = 0, n;
    USHORT nreg, i, st;

    gBoot.RamState = gRamState;
    gBoot.RamSize  = gRamSize;
    gBoot.RamPhys  = gRamPhys;
    gBoot.FwStart  = 0xFFFF;

    /* already running? (second /fw run) */
    gBoot.ConnMisc = Rd(REG_CONN_ON_MISC);
    if ((gBoot.ConnMisc & 3UL) == 3UL) { Fwb(FWB_STEP_DONE, FWB_OK); return; }

    if (gRamState != FWS_LOADED) { Fwb(FWB_STEP_RAM_DL, FWB_ERR_NO_RAM); return; }

    /* trailer: 36 bytes at the end; regions (40 bytes each) before it */
    tr = gRamSize - 36;
    nreg = RamByte(tr + 2);
    for (i = 0; i < 10; i++) gBoot.FwVer[i]  = (char)RamByte(tr + 7 + i);
    for (i = 0; i < 14; i++) gBoot.FwDate[i] = (char)RamByte(tr + 17 + i);
    /* 16-bit multiplies only: a 32-bit one needs __U4M from the clib */
    if (nreg == 0 || nreg > 8 || (ULONG)(USHORT)(nreg * 40) > tr) {
        Fwb(FWB_STEP_RAM_DL, FWB_ERR_BAD_RAM); return;
    }
    base = tr - (ULONG)(USHORT)(nreg * 40);

    for (i = 0; i < nreg; i++) {
        ULONG rb = base + (ULONG)(USHORT)(i * 40), addr, len, mode, phys;
        UCHAR feat;

        addr = RamLe32(rb + 16);
        len  = RamLe32(rb + 20);
        feat = RamByte(rb + 24);
        if (len > base - offset) { Fwb(FWB_STEP_RAM_DL, FWB_ERR_BAD_RAM); return; }

        if (!(feat & 0x40)) {                   /* not FW_FEATURE_NON_DL */
            mode = 0x80000000UL;                /* DL_MODE_NEED_RSP */
            if (feat & 0x01) mode |= 0x00000009UL;  /* ENCRYPT + RESET_SEC_IV */
            if (feat & 0x10) mode |= 0x00000040UL;  /* ENCRY_MODE_SEL */
            mode |= (ULONG)(feat & 0x06);           /* KEY_IDX, same bits */
            if (feat & 0x20) override = addr;       /* OVERRIDE_ADDR */

            PutLe32(0, addr);
            PutLe32(4, len);
            PutLe32(8, mode);
            st = McuSend((addr == 0x00200000UL || addr == 0x00900000UL) ?
                         MCU_CMD_PATCH_START : MCU_CMD_TARGET_ADDR_LEN, 12, 1);
            if (st >= 0xFFFE) {
                Fwb(FWB_STEP_RAM_DL, st == 0xFFFE ? FWB_ERR_TX_TIMEOUT : FWB_ERR_NO_EVENT);
                return;
            }

            phys = gRamPhys + offset;
            n = len;
            while (n) {
                USHORT chunk = n > 4096UL ? 4096 : (USHORT)n;
                if (TxSend(&rFwdl, phys, chunk)) {
                    Fwb(FWB_STEP_RAM_DL, FWB_ERR_TX_TIMEOUT);
                    return;
                }
                gBoot.RamChunks++;
                phys += chunk;
                n    -= chunk;
            }
            gBoot.RamRegionsDone++;
        }
        offset += len;
    }

    /* FW_START_REQ: option OVERRIDE if a region asked for it */
    PutLe32(0, override ? 0x00000001UL : 0);
    PutLe32(4, override);
    st = McuSend(MCU_CMD_FW_START, 8, 1);
    gBoot.FwStart = st;
    if (st >= 0xFFFE) {
        Fwb(FWB_STEP_FW_START, st == 0xFFFE ? FWB_ERR_TX_TIMEOUT : FWB_ERR_NO_EVENT);
        return;
    }

    /* mt792x_load_firmware: poll MT_TOP_MISC2_FW_N9_RDY, 1.5 s */
    for (n = 1; n <= 3000000UL; n++) {
        gBoot.ConnMisc = Rd(REG_CONN_ON_MISC);
        if ((gBoot.ConnMisc & 3UL) == 3UL) break;
    }
    gBoot.N9Polls = n;
    if (n > 3000000UL) { Fwb(FWB_STEP_N9_READY, FWB_ERR_TIMEOUT); return; }

    gFwRunning = 1;
    Fwb(FWB_STEP_DONE, FWB_OK);
    NicCap();                                   /* real MAC address */
}

/* ---- PCIe secondary bus reset of the card (v0.12) ----------------- */
/* After a warm reboot the chip still holds the patch/firmware and old
 * DMA rings of the previous session. Resetting the link below the root
 * port gives a cold chip, like a power cycle. Config space of the card
 * is saved and restored around it.                                   */
static void CfgDelay(ULONG n)                   /* ~1 us per config read */
{
    while (n--)
        (void)CfgRd(0, gBoot.RpDevFn, 0x00);
}

static USHORT BusReset(void)
{
    static ULONG save[16], devctl, lnkctl;
    UCHAR b = gInfo.Bus, df = gInfo.DevFunc, rp = gBoot.RpDevFn;
    ULONG bc, n;
    U32W v;
    USHORT i;

    if (rp == 0xFF || !gBoot.RpCap)
        return BRS_NO_PORT;

    for (i = 0; i < 16; i++)
        save[i] = CfgRd(b, df, (UCHAR)(i * 4));
    if (gBoot.DevCap) {
        devctl = CfgRd(b, df, gBoot.DevCap + 0x08);
        lnkctl = CfgRd(b, df, gBoot.DevCap + 0x10);
    }

    bc = CfgRd(0, rp, 0x3C);                    /* bridge control in 31:16 */
    CfgWr(0, rp, 0x3C, bc | 0x00400000UL);      /* bit 6: secondary reset  */
    CfgDelay(5000UL);                           /* hold >= 1 ms            */
    CfgWr(0, rp, 0x3C, bc & ~0x00400000UL);
    CfgDelay(300000UL);                         /* >= 100 ms link + init   */

    for (n = 1; n <= 1000000UL; n++) {
        v.l = CfgRd(b, df, 0x00);
        if (v.w.lo == MT_VENDOR && v.w.hi == MT_DEVICE)
            break;
    }
    gBoot.ResetPolls = n;
    if (n > 1000000UL)
        return BRS_FAILED;

    /* restore: BARs first, then misc, PCIe control, command last */
    for (i = 4; i <= 9; i++)
        CfgWr(b, df, (UCHAR)(i * 4), save[i]);
    CfgWr(b, df, 0x0C, save[3]);
    CfgWr(b, df, 0x3C, save[15]);
    if (gBoot.DevCap) {
        CfgWr(b, df, gBoot.DevCap + 0x08, devctl & 0x0000FFFFUL);
        CfgWr(b, df, gBoot.DevCap + 0x10, lnkctl & 0x0000FFFFUL);
    }
    CfgWr(b, df, 0x04, save[1] & 0x0000FFFFUL);
    gWinBase = 0xFFFFFFFFUL;
    return BRS_DONE;
}

/* ---- first command to the running firmware (v0.12) --------------- */
/* mt7921_mcu_get_nic_capability(): CE command 0x8a, no payload; the
 * answer is rxd (36 bytes), n_element (le16, +2 rsv), then TLVs of
 * { le32 type, le32 len, data }. Type 7 = MAC address.               */
static USHORT EvtLe16(USHORT o) { return (USHORT)(gEvt[o] | ((USHORT)gEvt[o + 1] << 8)); }

static void NicCap(void)
{
    USHORT st, n, i, p;
    U32W m;

    gSetQuery = MCU_Q_SET;
    st = McuSend(MCU_CE_CMD_GET_NIC_CAPAB, 0, 1);
    if (st >= 0xFFFE) { gBoot.NicStatus = st; return; }
    gBoot.NicStatus = 0;
    gBoot.NicEvtLen = gEvtLen;
    if (gEvtLen < 40) return;

    n = EvtLe16(36);
    gBoot.NicElems = n;
    m.l = 0;
    for (i = 0, p = 40; i < n && p + 8 <= gEvtLen; i++) {
        USHORT type = EvtLe16(p), len = EvtLe16(p + 4);
        p += 8;
        if (p + len > gEvtLen) break;
        if (type < 16)      m.w.lo |= (USHORT)(1U << type);
        else if (type < 32) m.w.hi |= (USHORT)(1U << (type - 16));
        if (type == 7 && len >= 6) {            /* MT_NIC_CAP_MAC_ADDR */
            FarCopy((UCHAR FAR *)gBoot.Mac, (UCHAR FAR *)&gEvt[p], 6);
            FarCopy((UCHAR FAR *)gMac, (UCHAR FAR *)&gEvt[p], 6);
        }
        p += len;
    }
    gBoot.CapMask = m.l;
}

/* ---- reuse firmware from a previous session (v0.13) -------------- */
/* Order matters: the old DMA rings may point into memory that OS/2
 * has since given to someone else (maybe even to us). So: take driver
 * ownership, stop WFDMA while bus mastering is still off, then build
 * fresh rings, and only then allow bus mastering and start WFDMA.    */
static void Reattach(void)
{
    ULONG n;
    U32W a;

    gBoot.BusReset = BRS_REATTACHED;

    if (OwnHandshake()) { Fwb(FWB_STEP_OWN, FWB_ERR_TIMEOUT); return; }
    gBoot.ChipId = L1Rd(HW_CHIPID);
    a.l = gBoot.ChipId;
    if (a.w.lo != 0x7961) { Fwb(FWB_STEP_CHIPID, FWB_ERR_CHIPID); return; }

    Wr(REG_HOST_INT_ENA, 0);
    DmaDisable();                       /* stop stale DMA first          */

    FarZero((UCHAR FAR *)MkFar(gDmaSel, 0), 0x8000);
    FarZero((UCHAR FAR *)MkFar(gDmaSel, 0x8000), 0x8000);
    FarZero((UCHAR FAR *)MkFar(gDma2Sel, 0), 0x8000);
    FarZero((UCHAR FAR *)MkFar(gDma2Sel, 0x8000), 0x8000);
    RingInitTx(&rTx0);
    Wr(REG_TX_EXT_CTRL, 0x00000004UL);
    RingInitTx(&rWm);
    RingInitTx(&rFwdl);
    RingInitRx(&rRx0);
    RingInitRx(&rRx4);
    RingInitRx(&rRx2);

    EnableBusMaster();
    if (!(gBoot.PciCmd & 0x0004)) { Fwb(FWB_STEP_BUSMASTER, FWB_ERR_TIMEOUT); return; }
    Wr(0x00010188UL, 0x000000FFUL);     /* PCIE_MAC_INT_ENABLE */
    DmaEnable();
    gBoot.GloCfg = Rd(REG_GLO_CFG);

    /* mt7921e_driver_own + L0s off, as after a normal boot */
    L1Wr(L1_TOP_LPCR_HOST_BAND0, 0x00000002UL);
    for (n = 1; n <= 500000UL; n++)
        if (!(L1Rd(L1_TOP_LPCR_HOST_BAND0) & 0x00000001UL))
            break;
    gBoot.Polls = n;
    if (n > 500000UL) { Fwb(FWB_STEP_MCU_OWN, FWB_ERR_TIMEOUT); return; }
    Set(REG_PCIE_MAC_PM, 0x00000100UL);

    gFwRunning = 1;
    Fwb(FWB_STEP_DONE, FWB_OK);
    NicCap();
}

static void FwBoot(void)
{
    ULONG n, v;
    USHORT st, i;
    U32W a;
    UCHAR FAR *h;

    FarZero((UCHAR FAR *)&gBoot, sizeof(gBoot));
    gBoot.Magic   = MT7921_FWBOOT_MAGIC;
    gBoot.FwState = gFwState;
    gBoot.FwSize  = gFwSize;
    gBoot.FwPhys  = gFwPhys;
    gBoot.DmaPhys = gDmaPhys;
    gBoot.SemGet = gBoot.SemRel = gBoot.Finish = 0xFFFF;
    gWinBase = 0xFFFFFFFFUL;
    gFwWin   = 0xFFFFFFFFUL;
    gBoot.Flags    = gFwx;
    gBoot.RamState = gRamState;
    gBoot.RamSize  = gRamSize;
    gBoot.RamPhys  = gRamPhys;
    gBoot.FwStart  = 0xFFFF;
    gBoot.NicStatus = 0xFFFF;

    /* firmware started earlier by this driver: leave rings alone, only
     * talk to it                                                      */
    if (gFwRunning) {
        gBoot.BusReset = BRS_NOT_DONE;
        gBoot.ConnMisc = Rd(REG_CONN_ON_MISC);
        NicCap();
        Fwb(FWB_STEP_DONE, FWB_OK);
        return;
    }

    if (gSel) CheckVtd();                       /* diagnostics only */
    gBoot.DevCap = FindPcieCap(gInfo.Bus, gInfo.DevFunc);
    FindRootPort();

    if (!gSel || gFwState != FWS_LOADED) { Fwb(FWB_STEP_START, FWB_ERR_NO_FW); return; }
    if (!gDmaPhys)                       { Fwb(FWB_STEP_START, FWB_ERR_NO_DMA); return; }

    /* v0.12: cold chip via PCIe secondary bus reset (warm reboot fix) */
    if (!(gFwx & FWX_BUS_RESET)) {
        gBoot.BusReset = BRS_SKIPPED;
    } else {
        gBoot.BusReset = BusReset();
        if (gBoot.BusReset == BRS_FAILED) { Fwb(FWB_STEP_START, FWB_ERR_TIMEOUT); return; }
    }

    /* v0.13: WiFi firmware still running from a previous session (warm
     * reboot): keep it and only set up the host side again, like the
     * Linux wake-up path (mt792x_wpdma_reinit_cond)                   */
    gBoot.ConnMisc = Rd(REG_CONN_ON_MISC);
    if ((gBoot.ConnMisc & 3UL) == 3UL) {
        Reattach();
        return;
    }
    FarZero((UCHAR FAR *)MkFar(gDmaSel, 0), 0x8000);
    FarZero((UCHAR FAR *)MkFar(gDmaSel, 0x8000), 0x8000);
    FarZero((UCHAR FAR *)MkFar(gDma2Sel, 0), 0x8000);
    FarZero((UCHAR FAR *)MkFar(gDma2Sel, 0x8000), 0x8000);
    gSeq = 0;

    /* patch header (mt76_connac2_patch_hdr, big endian) */
    h = FwPtr(0);
    FarCopy((UCHAR FAR *)gBoot.BuildDate, h, 16);
    gBoot.HwSwVer  = Be32(h + 20);
    gBoot.PatchVer = Be32(h + 24);
    gBoot.NRegion  = Be32(h + 44);
    if (gBoot.NRegion == 0 || gBoot.NRegion > 8 ||
        96UL + gBoot.NRegion * 64UL > gFwSize) {
        Fwb(FWB_STEP_START, FWB_ERR_BAD_PATCH); return;
    }


    /* 0. bus master first, like pci_set_master() at the top of
     *    mt7921_pci_probe() (v0.07 did this after the WFSYS reset)   */
    if (!(gFwx & FWX_BM_LATE)) {
        EnableBusMaster();
        if (!(gBoot.PciCmd & 0x0004)) { Fwb(FWB_STEP_BUSMASTER, FWB_ERR_TIMEOUT); return; }
    }

    /* 1. fw own + driver own (mt7921_pci_probe) */
    if (OwnHandshake()) { Fwb(FWB_STEP_OWN, FWB_ERR_TIMEOUT); return; }

    /* 2. chip ID */
    gBoot.ChipId = L1Rd(HW_CHIPID);
    a.l = gBoot.ChipId;
    if (a.w.lo != 0x7961) { Fwb(FWB_STEP_CHIPID, FWB_ERR_CHIPID); return; }

    /* 3. WFSYS reset (mt792x_wfsys_reset_default) */
    /* v0.10: OFF by default. On this machine the WFDMA engine never
     * fetches a descriptor after this reset (v0.05-v0.09); without it
     * the MCU answers right away.                                    */
    if (gFwx & FWX_DO_WFSYS_RESET) {
    v = L1Rd(L1_WFSYS_SW_RST);
    L1Wr(L1_WFSYS_SW_RST, v & ~0x00000001UL);   /* clear SW_RST_B */
    Delay(200000UL);                            /* >= 50 ms       */
    v = L1Rd(L1_WFSYS_SW_RST);
    L1Wr(L1_WFSYS_SW_RST, v | 0x00000001UL);
    for (n = 1; n <= 500000UL; n++)
        if (L1Rd(L1_WFSYS_SW_RST) & 0x00000010UL)   /* INIT_DONE */
            break;
    gBoot.Polls = n;
    if (n > 500000UL) { Fwb(FWB_STEP_WFSYS, FWB_ERR_TIMEOUT); return; }
    }

    /* 4. bus master (only here with FWX_BM_LATE) */
    if (gFwx & FWX_BM_LATE) {
        EnableBusMaster();
        if (!(gBoot.PciCmd & 0x0004)) { Fwb(FWB_STEP_BUSMASTER, FWB_ERR_TIMEOUT); return; }
    }

    /* 5. WFDMA: disable, rings, enable (mt7921_dma_init); before it
     *    Linux writes HOST_INT_ENA = 0 and PCIE_MAC_INT_ENABLE = 0xff */
    Wr(REG_HOST_INT_ENA, 0);
    Wr(0x00010188UL, 0x000000FFUL);             /* PCIE_MAC_INT_ENABLE */
    DmaDisable();
    RingInitTx(&rTx0);
    Wr(REG_TX_EXT_CTRL, 0x00000004UL);
    RingInitTx(&rWm);
    RingInitTx(&rFwdl);
    RingInitRx(&rRx0);
    RingInitRx(&rRx4);
    RingInitRx(&rRx2);
    DmaEnable();
    gBoot.GloCfg = Rd(REG_GLO_CFG);

    /* 6. mcu init: normal mode, mt7921e_driver_own, L0s off */
    Wr(REG_SWDEF_MODE, 0);
    L1Wr(L1_TOP_LPCR_HOST_BAND0, 0x00000002UL);         /* DRV_OWN */
    for (n = 1; n <= 500000UL; n++)
        if (!(L1Rd(L1_TOP_LPCR_HOST_BAND0) & 0x00000001UL)) /* FW_OWN */
            break;
    gBoot.Polls = n;
    if (n > 500000UL) { Fwb(FWB_STEP_MCU_OWN, FWB_ERR_TIMEOUT); return; }
    Set(REG_PCIE_MAC_PM, 0x00000100UL);                 /* L0S_DIS */

    /* 7. patch semaphore. Status values (mt76_connac_mcu.h):
     *    0 NOT_DL_SEM_FAIL, 1 IS_DL, 2 NOT_DL_SEM_SUCCESS, 3 REL_SEM_SUCCESS
     *    (v0.05-v0.09 had these wrong)                                  */
    PcieErrClear();
    PutLe32(0, 1);                                      /* PATCH_SEM_GET */
    st = McuSend(MCU_CMD_PATCH_SEM, 4, 1);
    PcieErrRead();
    if (st == 0) {
        /* semaphore busy - most likely still held by an earlier run
         * that stopped half way: release it and try once more        */
        PutLe32(0, 0);
        (void)McuSend(MCU_CMD_PATCH_SEM, 4, 1);
        PutLe32(0, 1);
        st = McuSend(MCU_CMD_PATCH_SEM, 4, 1);
    }
    gBoot.SemGet = st;
    if (st == 0xFFFE) { Fwb(FWB_STEP_SEM_GET, FWB_ERR_TX_TIMEOUT); return; }
    if (st == 0xFFFF) { Fwb(FWB_STEP_SEM_GET, FWB_ERR_NO_EVENT); return; }
    if (st == 1)      { RamBoot(); return; }   /* PATCH_IS_DL: go on with RAM */
    if (st != 2)      { Fwb(FWB_STEP_SEM_GET, FWB_ERR_BAD_STATUS); return; }

    /* 8. regions: init download + FW_SCATTER */
    for (i = 0; i < (USHORT)gBoot.NRegion; i++) {
        ULONG offs, addr, len, key, mode;
        h = FwPtr(96UL + (ULONG)i * 64UL);
        if ((Be32(h) & 0xFFFFUL) != 2UL) { Fwb(FWB_STEP_DOWNLOAD, FWB_ERR_BAD_PATCH); goto release; }
        offs = Be32(h + 4);
        addr = Be32(h + 12);
        len  = Be32(h + 16);
        key  = Be32(h + 20);
        if (offs > gFwSize || len > gFwSize - offs) { Fwb(FWB_STEP_DOWNLOAD, FWB_ERR_BAD_PATCH); goto release; }

        mode = 0x80000000UL;                            /* DL_MODE_NEED_RSP */
        if (key != 0xFFFFFFFFUL) {
            a.l = key;
            switch (a.w.hi >> 8) {                      /* ENC_TYPE */
            case 1:                                     /* AES */
                mode |= 0x00000001UL | 0x00000008UL |
                        (((ULONG)(a.w.lo & 0xFF) << 1) & 0x00000006UL);
                break;
            case 2:                                     /* scramble */
                mode |= 0x00000001UL | 0x00000040UL | 0x00000008UL;
                break;
            }
        }

        PutLe32(0, addr);
        PutLe32(4, len);
        PutLe32(8, mode);
        st = McuSend((addr == 0x00200000UL || addr == 0x00900000UL) ?
                     MCU_CMD_PATCH_START : MCU_CMD_TARGET_ADDR_LEN, 12, 1);
        if (st >= 0xFFFE) {
            Fwb(FWB_STEP_DOWNLOAD, st == 0xFFFE ? FWB_ERR_TX_TIMEOUT : FWB_ERR_NO_EVENT);
            goto release;
        }

        while (len) {
            USHORT chunk = len > 4096UL ? 4096 : (USHORT)len;
            if (TxSend(&rFwdl, gFwPhys + offs, chunk)) {
                Fwb(FWB_STEP_DOWNLOAD, FWB_ERR_TX_TIMEOUT);
                goto release;
            }
            gBoot.ChunksSent++;
            offs += chunk;
            len  -= chunk;
        }
        gBoot.RegionsDone++;
    }

    /* 9. start the patch */
    PutLe32(0, 0);                                      /* check_crc = 0 */
    st = McuSend(MCU_CMD_PATCH_FINISH, 4, 1);
    gBoot.Finish = st;
    if (st != 0) {
        Fwb(FWB_STEP_FINISH, st >= 0xFFFE ? FWB_ERR_NO_EVENT : FWB_ERR_BAD_STATUS);
        goto release;
    }
    Fwb(FWB_STEP_SEM_REL, FWB_OK);

release:
    /* 10. always release the semaphore, like Linux does */
    PutLe32(0, 0);                                      /* PATCH_SEM_RELEASE */
    st = McuSend(MCU_CMD_PATCH_SEM, 4, 1);
    gBoot.SemRel = st;
    if (gBoot.Error == FWB_OK) {
        if (st == 3)                                    /* REL_SEM_SUCCESS */
            RamBoot();
        else
            Fwb(FWB_STEP_SEM_REL, st >= 0xFFFE ? FWB_ERR_NO_EVENT : FWB_ERR_BAD_STATUS);
    }
}


/* ------------------------------------------------------------------ */
/* Phase 4a: first scan (OID_PRIVATE_MT7921_SCAN)                      */
/* Same order as Linux: init_hardware (EFUSE_BUFFER_MODE), start       */
/* (MAC_INIT_CTRL), add_interface (DEV_INFO + BSS_INFO), hw_scan.      */
/* ------------------------------------------------------------------ */
#define MCU_EXT_CMD_EFUSE_BUFFER_MODE   0x21
#define MCU_EXT_CMD_MAC_INIT_CTRL       0x46
#define MCU_UNI_CMD_DEV_INFO_UPDATE     0x01
#define MCU_UNI_CMD_BSS_INFO_UPDATE     0x02
#define MCU_CE_CMD_START_HW_SCAN        0x03
#define MCU_EVENT_SCAN_DONE             0x0D
#define MCU_CE_CMD_SET_CHAN_DOMAIN      0x0F
#define MCU_EXT_CMD_SET_RX_PATH         0x4E
#define MCU_UNI_CMD_STA_REC_UPDATE      0x03
#define MCU_UNI_CMD_ROC                 0x27
#define MCU_UNI_EVENT_ROC               0x27

/* channel list for SET_CHAN_DOMAIN (country NL): hw_value, flags.
 * flags are mac80211 channel flags: NO_IR = 0x02, RADAR = 0x08      */
static const UCHAR gCh2g[] = { 1,2,3,4,5,6,7,8,9,10,11,12,13 };
static const UCHAR gCh5g[] = { 36,40,44,48, 52,56,60,64,
                               100,104,108,112,116,120,124,128,132,136,140 };

#define SCAN_LIMIT      8000000UL       /* polls, roughly 8+ seconds */

static MT7921_SCAN gScan;

static void PayloadZero(USHORT n)
{
    USHORT i;
    for (i = 0; i < n; i++) gPayload[i] = 0;
}

static ULONG EvtLe32(USHORT o)
{
    union { ULONG l; UCHAR b[4]; } u;
    u.b[0] = gEvt[o]; u.b[1] = gEvt[o + 1]; u.b[2] = gEvt[o + 2]; u.b[3] = gEvt[o + 3];
    return u.l;
}

static int Scn(USHORT step, USHORT err)
{
    gScan.Step = step;
    gScan.Error = err;
    return err;
}

static USHORT ScnErr(USHORT st)
{
    return st == 0xFFFE ? SCN_ERR_TX : SCN_ERR_NO_EVENT;
}

/* ---- BSS table (v0.17) -------------------------------------------- */
#define MAX_BSS     32
#define MAX_IES     400

typedef struct {
    UCHAR  bssid[6];
    UCHAR  ch;
    signed char rssi;               /* dBm                             */
    UCHAR  tsf[8];
    USHORT bint;                    /* beacon interval                 */
    USHORT cap;                     /* capability information          */
    USHORT ielen;
    UCHAR  ssidlen;
    char   ssid[32];
    UCHAR  ies[MAX_IES];            /* variable IEs (after fixed part) */
} BSSE;

static BSSE   gBss[MAX_BSS];
static USHORT gNBss;
static int    gScanReady;                       /* ScanPrep done       */
static int    gHwBusy;                          /* FwBoot / scan active */

/* ---- phase 5a: authentication test state ------------------------- */
static MT7921_AUTH gAuth;
static int    gAuthRun;                         /* record frames to us */
static UCHAR  gRocToken;

static int IsOurMac(UCHAR FAR *a)
{
    USHORT i;
    for (i = 0; i < 6; i++)
        if (a[i] != gMac[i]) return 0;
    return 1;
}

/* management frame addressed to us (auth, assoc resp, deauth ...) */
static void OwnFrame(UCHAR FAR *f, USHORT n)
{
    if (gAuth.RxFrames < 8)
        gAuth.Fc[gAuth.RxFrames] = f[0];
    gAuth.RxFrames++;
    if (f[0] == 0xB0 && n >= 30) {              /* authentication */
        gAuth.AuthAlg    = (USHORT)(f[24] | ((USHORT)f[25] << 8));
        gAuth.AuthSeq    = (USHORT)(f[26] | ((USHORT)f[27] << 8));
        gAuth.AuthStatus = (USHORT)(f[28] | ((USHORT)f[29] << 8));
    }
}

/* RX descriptor of a frame (mt7921_mac_fill_rx): 6 dwords, then the
 * optional groups 4 (16 bytes), 1 (16), 2 (8), 3 (8, RXV with RCPI),
 * 5 (96); then 2 * HDR_OFFSET pad bytes; then the 802.11 header.
 * rxd1 bits 11..15 = groups 1,2,3,4,5 (byte 5, bits 3..7)           */
static void ParseFrame(UCHAR FAR *b, USHORT len)
{
    USHORT hdr = 24, v1 = 0xFFFF, p, i, n;
    UCHAR g = b[5], rcpi;
    int rssi;
    BSSE *e = 0;

    if (len < 48) return;
    if (g & 0x40) hdr += 16;                    /* group 4 */
    if (g & 0x08) hdr += 16;                    /* group 1 */
    if (g & 0x10) hdr += 8;                     /* group 2 */
    if (g & 0x20) {                             /* group 3: RXV */
        v1 = hdr + 4;
        hdr += 8;
        if (g & 0x80) {                         /* group 5 */
            hdr += 24;
            v1 = hdr;
            hdr += 48;
        }
    }
    hdr += 2 * (b[9] >> 6);                     /* rxd2 HDR_OFFSET */
    if (hdr + 24 > len) return;

    /* v0.19: management frames to our own address */
    if (gAuthRun && (b[hdr] & 0x0C) == 0 && IsOurMac(b + hdr + 4)) {
        OwnFrame(b + hdr, len - hdr);
        return;
    }
    if (hdr + 36 > len) return;

    /* beacon (80 00) or probe response (50 00) */
    if ((b[hdr] != 0x80 && b[hdr] != 0x50) || b[hdr + 1] != 0)
        return;
    gScan.Beacons++;

    for (i = 0; i < gNBss; i++) {
        USHORT k;
        for (k = 0; k < 6 && gBss[i].bssid[k] == b[hdr + 16 + k]; k++) ;
        if (k == 6) { e = &gBss[i]; break; }
    }
    if (!e) {
        if (gNBss >= MAX_BSS) return;
        e = &gBss[gNBss++];
        for (i = 0; i < 6; i++) e->bssid[i] = b[hdr + 16 + i];
    }

    e->ch = b[13];                              /* rxd3 CH_FREQ    */
    rssi = -127;
    if (v1 != 0xFFFF) {
        rcpi = b[v1] > b[v1 + 1] ? b[v1] : b[v1 + 1];
        rssi = ((int)rcpi - 220) / 2;           /* to_rssi()       */
    }
    e->rssi = (signed char)rssi;
    for (i = 0; i < 8; i++) e->tsf[i] = b[hdr + 24 + i];
    e->bint = (USHORT)(b[hdr + 32] | ((USHORT)b[hdr + 33] << 8));
    e->cap  = (USHORT)(b[hdr + 34] | ((USHORT)b[hdr + 35] << 8));

    n = len - (hdr + 36);
    if (n > MAX_IES) n = MAX_IES;
    FarCopy((UCHAR FAR *)e->ies, b + hdr + 36, n);
    e->ielen = n;

    for (p = 0; p + 2 <= n; ) {
        UCHAR id = e->ies[p], l = e->ies[p + 1];
        if (p + 2 + l > n) break;
        if (id == 0) {
            e->ssidlen = l > 32 ? 32 : l;
            for (i = 0; i < e->ssidlen; i++) e->ssid[i] = (char)e->ies[p + 2 + i];
        } else if (id == 3 && l >= 1 && !e->ch) {
            e->ch = e->ies[p + 2];
        }
        p += 2 + l;
    }
}

/* one RX buffer from ring 0/4: event or (pkt type 7, flag 1) frame */
static void RxBuffer(RING *r, USHORT len)
{
    UCHAR FAR *ev = RX_BUF_PTR(r, r->tail);
    UCHAR type = (UCHAR)(ev[3] >> 3);
    if (len > RX_BUF) len = RX_BUF;
    if (type == 2 || (type == 7 && (ev[2] & 0x0F) == 1)) { /* NORMAL(_MCU) frame */
        if (gScan.Events == 0) {
            gScan.FirstLen = len;
            FarCopy((UCHAR FAR *)gScan.FirstFrame, ev, len < 64 ? len : 64);
        }
        gScan.Events++;
        ParseFrame(ev, len);
    } else if (gAuthRun && ev[28] == MCU_UNI_EVENT_ROC && len >= 50) {
        gAuth.RocGranted = 1;                   /* grant tlv at +40   */
        gAuth.RocStatus  = ev[46];
        gAuth.RocChannel = ev[47];
        gAuth.RocBand    = ev[49];
    } else if (ev[28] == MCU_EVENT_SCAN_DONE) {
        gScan.ScanDone = 1;
        FarCopy((UCHAR FAR *)gScan.DoneEvt, ev + 36, 48);
    } else {
        if (gScan.Frames < 8)                   /* reuse: other events */
            gScan.EvtIds[(USHORT)gScan.Frames] = ev[28];
        gScan.Frames++;
        if (gAuthRun) {
            if (gAuth.RxOther < 8)
                gAuth.EvtIds[gAuth.RxOther] = type == 7 ? ev[28] : (UCHAR)(0xF0 | type);
            gAuth.RxOther++;
        }
    }
    RxRelease(r);
}

/* drain all completed RX buffers of rings 0, 4 and 2; returns count  */
static USHORT RxDrain(void)
{
    static RING *rr[3] = { &rRx4, &rRx0, &rRx2 };
    USHORT got = 0;
    int k;
    U32W c;

    for (k = 0; k < 3; k++) {
        RING *r = rr[k];
        for (;;) {
            c.l = DmaRd(r->desc + r->tail * 16 + 4);
            if (!(c.w.hi & DESC_DMA_DONE_HI)) break;
            RxBuffer(r, (USHORT)(c.w.hi & 0x3FFF));
            got++;
        }
    }
    return got;
}

/* wait for SCAN_DONE; sleep 16 ms between polls so OS/2 keeps running */
static void ScanCollect(void)
{
    USHORT t;
    for (t = 0; t < 600 && !gScan.ScanDone; t++) {   /* ~10 s max */
        RxDrain();
        if (gScan.ScanDone) break;
        DH_Sleep(16);
    }
    RxDrain();
    gScan.Polls = t;
}

/* once per firmware start: EFUSE mode .. BSS_INFO (Linux init_hardware,
 * start and add_interface)                                           */
static void ScanPrep(void)
{
    USHORT st, i;

    /* 1. mt7921_mcu_set_eeprom: buffer_mode EFUSE, format WHOLE */
    PayloadZero(4);
    gPayload[1] = 1;                            /* EE_FORMAT_WHOLE  */
    gKind = MK_EXT; gExtCid = MCU_EXT_CMD_EFUSE_BUFFER_MODE;
    st = McuSend(0xED, 4, 1);
    gScan.StEfuse = st;
    if (st >= 0xFFFE) { Scn(SCN_STEP_EFUSE, ScnErr(st)); return; }

    /* 2. mt76_connac_mcu_set_mac_enable(band 0, enable) */
    PayloadZero(4);
    gPayload[0] = 1;
    gKind = MK_EXT; gExtCid = MCU_EXT_CMD_MAC_INIT_CTRL;
    st = McuSend(0xED, 4, 1);
    gScan.StMacEn = st;
    if (st >= 0xFFFE) { Scn(SCN_STEP_MAC_EN, ScnErr(st)); return; }

    /* 2a. SET_CHAN_DOMAIN: hdr { alpha2[4], bw_2g, bw_5g, bw_6g, pad,
     *     n_2ch, n_5ch, n_6ch, pad2 }, { le16 ch, le16 pad, le32 flags } */
    {
        USHORT p = 12, k;
        PayloadZero(12 + (sizeof(gCh2g) + sizeof(gCh5g)) * 8);
        gPayload[0] = 'N'; gPayload[1] = 'L';
        gPayload[5] = 3;
        gPayload[6] = 3;
        gPayload[8] = sizeof(gCh2g);
        gPayload[9] = sizeof(gCh5g);
        for (k = 0; k < sizeof(gCh2g); k++, p += 8)
            gPayload[p] = gCh2g[k];
        for (k = 0; k < sizeof(gCh5g); k++, p += 8) {
            gPayload[p] = gCh5g[k];
            if (gCh5g[k] >= 52)
                gPayload[p + 4] = 0x0A;         /* NO_IR | RADAR */
        }
        gSetQuery = MCU_Q_SET;
        st = McuSend(MCU_CE_CMD_SET_CHAN_DOMAIN, p, 0);
        gScan.StChanDom = st;
        if (st >= 0xFFFE) { Scn(SCN_STEP_CHAN_DOM, SCN_ERR_TX); return; }
    }

    /* 2b. SET_RX_PATH: channel 1, 20 MHz, 2 TX streams, RX mask 3 */
    PayloadZero(76);
    gPayload[0] = 1; gPayload[1] = 1; gPayload[3] = 2; gPayload[4] = 3;
    gKind = MK_EXT; gExtCid = MCU_EXT_CMD_SET_RX_PATH;
    st = McuSend(0xED, 76, 1);
    gScan.StRxPath = st;
    if (st >= 0xFFFE) { Scn(SCN_STEP_RX_PATH, ScnErr(st)); return; }

    /* 3. DEV_INFO_UPDATE */
    PayloadZero(16);
    gPayload[6] = 12;                           /* tlv len    */
    gPayload[8] = 1;                            /* active     */
    for (i = 0; i < 6; i++) gPayload[10 + i] = gMac[i];
    gKind = MK_UNI;
    st = McuSend(MCU_UNI_CMD_DEV_INFO_UPDATE, 16, 1);
    if (st >= 0xFFFE) { Scn(SCN_STEP_DEV_INFO, ScnErr(st)); return; }
    gScan.DevUni = gEvtLen >= 44 ? EvtLe32(40) : 0xFFFFFFFEUL;

    /* 4. BSS_INFO_UPDATE (basic, station) */
    PayloadZero(36);
    gPayload[6] = 32;
    gPayload[8] = 1;
    gPayload[12] = 0x01; gPayload[14] = 0x01;   /* CONNECTION_INFRA_STA */
    gPayload[16] = 1;
    gKind = MK_UNI;
    st = McuSend(MCU_UNI_CMD_BSS_INFO_UPDATE, 36, 1);
    if (st >= 0xFFFE) { Scn(SCN_STEP_BSS_INFO, ScnErr(st)); return; }
    gScan.BssUni = gEvtLen >= 44 ? EvtLe32(40) : 0xFFFFFFFEUL;

    gScanReady = 1;
}

/* hw_scan + collect into gBss[] */
static void ScanRun(void)
{
    USHORT st, i;
    static UCHAR seq;

    gNBss = 0;
    RxDrain();                                  /* old events away */

    PayloadZero(1186);
    seq = (UCHAR)((seq + 1) & 0x7F);
    if (!seq) seq = 1;
    gPayload[0] = seq;                          /* seq_num, band 0    */
    gPayload[3] = 0x01;                         /* wildcard SSID      */
    gPayload[6] = 0x20;                         /* SCAN_FUNC_SPLIT_SCAN */
    gPayload[7] = 1;                            /* version            */
    for (i = 0; i < 6; i++) gPayload[1110 + i] = 0xFF;
    gSetQuery = MCU_Q_SET;
    st = McuSend(MCU_CE_CMD_START_HW_SCAN, 1186, 0);
    gScan.StScan = st;
    if (st >= 0xFFFE) { Scn(SCN_STEP_HW_SCAN, SCN_ERR_TX); return; }

    ScanCollect();
    if (!gScan.ScanDone) { Scn(SCN_STEP_COLLECT, SCN_ERR_NO_DONE); return; }
    Scn(SCN_STEP_DONE, SCN_OK);
}

static void ScanReset(void)
{
    FarZero((UCHAR FAR *)&gScan, sizeof(gScan));
    gScan.Magic = MT7921_SCAN_MAGIC;
    gScan.StEfuse = gScan.StMacEn = gScan.StScan = 0xFFFF;
    gScan.StChanDom = gScan.StRxPath = 0xFFFF;
    gScan.DevUni = gScan.BssUni = 0xFFFFFFFFUL;
}

/* full path used by BSSID_LIST_SCAN and /scan: firmware, prep, scan */
static void DoScan(void)
{
    USHORT i, k;

    ScanReset();
    if (!gFwRunning) {
        gFwx = 0;
        FwBoot();
    }
    if (!gFwRunning) { Scn(SCN_STEP_START, SCN_ERR_NO_FW); return; }
    if (!gScanReady) {
        ScanPrep();
        if (!gScanReady) return;
    } else {
        gScan.StEfuse = gScan.StMacEn = gScan.StRxPath = 0;  /* done before */
        gScan.StChanDom = 0;
        gScan.DevUni = gScan.BssUni = 0;
    }
    ScanRun();

    /* summary for wrndtest /scan */
    gScan.NBss = gNBss;
    for (i = 0; i < gNBss && i < SCAN_MAX_BSS; i++) {
        MT7921_BSS *d = &gScan.Bss[i];
        for (k = 0; k < 6; k++) d->Bssid[k] = gBss[i].bssid[k];
        d->Channel = gBss[i].ch;
        d->SsidLen = gBss[i].ssidlen;
        for (k = 0; k < 32; k++) d->Ssid[k] = gBss[i].ssid[k];
        gScan.Rssi[i] = gBss[i].rssi;
    }
}

/* ------------------------------------------------------------------ */
/* Phase 5a: authentication test (OID_PRIVATE_MT7921_AUTH)             */
/* Linux order (mac80211 + mt7921): sta_add -> STA_REC_UPDATE for the  */
/* AP (state NONE), mgd_prepare_tx -> ROC JOIN on the AP channel, then */
/* the auth frame goes out on the data ring with a connac2 TXD + TXP.  */
/* ------------------------------------------------------------------ */
#define OFS_TXF     0x1800      /* TXD + TXP + 802.11 frame (in CMD area) */
#define AP_WCID     1

static int Aut(USHORT step, USHORT err)
{
    gAuth.Step = step;
    gAuth.Error = err;
    return err;
}

static void PutLe16(USHORT at, USHORT v)
{
    gPayload[at] = (UCHAR)v;
    gPayload[at + 1] = (UCHAR)(v >> 8);
}

/* strongest BSS with the requested SSID */
static BSSE *AuthPick(void)
{
    BSSE *best = 0;
    USHORT i, k;
    for (i = 0; i < gNBss; i++) {
        BSSE *e = &gBss[i];
        if (e->ssidlen != gAuth.SsidLen) continue;
        for (k = 0; k < e->ssidlen && e->ssid[k] == gAuth.Ssid[k]; k++) ;
        if (k != e->ssidlen) continue;
        if (!best || e->rssi > best->rssi) best = e;
    }
    return best;
}

/* AP advertises WMM (vendor IE 00:50:F2 type 2) */
static UCHAR HasWmm(BSSE *e)
{
    USHORT p;
    for (p = 0; p + 2 <= e->ielen; p += 2 + e->ies[p + 1]) {
        UCHAR FAR *ie = (UCHAR FAR *)e->ies + p;
        if (p + 2 + ie[1] > e->ielen) break;
        if (ie[0] == 221 && ie[1] >= 5 && ie[2] == 0x00 && ie[3] == 0x50 &&
            ie[4] == 0xF2 && ie[5] == 2)
            return 1;
    }
    return 0;
}

/* mt7921_mcu_sta_update(sta = AP, enable, STATE_NONE): sta_req_hdr +
 * BASIC, PHY, RA, STATE, WTBL{wtbl_req_hdr, GENERIC, RX, HDR_TRANS}  */
static USHORT StaRec(BSSE *e)
{
    USHORT i, p;
    UCHAR g5 = e->ch > 14;

    PayloadZero(120);
    gPayload[0] = 0;                            /* bss_idx          */
    gPayload[1] = AP_WCID;                      /* wlan_idx_lo      */
    PutLe16(2, 5);                              /* tlv_num          */
    gPayload[4] = 1;                            /* is_tlv_append    */
    gPayload[5] = 0;                            /* muar_idx = omac  */

    p = 8;                                      /* STA_REC_BASIC    */
    PutLe16(p, 0); PutLe16(p + 2, 20);
    PutLe32(p + 4, 0x00010002UL);               /* CONNECTION_INFRA_AP */
    gPayload[p + 8] = 2;                        /* CONN_STATE_PORT_SECURE */
    gPayload[p + 9] = HasWmm(e);                /* qos              */
    for (i = 0; i < 6; i++) gPayload[p + 12 + i] = e->bssid[i];
    PutLe16(p + 18, 3);                         /* EXTRA_INFO_VER|NEW */

    p += 20;                                    /* STA_REC_PHY      */
    PutLe16(p, 0x15); PutLe16(p + 2, 12);
    PutLe16(p + 4, g5 ? 0x0015 : 0x000F);       /* basic rates      */
    gPayload[p + 6] = g5 ? 0x08 : 0x03;         /* OFDM | HR_DSSS + ERP */

    p += 12;                                    /* STA_REC_RA       */
    PutLe16(p, 1); PutLe16(p + 2, 16);
    PutLe16(p + 4, g5 ? 0x3FC0 : 0x3FCF);       /* legacy OFDM | CCK */

    p += 16;                                    /* STA_REC_STATE    */
    PutLe16(p, 7); PutLe16(p + 2, 12);          /* state 0 = NONE   */

    p += 12;                                    /* STA_REC_WTBL     */
    PutLe16(p, 13); PutLe16(p + 2, 52);
    gPayload[p + 4] = AP_WCID;                  /* wtbl_req_hdr     */
    gPayload[p + 5] = 1;                        /* WTBL_RESET_AND_SET */
    PutLe16(p + 6, 3);                          /* nested tlv_num   */
    p += 12;                                    /* WTBL_GENERIC     */
    PutLe16(p, 0); PutLe16(p + 2, 20);
    for (i = 0; i < 6; i++) gPayload[p + 4 + i] = e->bssid[i];
    gPayload[p + 13] = HasWmm(e);               /* qos              */
    p += 20;                                    /* WTBL_RX          */
    PutLe16(p, 1); PutLe16(p + 2, 12);
    gPayload[p + 5] = 1; gPayload[p + 6] = 1; gPayload[p + 7] = 1;
    p += 12;                                    /* WTBL_HDR_TRANS   */
    PutLe16(p, 6); PutLe16(p + 2, 8);
    gPayload[p + 4] = 1;                        /* to_ds            */
    gPayload[p + 6] = 1;                        /* no_rx_trans      */
    p += 8;                                     /* = 120            */

    gKind = MK_UNI;
    return McuSend(MCU_UNI_CMD_STA_REC_UPDATE, p, 1);
}

/* UNI ROC acquire (JOIN) / abort */
static USHORT RocCmd(UCHAR acquire, UCHAR ch, USHORT ms)
{
    PayloadZero(28);
    if (acquire) {
        PutLe16(4, 0); PutLe16(6, 24);          /* UNI_ROC_ACQUIRE  */
        gPayload[9]  = gRocToken;
        gPayload[10] = ch;                      /* control channel  */
        gPayload[12] = ch > 14 ? 2 : 1;         /* band             */
        gPayload[14] = ch;                      /* center_chan      */
        gPayload[17] = ch;                      /* center_chan_from_ap */
        gPayload[19] = 0;                       /* MT7921_ROC_REQ_JOIN */
        PutLe32(20, ms);
        gPayload[24] = 0xFF;                    /* dbdcband auto    */
        gKind = MK_UNI;
        return McuSend(MCU_UNI_CMD_ROC, 28, 0);
    }
    PutLe16(4, 1); PutLe16(6, 12);              /* UNI_ROC_ABORT    */
    gPayload[9]  = gRocToken;
    gPayload[10] = 0xFF;
    gKind = MK_UNI;
    return McuSend(MCU_UNI_CMD_ROC, 16, 0);
}

static void PutFarLe32(UCHAR FAR *b, ULONG v)
{
    union { ULONG l; UCHAR c[4]; } u;
    u.l = v;
    b[0] = u.c[0]; b[1] = u.c[1]; b[2] = u.c[2]; b[3] = u.c[3];
}

/* connac2 TXD (32) + hw TXP (32) + frame, queued on TX ring 0 */
static int TxMgmt(const UCHAR *f, USHORT n, UCHAR ch)
{
    UCHAR FAR *b = (UCHAR FAR *)MkFar(gDmaSel, OFS_TXF);
    UCHAR stype = (UCHAR)(f[0] >> 4);
    USHORT i;

    FarZero(b, 64);
    /* dw0: TX_BYTES, PKT_FMT CT (0), Q_IDX ALTX0 (0x10) */
    PutFarLe32(b + 0,  (ULONG)(n + 32) | 0x20000000UL);
    /* dw1: LONG_FORMAT, HDR_FORMAT 802.11, HDR_INFO 24/2, WLAN_IDX */
    PutFarLe32(b + 4,  0x80020000UL | (12UL << 11) | AP_WCID);
    /* dw2: FIX_RATE, HTC_VLD, frame type mgmt, sub type */
    PutFarLe32(b + 8,  0x80002000UL | stype);
    /* dw3: BA_DISABLE, REM_TX_COUNT 15 */
    PutFarLe32(b + 12, 0x10000000UL | (15UL << 11));
    /* dw6: FIXED_BW, rate CCK 1M (2.4 GHz) or OFDM 6M (5 GHz) */
    PutFarLe32(b + 24, 0x00000004UL | ((ch > 14 ? 0x4BUL : 0x00UL) << 16));
    /* dw7: SPE_IDX 24, sub type, type mgmt */
    PutFarLe32(b + 28, (24UL << 11) | ((ULONG)stype << 16));
    /* hw TXP: msdu_id[0] = token 1 | VALID, ptr[0] = frame, LAST */
    b[32] = 0x01; b[33] = 0x80;
    PutFarLe32(b + 40, gDmaPhys + OFS_TXF + 64);
    b[44] = (UCHAR)n; b[45] = (UCHAR)(0x80 | (n >> 8));
    for (i = 0; i < n; i++) b[64 + i] = f[i];

    return TxSend(&rTx0, gDmaPhys + OFS_TXF, 64);
}

static void DoAuth(void)
{
    static UCHAR fr[30];
    BSSE *e;
    USHORT st, t, i;

    gAuth.Magic = MT7921_AUTH_MAGIC;
    gAuth.AuthStatus = 0xFFFF;
    gAuth.StSta = gAuth.StRoc = 0xFFFF;
    gAuth.StaUni = 0xFFFFFFFFUL;
    Aut(AUT_STEP_START, AUT_OK);

    /* 1. scan when the table has no match yet */
    e = AuthPick();
    if (!e) {
        DoScan();
        if (!gFwRunning) { Aut(AUT_STEP_SCAN, AUT_ERR_NO_FW); return; }
        e = AuthPick();
    }
    if (!e) { Aut(AUT_STEP_SCAN, AUT_ERR_NO_BSS); return; }
    for (i = 0; i < 6; i++) gAuth.Bssid[i] = e->bssid[i];
    gAuth.Channel = e->ch;
    gAuth.Rssi = e->rssi;

    gAuthRun = 1;
    RxDrain();

    /* 2. STA record for the AP */
    st = StaRec(e);
    gAuth.StSta = st;
    if (st >= 0xFFFE) { gAuthRun = 0; Aut(AUT_STEP_STA_REC, st == 0xFFFE ? AUT_ERR_TX : AUT_ERR_NO_EVENT); return; }
    gAuth.StaUni = gEvtLen >= 44 ? EvtLe32(40) : 0xFFFFFFFEUL;

    /* 3. ROC JOIN, wait up to ~1 s for the grant */
    gRocToken++;
    st = RocCmd(1, e->ch, 1500);
    gAuth.StRoc = st;
    if (st >= 0xFFFE) { gAuthRun = 0; Aut(AUT_STEP_ROC, AUT_ERR_TX); return; }
    for (t = 0; t < 60 && !gAuth.RocGranted; t++) {
        RxDrain();
        if (gAuth.RocGranted) break;
        DH_Sleep(16);
    }
    if (!gAuth.RocGranted) {
        RocCmd(0, 0, 0);
        gAuthRun = 0;
        Aut(AUT_STEP_ROC, AUT_ERR_NO_GRANT);
        return;
    }

    /* 4. Open System authentication, transaction 1 */
    for (i = 0; i < 30; i++) fr[i] = 0;
    fr[0] = 0xB0;
    for (i = 0; i < 6; i++) {
        fr[4 + i]  = e->bssid[i];               /* addr1 = AP       */
        fr[10 + i] = gMac[i];                   /* addr2 = us       */
        fr[16 + i] = e->bssid[i];               /* addr3 = BSSID    */
    }
    fr[26] = 1;                                 /* transaction seq 1 */
    if (TxMgmt(fr, 30, e->ch)) {
        RocCmd(0, 0, 0);
        gAuthRun = 0;
        Aut(AUT_STEP_TX, AUT_ERR_TX);
        return;
    }
    gAuth.TxDone = 1;

    /* 5. wait up to ~1 s for the answer */
    for (t = 0; t < 60 && gAuth.AuthStatus == 0xFFFF; t++) {
        RxDrain();
        if (gAuth.AuthStatus != 0xFFFF) break;
        DH_Sleep(16);
    }
    gAuth.Polls = t;
    RocCmd(0, 0, 0);
    RxDrain();
    gAuthRun = 0;

    if (gAuth.AuthStatus == 0xFFFF) { Aut(AUT_STEP_WAIT, AUT_ERR_NO_RESP); return; }
    if (gAuth.AuthStatus != 0)      { Aut(AUT_STEP_WAIT, AUT_ERR_STATUS); return; }
    Aut(AUT_STEP_DONE, AUT_OK);
}

/* ---- OID_802_11_BSSID_LIST: NDIS_802_11_BSSID_LIST_EX --------------- */
/* NDIS_WLAN_BSSID_EX as driver_genmac.c sees it (116 bytes + IEs):
 *   0 Length, 4 MacAddress[6], 10 Reserved[2], 12 Ssid{len, [32]},
 *  48 Privacy, 52 Rssi, 56 NetworkTypeInUse, 60 Configuration{Length,
 *  BeaconPeriod, ATIMWindow, DSConfig (kHz), FH{4 x ULONG}},
 *  92 InfrastructureMode, 96 SupportedRates[16], 112 IELength,
 * 116 IEs = FIXED_IEs{Timestamp[8], BeaconInterval, Capabilities}+IEs */
static void FarLe32(UCHAR FAR *p, ULONG v)
{
    union { ULONG l; UCHAR b[4]; } u;
    u.l = v;
    p[0] = u.b[0]; p[1] = u.b[1]; p[2] = u.b[2]; p[3] = u.b[3];
}

static ULONG ChanKHz(UCHAR ch)
{
    ULONG mhz;
    if (ch == 14)      mhz = 2484;
    else if (ch < 14)  mhz = 2407 + 5 * (USHORT)ch;
    else               mhz = 5000 + 5 * (USHORT)ch;
    /* * 1000 without __U4M: 1024x - 16x - 8x */
    return (mhz << 10) - (mhz << 4) - (mhz << 3);
}

static ULONG BuildBssList(UCHAR FAR *d, ULONG len, ULONG *done)
{
    USHORT pos = 4, i, k, cnt = 0;

    if (len < 4) { *done = 4; return NDIS_STATUS_BUFFER_TOO_SHORT; }
    for (i = 0; i < gNBss; i++) {
        BSSE *e = &gBss[i];
        USHORT need = (USHORT)((116 + 12 + e->ielen + 3) & ~3);
        USHORT nr = 0, p;
        UCHAR FAR *x;

        if ((ULONG)pos + need > len) break;
        x = d + pos;
        FarZero(x, need);
        FarLe32(x + 0, need);
        for (k = 0; k < 6; k++) x[4 + k] = e->bssid[k];
        FarLe32(x + 12, e->ssidlen);
        for (k = 0; k < e->ssidlen; k++) x[16 + k] = (UCHAR)e->ssid[k];
        FarLe32(x + 48, (e->cap & 0x0010) ? 1 : 0);         /* privacy */
        FarLe32(x + 52, (ULONG)(long)e->rssi);
        FarLe32(x + 56, e->ch > 14 ? 2 : 3);    /* Ndis802_11OFDM5 / OFDM24 */
        FarLe32(x + 60, 32);                    /* Configuration.Length */
        FarLe32(x + 64, e->bint);
        FarLe32(x + 72, ChanKHz(e->ch));
        FarLe32(x + 76, 16);                    /* FHConfig.Length      */
        FarLe32(x + 92, (e->cap & 0x0002) ? 0 : 1);         /* IBSS : ESS */
        for (p = 0; p + 2 <= e->ielen; p += 2 + e->ies[p + 1]) {
            UCHAR id = e->ies[p], l = e->ies[p + 1];
            if (p + 2 + l > e->ielen) break;
            if (id == 1 || id == 50)            /* (extended) rates */
                for (k = 0; k < l && nr < 16; k++)
                    x[96 + nr++] = e->ies[p + 2 + k];
        }
        FarLe32(x + 112, 12 + e->ielen);
        for (k = 0; k < 8; k++) x[116 + k] = e->tsf[k];
        x[124] = (UCHAR)e->bint; x[125] = (UCHAR)(e->bint >> 8);
        x[126] = (UCHAR)e->cap;  x[127] = (UCHAR)(e->cap >> 8);
        FarCopy(x + 128, (UCHAR FAR *)e->ies, e->ielen);
        pos += need;
        cnt++;
    }
    FarLe32(d, cnt);
    *done = pos;
    return NDIS_STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* OID handling                                                        */
/* ------------------------------------------------------------------ */
static ULONG PutBytes(UCHAR FAR *data, ULONG len, const UCHAR FAR *src,
                      USHORT n, ULONG *done)
{
    if (len < n) { *done = n; return NDIS_STATUS_BUFFER_TOO_SHORT; }
    FarCopy(data, src, n);
    *done = n;
    return NDIS_STATUS_SUCCESS;
}

static ULONG PutU32(UCHAR FAR *data, ULONG len, ULONG val, ULONG *done)
{
    static ULONG v;
    v = val;
    return PutBytes(data, len, (UCHAR FAR *)&v, 4, done);
}

static ULONG GetU32(UCHAR FAR *data, ULONG len, ULONG *dst, ULONG *done)
{
    if (len < 4) return NDIS_STATUS_INVALID_LENGTH;
    *dst = *(ULONG FAR *)data;
    *done = 4;
    return NDIS_STATUS_SUCCESS;
}

/* NDIS_802_11_CAPABILITY v2: Length, Version, NoOfPMKIDs, NoOfPairs,
 * then { AuthMode, EncryptStatus } pairs                             */
static const UCHAR gCapPairs[] = {
    0, 1,       /* Open    / Disabled              */
    7, 6,       /* WPA2PSK / Encryption3 (CCMP)    */
    4, 6,       /* WPAPSK  / Encryption3           */
    7, 4,       /* WPA2PSK / Encryption2 (TKIP)    */
    4, 4        /* WPAPSK  / Encryption2           */
};

static ULONG PutCapability(UCHAR FAR *data, ULONG len, ULONG *done)
{
    USHORT n = sizeof(gCapPairs) / 2, need = 16 + n * 8, i;
    if (len < need) { *done = need; return NDIS_STATUS_BUFFER_TOO_SHORT; }
    FarZero(data, need);
    FarLe32(data, need);
    FarLe32(data + 4, 2);
    FarLe32(data + 8, 0);                       /* no PMKID cache yet */
    FarLe32(data + 12, n);
    for (i = 0; i < n; i++) {
        data[16 + i * 8] = gCapPairs[i * 2];
        data[20 + i * 8] = gCapPairs[i * 2 + 1];
    }
    *done = need;
    return NDIS_STATUS_SUCCESS;
}

static ULONG OidGet(ULONG oid, UCHAR FAR *data, ULONG len, ULONG *done)
{
    *done = 0;
    switch (oid) {
    case OID_PRIVATE_WRAPPER_ISWIRELESS:
        return PutU32(data, len, 1, done);
    case OID_PRIVATE_WRAPPER_GENMAC_VERSION:
        return PutU32(data, len, WRND_MIN_VERSION, done);
    case OID_PRIVATE_WRAPPER_LANNUMBER:
        return PutU32(data, len, gLanNumber, done);
    case OID_PRIVATE_WRAPPER_802_1X_READY:
        return PutU32(data, len, 0, done);      /* no EAPOL frames queued */
    case OID_PRIVATE_MT7921_INFO:
        return PutBytes(data, len, (UCHAR FAR *)&gInfo, sizeof(gInfo), done);
    case OID_PRIVATE_MT7921_SCAN:
        if (len < sizeof(gScan)) { *done = sizeof(gScan); return NDIS_STATUS_BUFFER_TOO_SHORT; }
        if (gHwBusy) return NDIS_STATUS_RESOURCES;
        gHwBusy = 1;
        DoScan();
        gHwBusy = 0;
        return PutBytes(data, len, (UCHAR FAR *)&gScan, sizeof(gScan), done);
    case OID_PRIVATE_MT7921_FWBOOT:
        if (len < sizeof(gBoot)) { *done = sizeof(gBoot); return NDIS_STATUS_BUFFER_TOO_SHORT; }
        if (gHwBusy) return NDIS_STATUS_RESOURCES;
        gFwx = *(USHORT FAR *)data;             /* flags come in Magic */
        gHwBusy = 1;
        FwBoot();
        gHwBusy = 0;
        return PutBytes(data, len, (UCHAR FAR *)&gBoot, sizeof(gBoot), done);
    case OID_PRIVATE_MT7921_HWPROBE:
        if (len < sizeof(gProbe)) { *done = sizeof(gProbe); return NDIS_STATUS_BUFFER_TOO_SHORT; }
        HwProbe();
        return PutBytes(data, len, (UCHAR FAR *)&gProbe, sizeof(gProbe), done);

    case OID_PRIVATE_MT7921_AUTH:
        if (len < sizeof(gAuth)) { *done = sizeof(gAuth); return NDIS_STATUS_BUFFER_TOO_SHORT; }
        if (gHwBusy) return NDIS_STATUS_RESOURCES;
        {
            MT7921_AUTH FAR *in = (MT7921_AUTH FAR *)data;
            USHORT k;
            FarZero((UCHAR FAR *)&gAuth, sizeof(gAuth));
            gAuth.SsidLen = in->SsidLen > 32 ? 32 : in->SsidLen;
            for (k = 0; k < 32; k++) gAuth.Ssid[k] = in->Ssid[k];
        }
        gHwBusy = 1;
        DoAuth();
        gHwBusy = 0;
        return PutBytes(data, len, (UCHAR FAR *)&gAuth, sizeof(gAuth), done);

    case OID_802_3_CURRENT_ADDRESS:
        /* v0.19: the real MAC is known only once the firmware runs */
        if (!gFwRunning && !gHwBusy) {
            gHwBusy = 1;
            gFwx = 0;
            FwBoot();
            gHwBusy = 0;
        }
        return PutBytes(data, len, (UCHAR FAR *)gMac, 6, done);
    case OID_802_11_CAPABILITY:                 /* v0.19 */
        return PutCapability(data, len, done);
    case OID_802_11_AUTHENTICATION_MODE:
        return PutU32(data, len, gAuthMode, done);
    case OID_802_11_ENCRYPTION_STATUS:         /* no key yet: KeyAbsent */
        return PutU32(data, len, gEncStatus == 0 ? 2 : gEncStatus == 4 ? 5 :
                      gEncStatus == 6 ? 7 : gEncStatus, done);
    case OID_802_11_INFRASTRUCTURE_MODE:
        return PutU32(data, len, gInfraMode, done);
    case OID_802_11_SSID:                       /* not associated: empty */
        if (len < 36) { *done = 36; return NDIS_STATUS_BUFFER_TOO_SHORT; }
        FarZero(data, 36);
        *done = 36;
        return NDIS_STATUS_SUCCESS;
    case OID_802_11_BSSID_LIST:                 /* v0.17: real results */
        return BuildBssList(data, len, done);

    case OID_802_11_BSSID:      /* failing = "disconnected" for the poll */
    default:
        return NDIS_STATUS_NOT_SUPPORTED;
    }
}

static ULONG OidSet(ULONG oid, UCHAR FAR *data, ULONG len, ULONG *done)
{
    *done = 0;
    switch (oid) {
    case OID_PRIVATE_WRAPPER_802_1X_STATE:
        return GetU32(data, len, &g8021xState, done);
    case OID_802_11_BSSID_LIST_SCAN:            /* v0.17: scan now (blocks
                                                   ~1-2 s, sleeping)     */
        if (gHwBusy) return NDIS_STATUS_RESOURCES;
        gHwBusy = 1;
        DoScan();
        gHwBusy = 0;
        *done = len;
        return gScan.Error == SCN_OK ? NDIS_STATUS_SUCCESS : NDIS_STATUS_FAILURE;
    case OID_802_11_AUTHENTICATION_MODE:
        return GetU32(data, len, &gAuthMode, done);
    case OID_802_11_ENCRYPTION_STATUS:
        return GetU32(data, len, &gEncStatus, done);
    case OID_802_11_INFRASTRUCTURE_MODE:
        return GetU32(data, len, &gInfraMode, done);

    /* accepted, but nothing happens yet (no radio in phase 1) */
    case OID_GEN_CURRENT_PACKET_FILTER:
    case OID_802_3_MULTICAST_LIST:
    case OID_802_11_SSID:
    case OID_802_11_BSSID:
    case OID_802_11_DISASSOCIATE:
    case OID_802_11_PRIVACY_FILTER:
    case OID_802_11_ADD_WEP:
    case OID_802_11_REMOVE_WEP:
    case OID_802_11_ADD_KEY:
    case OID_802_11_REMOVE_KEY:
    case OID_802_11_PMKID:
    case OID_PRIVATE_WRAPPER_802_1X_TX:         /* dropped */
        *done = len;
        return NDIS_STATUS_SUCCESS;

    default:
        return NDIS_STATUS_NOT_SUPPORTED;
    }
}

static USHORT DoIoctl(RP_IOCTL FAR *rp)
{
    static ULONG done;          /* static: its address is passed (SS != DS) */
    ULONG FAR *parm;
    UCHAR FAR *data;
    ULONG oid, len, status;
    int set;

    if (rp->Category != WRND_CATEGORY)
        return STDON | STERR | ERR_BAD_COMMAND;
    if (rp->Function == WRND_FUNC_OID_GET)
        set = 0;
    else if (rp->Function == WRND_FUNC_OID_SET)
        set = 1;
    else
        return STDON | STERR | ERR_BAD_COMMAND;

    if (VerifyFar(rp->ParmPacket, WRND_PARM_COUNT * 4, 1))
        return STDON | STERR | ERR_INVALID_PARM;
    parm = (ULONG FAR *)rp->ParmPacket;

    oid = parm[0];
    len = parm[1];
    /* v0.18: wpa_supplicant's IsWireless() reuses the parameter block
     * for three requests in a row, so parm[1] then holds the status of
     * the previous call (0), not a length. Use the larger of parm[1]
     * and the real IOCtl data length (RX passes DataLen 4 with
     * parm[1] = 2000, so parm[1] alone is needed there too).         */
    if ((ULONG)rp->DataLen > len)
        len = rp->DataLen;
    if (len > WRND_MAX_DATA)
        len = WRND_MAX_DATA;

    /* Pitfall: the IOCtl DataLen is often just 4, the real buffer size
     * is in parm[1]. Verify that size, not rp->DataLen.              */
    data = (UCHAR FAR *)rp->DataPacket;
    if (len && VerifyFar(data, (USHORT)len, set ? 0 : 1))
        return STDON | STERR | ERR_INVALID_PARM;

    status = set ? OidSet(oid, data, len, &done)
                 : OidGet(oid, data, len, &done);

    parm[1] = status;
    parm[2] = (status == NDIS_STATUS_SUCCESS) ? done : 0;
    parm[3] = (status == NDIS_STATUS_BUFFER_TOO_SHORT) ? done : 0;
    return STDON;
}

/* ------------------------------------------------------------------ */
/* Strategy                                                            */
/* ------------------------------------------------------------------ */
void StrategyC(RPH FAR *rp)
{
    switch (rp->Cmd) {
    case CMD_INIT:
        rp->Status = DoInit(rp);
        break;
    case CMD_OPEN:
    case CMD_CLOSE:
    case CMD_SHUTDOWN:
        rp->Status = STDON;
        break;
    case CMD_GENIOCTL:
        rp->Status = DoIoctl((RP_IOCTL FAR *)rp);
        break;
    default:
        rp->Status = STDON | STERR | ERR_BAD_COMMAND;
        break;
    }
}
