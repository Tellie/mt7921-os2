/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * mt.h - the MediaTek MT7921 back end (wlan/hw.h) of the OS/2 WLAN driver.
 *
 * The chip side follows Linux mt76 (drivers/net/wireless/mediatek/mt76: mt7921/, mt792x_*, mt76_connac*),
 * which is ISC licensed; it was first written as the stand-alone MT7921.OS2 (v0.01-v0.20, see old/).
 * The OS/2 side (os2/, wlan/) is Paul Smedley's, from github.com/psmedley/iwlwifi (GPL-2.0-only).
 *
 * Register offsets are BAR0 offsets after the mt76 fixed_map (BAR0 is 1 MB, mapped as 16 windows by bar_map).
 */
#ifndef MT_H
#define MT_H

/* ---- registers -------------------------------------------------------------------------------------- */
#define REG_CONN_ON_LPCTL       0x000E0010UL    /* 0x7C060010: SET_OWN 1, CLR_OWN 2, OWN_SYNC 4        */
#define REG_CONN_ON_MISC        0x000E00F0UL    /* 0x7C0600F0: bits 0+1 = N9 firmware ready             */
#define REG_HIF_REMAP_L1        0x000FE24CUL
#define REG_REMAP_BASE_L1       0x00040000UL
#define REG_WFDMA0_RST          0x000D4100UL
#define REG_MCU_CMD             0x000D41F0UL    /* MCU2HOST_SW_INT_STA                                  */
#define REG_MCU2HOST_SW_INT_ENA 0x000D41F4UL
#define REG_HOST_INT_STA        0x000D4200UL
#define REG_HOST_INT_ENA        0x000D4204UL
#define REG_GLO_CFG             0x000D4208UL
#define REG_RST_DTX_PTR         0x000D420CUL
#define REG_GLO_CFG_EXT0        0x000D42B0UL
#define REG_PRI_DLY_INT_CFG0    0x000D42F0UL
#define REG_TX_RING_BASE        0x000D4300UL    /* + 0x10 * ring                                         */
#define REG_RX_RING_BASE        0x000D4500UL
#define REG_TX_EXT_CTRL         0x000D4600UL    /* + 4 * ring                                            */
#define REG_RX_EXT_CTRL         0x000D4680UL
#define REG_DMASHDL_SW_CONTROL  0x000D6004UL
#define REG_WFDMA_DUMMY_CR      0x00002120UL
#define REG_SWDEF_MODE          0x0009F23CUL
#define REG_PCIE_MAC_INT_ENABLE 0x00010188UL
#define REG_PCIE_MAC_PM         0x00010194UL
#define L1_TOP_LPCR_HOST_BAND0  0x18060010UL
#define HW_CHIPID               0x70010200UL

/* host interrupt bits (mt7921/regs.h) */
#define INT_RX_DONE_WM          0x00000001UL    /* RX ring 0: MCU events                                 */
#define INT_RX_DONE_DATA        0x00000004UL    /* RX ring 2: frames                                     */
#define INT_RX_DONE_WM2         0x00000010UL    /* RX ring 4: events and frames from the firmware       */
#define INT_MCU_CMD             0x20000000UL    /* MCU2HOST software interrupt                           */
#define INT_MASK                (INT_RX_DONE_WM | INT_RX_DONE_DATA | INT_RX_DONE_WM2 | INT_MCU_CMD)

/* ---- DMA rings ---------------------------------------------------------------------------------------- */
#define RX_BUF          2048                    /* bytes per receive buffer                              */
#define RX_MAXBUF       32
#define TX0_N           32                      /* data/management ring: slots of TXD + TXP + frame      */
#define TX_SLOT         2048

typedef struct {
   ULONG  regs;                                 /* ring register block in BAR0                           */
   USHORT n;
   USHORT head, tail;
   DMA    desc;                                 /* n x 16-byte descriptors                               */
   DMA    buf[RX_MAXBUF];                       /* RX: one buffer per descriptor                         */
} RING;

extern RING g_rtx0, g_rfwdl, g_rwm, g_rrx0, g_rrx2, g_rrx4;
extern DMA  g_cmdbuf, g_bounce, g_txslot[TX0_N];

/* ---- MCU messages ---------------------------------------------------------------------------------------- */
#define MK_LEGACY   0                           /* mcu_txd, 64-byte header (CMD / CE)                    */
#define MK_EXT      1                           /* mcu_txd with cid 0xED + ext_cid                       */
#define MK_UNI      2                           /* uni_txd, 48-byte header                               */
#define MCU_Q_SET   1
#define MCU_Q_NA    3

#define MCU_CMD_TARGET_ADDR_LEN         0x01
#define MCU_CMD_FW_START                0x02
#define MCU_CMD_PATCH_START             0x05
#define MCU_CMD_PATCH_FINISH            0x07
#define MCU_CMD_PATCH_SEM               0x10
#define MCU_CE_CMD_START_HW_SCAN        0x03
#define MCU_CE_CMD_SET_EDCA_PARMS       0x1D
#define MCU_CE_CMD_SET_CHAN_DOMAIN      0x0F
#define MCU_CE_CMD_GET_NIC_CAPAB        0x8A
#define MCU_EXT_CMD_EFUSE_BUFFER_MODE   0x21
#define MCU_EXT_CMD_MAC_INIT_CTRL       0x46
#define MCU_EXT_CMD_SET_RX_PATH         0x4E
#define MCU_UNI_CMD_DEV_INFO_UPDATE     0x01
#define MCU_UNI_CMD_BSS_INFO_UPDATE     0x02
#define MCU_UNI_CMD_STA_REC_UPDATE      0x03
#define MCU_UNI_CMD_ROC                 0x27
#define MCU_EVENT_SCAN_DONE             0x0D
#define MCU_UNI_EVENT_ROC               0x27

#define ST_NOTX     0xFFFE                      /* McuSend: the command was not taken by the DMA         */
#define ST_NOEVT    0xFFFF                      /* McuSend: no answer                                    */

extern UCHAR  g_kind, g_setq, g_extcid;
extern UCHAR  g_pl[1200];                       /* payload of the next McuSend (hw_scan_req: 1186 bytes) */
extern UCHAR  g_evt[1024];                      /* the last answer                                       */
extern USHORT g_evtlen;
USHORT mt_mcu(UCHAR cid, USHORT plen, int wait);
int    mt_mcu_async_uni(UCHAR cid, UCHAR FAR *pl, USHORT plen);
void   mt_ba_flush(void);
#define MCU_DEAD   3                            /* this many unanswered commands in a row: firmware dead  */
extern USHORT g_mcu_fail, g_ba_cmds, g_ba_busy, g_ba_lost;
extern UCHAR  g_rxba;
extern USHORT g_force_cold, g_resets;
void   pl_zero(USHORT n);
void   pl16(USHORT at, USHORT v);
void   pl32(USHORT at, ULONG v);
ULONG  evt32(USHORT at);
USHORT evt16(USHORT at);

/* ---- registers, rings, service (mt_dma.c) ----------------------------------------------------------------- */
ULONG  mt_rd(ULONG off);
void   mt_wr(ULONG off, ULONG v);
void   mt_set(ULONG off, ULONG bits);
void   mt_clr(ULONG off, ULONG bits);
ULONG  mt_l1rd(ULONG addr);
void   mt_l1wr(ULONG addr, ULONG v);
ULONG  mt_poll(ULONG off, ULONG mask, ULONG val, ULONG limit);   /* polls taken, 0 = timeout              */
int    mt_dma_alloc(void);
void   mt_dma_disable(void);
void   mt_rings_init(void);
void   mt_dma_enable(void);
int    mt_txsend(RING *r, ULONG phys, USHORT len);               /* queue one buffer, wait for the DMA   */
int    mt_tx0(USHORT len, UCHAR FAR **frame);                     /* see mt_mac.c                          */
void   mt_irq_on(void);
void   mt_irq_off(void);
extern USHORT g_mt_on;                          /* rings set up: the service routine may run             */

/* events and frames seen by the service routine */
extern USHORT g_scan_done, g_roc_granted, g_evt_done;
extern UCHAR  g_roc_status, g_roc_ch;
extern UCHAR  g_wait_seq;                       /* sequence number McuSend waits for, 0 = none           */
extern USHORT g_rx_frames, g_rx_events, g_rx_other, g_tx_frames, g_tx_full;

/* ---- firmware and bring-up (mt_fw.c) ----------------------------------------------------------------------- */
extern ULONG  g_patch_len, g_ram_len;           /* the two files inside g_fwblob                         */
extern char   g_fwver[12], g_fwdate[16];
extern USHORT g_reattached;

/* ---- station objects (mt_mac.c) ---------------------------------------------------------------------------- */
#define AP_WCID     1                           /* the access point                                      */
#define OWN_WCID    19                          /* our own BSS entry (broadcast/multicast, group keys)   */
int    mt_dev_bss_init(void);                   /* DEV_INFO + BSS_INFO (disconnected)                    */
void   mt_rx_frame(UCHAR FAR *b, USHORT len);   /* one RX buffer with a frame (RXD + 802.11)             */
extern USHORT g_roc_held;
extern USHORT g_rxs[2][6], g_rxs_n, g_rxm[2][5], g_act_n, g_rxbw[4], g_rx2ss;
extern UCHAR  g_act[4][6];
extern UCHAR  g_ev_c[16];
extern USHORT g_ev_v[16], g_ev_n, g_key_wait;
extern ULONG  g_ev_t[16];
extern USHORT g_tim_bcn, g_tim_set, g_tim_null, g_tim_last, g_echo;
extern ULONG  g_rxs_d1[4];
extern UCHAR  g_rxs_fc[4], g_rxs_a[4][18];
void   mt_txs(UCHAR FAR *b, USHORT len);
extern UCHAR  g_txs_fc[8], g_txs_err[8];
extern USHORT g_txs_n, g_txs_dok, g_txs_dbad, g_txs_mok, g_txs_mbad, g_txe_state, g_txe_len, g_txe_lock;
extern USHORT g_uni_st[4];                      /* 0 DEV_INFO, 1 BSS_INFO, 3 last UNI command          */
extern ULONG  g_uni_val[4];
extern UCHAR  g_uh_cid[8], g_uh_tag[8];
extern USHORT g_uh_st[8], g_uh_n;
extern ULONG  g_uh_val[8];

#endif
