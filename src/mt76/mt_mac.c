/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * mt_mac.c - MT7921 back end: scan, the firmware objects of a join, transmit, receive and keys (Linux mt76:
 * mt7921/main.c, mt7921/mcu.c, mt76_connac_mcu.c, mt76_connac_mac.c, mt7921/mac.c).
 *
 * Objects, as mt7921 + mac80211 create them for a station:
 *   DEV_INFO (own MAC, omac 0) and BSS_INFO (bss 0)          at bring-up  (add_interface)
 *   STA_REC for the AP, wcid 1, state NONE                    join_prepare (sta_add)
 *   ROC JOIN on the AP's channel, held until the pairwise key  join_prepare (mgd_prepare_tx)
 *   BSS_INFO connected + QBSS + RLM, STA_REC ASSOC,            join_complete (sta_event ASSOC,
 *   STA_REC for our own BSS entry (wcid 19, sta = NULL)                       bss_info_changed ASSOC)
 *   STA_REC KEY_V2 on wcid 1 (pairwise) / wcid 19 (group)     key_set
 * Frames leave on TX ring 0 in the connac2 "cut-through" format: TXD (32 bytes) + hw TXP (32 bytes) in the DMA
 * descriptor, the 802.11 frame behind them in the same slot, referenced by the TXP.
 */
#include "drv.h"
#include "mt.h"

USHORT g_roc_held;
static UCHAR  g_roc_token;
static net_t *g_mnet;                               /* the AP of the current join                         */
static USHORT g_txlk;

USHORT g_uni_st[4];                                 /* DEV_INFO, BSS_INFO at bring-up; last join command   */
ULONG  g_uni_val[4];
UCHAR  g_uh_cid[8], g_uh_tag[8];                    /* the last 8 UNI commands: cid, first tlv tag,         */
USHORT g_uh_st[8];                                  /* McuSend status and firmware status                  */
ULONG  g_uh_val[8];
USHORT g_uh_n;

/* send a UNI command and wait; answer: le32 status at +40 (mt76_connac2_mcu_uni_event, after the 36-byte rxd).
 * Returns 0 = OK, else ST_NOTX / ST_NOEVT. A firmware error status is recorded (status text, UNIHIST=) but
 * not fatal: the stand-alone driver never checked it and its joins went through.                          */
static USHORT uni_slot(UCHAR cid, USHORT plen, USHORT slot)
{
   USHORT st, k = (USHORT)(g_uh_n & 7);
   UCHAR tag = (UCHAR)((cid == MCU_UNI_CMD_STA_REC_UPDATE) ? g_pl[8] : g_pl[4]);
   ULONG v = 0;

   g_kind = MK_UNI;
   st = mt_mcu(cid, plen, 1);
   if (st < ST_NOTX)
      v = (g_evtlen >= 44) ? evt32(40) : 0xFFFFFFFEUL;
   g_uni_st[slot] = st;
   g_uni_val[slot] = v;
   g_uh_cid[k] = cid;
   g_uh_tag[k] = tag;
   g_uh_st[k] = st;
   g_uh_val[k] = v;
   g_uh_n++;
   if (st >= ST_NOTX)
      return st;
   return 0;
}

static USHORT uni(UCHAR cid, USHORT plen)
{
   return uni_slot(cid, plen, 3);
}

/* ---- bring-up objects: DEV_INFO_UPDATE + BSS_INFO_UPDATE (basic, disconnected) --------------------------- */
int mt_dev_bss_init(void)
{
   USHORT st;

   pl_zero(16);
   pl16(6, 12);                                     /* DEV_INFO_ACTIVE tlv                                 */
   g_pl[8] = 1;
   fmemcpy((UCHAR FAR *)g_pl + 10, (UCHAR FAR *)g_hw_mac, 6);
   st = uni_slot(MCU_UNI_CMD_DEV_INFO_UPDATE, 16, 0);
   if (st == ST_NOTX)
      return -1;
   pl_zero(36);
   pl16(6, 32);                                     /* UNI_BSS_INFO_BASIC                                  */
   g_pl[8] = 1;                                     /* active                                              */
   pl32(12, 0x00010001UL);                          /* CONNECTION_INFRA_STA                                */
   g_pl[16] = 1;                                    /* disconnected                                        */
   st = uni_slot(MCU_UNI_CMD_BSS_INFO_UPDATE, 36, 1);
   /* like the stand-alone driver (whose scans worked): only a command the DMA did not take is fatal;
    * a missing answer or an error status is shown in the status text (DEVINFO= / BSSINFO=) */
   return st == ST_NOTX ? -1 : 0;
}

/* ---- receive ------------------------------------------------------------------------------------------------- */
void mt_rx_frame(UCHAR FAR *b, USHORT len)
{
   USHORT h = 24, v = 0, fl = 0;
   UCHAR g = b[5], sec = (UCHAR)(b[6] & 0x1F), r0, r1;
   short rssi = -127;

   if (!g_hw_ready || len < 48)
      return;
   if (b[7] & 0x0A)                                 /* rxd1 FCS error / ICV error                          */
      return;
   if (g & 0x40) h += 16;                           /* group 4                                             */
   if (g & 0x08) h += 16;                           /* group 1                                             */
   if (g & 0x10) h += 8;                            /* group 2                                             */
   if (g & 0x20) {                                  /* group 3: RXV, RCPI at +4                            */
      v = (USHORT)(h + 4);
      h += 8;
      if (g & 0x80) {                               /* group 5                                             */
         h += 24;
         v = h;
         h += 48;
      }
   }
   h = (USHORT)(h + 2 * (b[9] >> 6));               /* rxd2 HDR_OFFSET (alignment padding)                 */
   if (b[9] & 0x20)                                 /* header translated: not configured, not expected     */
      return;
   if (h + 24 > len)
      return;
   if (v) {
      r0 = b[v]; r1 = b[v + 1];
      rssi = (short)(((short)(r0 > r1 ? r0 : r1) - 220) / 2);
   }
   /* decrypted by the hardware: IV and MIC are stripped (mt7921_mac_fill_rx), so to the station this is a
    * plain frame. CM / CLM set = the key was missing: leave the protected bit, the station drops it. */
   if (sec && !(b[6] & 0x80) && !(b[7] & 0x01))
      b[h + 1] &= (UCHAR)~0x40;
   sta_rx_frame(b + h, (USHORT)(len - h), b[13], rssi, fl, 0UL);
}

/* ---- transmit on ring 0 ---------------------------------------------------------------------------------------- */
#define TXF_FIXED    0x0100                         /* basic rate (management, EAPOL)                       */

static int tx0(UCHAR FAR *hdr, USHORT hlen, UCHAR FAR *body, USHORT blen, USHORT flags, UCHAR wcid)
{
   USHORT fl, next, slot, n = (USHORT)(hlen + blen);
   UCHAR FAR *b, FAR *d;
   UCHAR fc = hdr[0], ftype = (UCHAR)((fc >> 2) & 3), stype = (UCHAR)(fc >> 4), tid = 0;
   ULONG q, w;
   int fixed = (ftype != 2) || (flags & (HWTX_EAPOL | TXF_FIXED));

   if (!g_mt_on || n > TX_SLOT - 64 || hlen < 24)
      return -1;
   fl = rt_irq_save();
   if (rt_lock_acquire(&g_txlk, (USHORT)(rt_cpu_id() + 1))) {
      rt_irq_restore(fl);
      return -2;
   }
   next = (USHORT)((g_rtx0.head + 1) % g_rtx0.n);
   if ((ULONG)next == mmio_rd(g_rtx0.regs + 0xC)) {   /* ring full                                        */
      g_tx_full++;
      rt_lock_release(&g_txlk);
      rt_irq_restore(fl);
      return -2;
   }
   slot = g_rtx0.head;
   b = DPTR(g_txslot[slot]);
   if (ftype == 2 && (fc & 0x80))
      tid = (UCHAR)(hdr[24] & 0x07);
   if (ftype != 2)        q = 0x10;                 /* ALTX0: management                                   */
   else if (flags & HWTX_EAPOL) q = 3;              /* AC_VO                                               */
   else                   q = 1;                    /* AC_BE                                               */

   fmemset(b, 0, 64);
   put32(b + 0, (ULONG)(n + 32) | (q << 25));       /* TX_BYTES, PKT_FMT CT (0), Q_IDX                     */
   put32(b + 4, 0x80020000UL | ((ULONG)(hlen >> 1) << 11) | ((ULONG)tid << 20) | wcid);
   w = ((ULONG)ftype << 4) | stype;                 /* frame type / sub type                               */
   if (fixed)
      w |= 0x80002000UL;                            /* FIX_RATE, HTC_VLD                                   */
   put32(b + 8, w);
   w = 0x10000000UL | (15UL << 11);                 /* BA_DISABLE, REM_TX_COUNT 15                         */
   if (flags & HWTX_PROTECT)
      w |= 0x00000002UL;                            /* PROTECT_FRAME: encrypt with the key in the WTBL     */
   put32(b + 12, w);
   if (fixed)                                       /* FIXED_BW, CCK 1M (2.4 GHz) / OFDM 6M (5 GHz)         */
      put32(b + 24, 0x00000004UL | ((g_jch > 14 ? 0x4BUL : 0x00UL) << 16));
   put32(b + 28, (24UL << 11) | ((ULONG)stype << 16) | ((ULONG)ftype << 20));
   put16(b + 32, (USHORT)(0x8000 | (slot + 1)));    /* hw TXP: msdu_id[0] = token                          */
   put32(b + 40, g_txslot[slot].phys + 64);         /* ptr[0].buf0                                         */
   put16(b + 44, (USHORT)(0x8000 | n));             /* len0 | LAST                                         */
   fmemcpy(b + 64, hdr, hlen);
   if (blen)
      fmemcpy(b + 64 + hlen, body, blen);

   d = DPTR(g_rtx0.desc) + slot * 16;
   put32(d + 0, g_txslot[slot].phys);
   put32(d + 8, 0);
   put32(d + 12, 0);
   put32(d + 4, ((ULONG)(64 | 0x4000)) << 16);      /* SD_LEN0 64, LAST_SEC0                               */
   g_rtx0.head = next;
   mmio_wr(g_rtx0.regs + 0x8, next);
   g_tx_frames++;
   rt_lock_release(&g_txlk);
   rt_irq_restore(fl);
   return 0;
}

int hw_mgmt_tx(UCHAR FAR *f, USHORT len)
{
   if (len < 24)
      return -1;
   return tx0(f, 24, f + 24, (USHORT)(len - 24), TXF_FIXED, AP_WCID);
}

int hw_data_tx(UCHAR FAR *hdr, USHORT hlen, UCHAR FAR *body, USHORT blen, USHORT flags)
{
   return tx0(hdr, hlen, body, blen, flags, AP_WCID);
}

/* ---- scan (START_HW_SCAN, passive on DFS channels by the regulatory flags) ------------------------------------- */
int hw_scan(void)
{
   static UCHAR seq;
   USHORT st, i;

   if (!g_hw_ready)
      return -1;
   pl_zero(1186);
   seq = (UCHAR)((seq + 1) & 0x7F);
   if (!seq)
      seq = 1;
   g_pl[0] = seq;                                   /* seq_num, band 0                                     */
   g_pl[3] = 0x01;                                  /* wildcard SSID                                       */
   g_pl[6] = 0x20;                                  /* SCAN_FUNC_SPLIT_SCAN                                */
   g_pl[7] = 1;                                     /* version                                             */
   for (i = 0; i < 6; i++)
      g_pl[1110 + i] = 0xFF;                        /* bssid                                               */
   g_scan_done = 0;
   g_setq = MCU_Q_SET;
   st = mt_mcu(MCU_CE_CMD_START_HW_SCAN, 1186, 0);
   if (st >= ST_NOTX)
      return -4;
   if (hw_wait(&g_scan_done, 12000))
      return -5;
   return 0;
}

/* no 40/80 MHz yet: 20 MHz only */
int hw_chan_ht40(UCHAR ch)  { (void)ch; return 0; }
int hw_chan_vht80(UCHAR ch) { (void)ch; return 0; }

/* ---- join objects -------------------------------------------------------------------------------------------------- */
static UCHAR has_wmm(void) { return (UCHAR)(g_mnet && g_mnet->wmm); }

/* mt7921_mcu_sta_update for the AP: BASIC, PHY, RA, STATE, WTBL{GENERIC, RX, HDR_TRANS} */
static USHORT sta_rec(UCHAR state, USHORT aid)
{
   USHORT p;
   UCHAR g5 = (UCHAR)(g_mnet->ch > 14);

   pl_zero(120);
   g_pl[1] = AP_WCID;                               /* sta_req_hdr: bss 0, wlan_idx_lo                     */
   pl16(2, 5);                                      /* tlv_num                                             */
   g_pl[4] = 1;                                     /* is_tlv_append                                       */
   p = 8;                                           /* STA_REC_BASIC                                       */
   pl16(p, 0); pl16(p + 2, 20);
   pl32(p + 4, 0x00010002UL);                       /* CONNECTION_INFRA_AP                                 */
   g_pl[p + 8] = 2;                                 /* CONN_STATE_PORT_SECURE                              */
   g_pl[p + 9] = has_wmm();
   pl16(p + 10, state ? aid : 0);
   fmemcpy((UCHAR FAR *)g_pl + p + 12, (UCHAR FAR *)g_mnet->bssid, 6);
   pl16(p + 18, state ? 1 : 3);                     /* EXTRA_INFO_VER (| NEW)                              */
   p += 20;                                         /* STA_REC_PHY                                         */
   pl16(p, 0x15); pl16(p + 2, 12);
   pl16(p + 4, g5 ? 0x0015 : 0x000F);               /* basic rates                                         */
   g_pl[p + 6] = (UCHAR)(g5 ? 0x08 : 0x03);         /* OFDM / HR_DSSS + ERP                                */
   p += 12;                                         /* STA_REC_RA: legacy rates                            */
   pl16(p, 1); pl16(p + 2, 16);
   pl16(p + 4, g5 ? 0x3FC0 : 0x3FCF);
   p += 16;                                         /* STA_REC_STATE                                       */
   pl16(p, 7); pl16(p + 2, 12);
   g_pl[p + 8] = state;                             /* 0 NONE, 2 ASSOC                                     */
   p += 12;                                         /* STA_REC_WTBL                                        */
   pl16(p, 13); pl16(p + 2, 52);
   g_pl[p + 4] = AP_WCID;
   g_pl[p + 5] = 1;                                 /* WTBL_RESET_AND_SET                                  */
   pl16(p + 6, 3);
   p += 12;                                         /* WTBL_GENERIC                                        */
   pl16(p, 0); pl16(p + 2, 20);
   fmemcpy((UCHAR FAR *)g_pl + p + 4, (UCHAR FAR *)g_mnet->bssid, 6);
   g_pl[p + 13] = has_wmm();
   pl16(p + 16, aid);                               /* partial_aid                                         */
   p += 20;                                         /* WTBL_RX                                             */
   pl16(p, 1); pl16(p + 2, 12);
   g_pl[p + 5] = 1; g_pl[p + 6] = 1; g_pl[p + 7] = 1;
   p += 12;                                         /* WTBL_HDR_TRANS: to_ds, no_rx_trans                  */
   pl16(p, 6); pl16(p + 2, 8);
   g_pl[p + 4] = 1;
   g_pl[p + 6] = 1;
   p += 8;
   return uni(MCU_UNI_CMD_STA_REC_UPDATE, p);
}

/* our own BSS entry (wcid 19, Linux sta = NULL): only WTBL{GENERIC peer = BSSID, muar 0xE; RX; HDR_TRANS} */
static USHORT vif_sta_rec(void)
{
   USHORT p = 8;

   pl_zero(60);
   g_pl[1] = OWN_WCID;
   pl16(2, 1);
   g_pl[4] = 1;
   g_pl[5] = 0x0E;                                  /* muar_idx                                            */
   pl16(p, 13); pl16(p + 2, 52);
   g_pl[p + 4] = OWN_WCID;
   g_pl[p + 5] = 1;
   pl16(p + 6, 3);
   p += 12;
   pl16(p, 0); pl16(p + 2, 20);
   fmemcpy((UCHAR FAR *)g_pl + p + 4, (UCHAR FAR *)g_mnet->bssid, 6);
   g_pl[p + 10] = 0x0E;
   p += 20;
   pl16(p, 1); pl16(p + 2, 12);
   g_pl[p + 5] = 1; g_pl[p + 6] = 1; g_pl[p + 7] = 1;
   p += 12;
   pl16(p, 6); pl16(p + 2, 8);
   g_pl[p + 4] = 1;
   g_pl[p + 6] = 1;
   return uni(MCU_UNI_CMD_STA_REC_UPDATE, 60);
}

/* BSS_INFO_UPDATE: basic (+QBSS), and when connected RLM (channel) */
static USHORT bss_info(int connected)
{
   USHORT st;
   UCHAR g5 = (UCHAR)(g_mnet->ch > 14);

   pl_zero(44);
   pl16(6, 32);                                     /* UNI_BSS_INFO_BASIC                                  */
   g_pl[8] = 1;
   pl32(12, 0x00010001UL);                          /* CONNECTION_INFRA_STA                                */
   g_pl[16] = (UCHAR)(connected ? 0 : 1);
   if (connected)
      fmemcpy((UCHAR FAR *)g_pl + 18, (UCHAR FAR *)g_mnet->bssid, 6);
   pl16(24, OWN_WCID);                              /* bmc_tx_wlan_idx                                     */
   pl16(26, g_jbcn ? g_jbcn : 100);
   g_pl[28] = (UCHAR)(g_jdtim ? g_jdtim : 1);
   g_pl[29] = (UCHAR)(g5 ? 0x01 : 0x06);            /* phymode A / B|G                                     */
   pl16(30, OWN_WCID);                              /* sta_idx                                             */
   pl16(32, g5 ? 0x08 : 0x03);                      /* nonht_basic_phy                                     */
   pl16(36, 15); pl16(38, 8);                       /* UNI_BSS_INFO_QBSS                                   */
   g_pl[40] = has_wmm();
   st = uni(MCU_UNI_CMD_BSS_INFO_UPDATE, 44);
   if (st || !connected)
      return st;
   pl_zero(24);                                     /* UNI_BSS_INFO_RLM                                    */
   pl16(4, 2); pl16(6, 20);
   g_pl[8] = g_mnet->ch;                            /* control channel                                     */
   g_pl[9] = g_mnet->ch;                            /* centre channel (20 MHz)                             */
   g_pl[12] = 2;                                    /* tx streams                                          */
   g_pl[13] = 3;                                    /* rx chain mask                                       */
   g_pl[14] = 1;                                    /* short_st                                            */
   g_pl[17] = g5;                                   /* nl80211 band                                        */
   return uni(MCU_UNI_CMD_BSS_INFO_UPDATE, 24);
}

/* UNI ROC acquire (JOIN) / abort */
static USHORT roc(int acquire, UCHAR ch, USHORT ms)
{
   pl_zero(28);
   if (acquire) {
      pl16(4, 0); pl16(6, 24);                      /* UNI_ROC_ACQUIRE                                     */
      g_pl[9]  = g_roc_token;
      g_pl[10] = ch;
      g_pl[12] = (UCHAR)(ch > 14 ? 2 : 1);
      g_pl[14] = ch;
      g_pl[17] = ch;
      g_pl[19] = 0;                                 /* MT7921_ROC_REQ_JOIN                                 */
      pl32(20, ms);
      g_pl[24] = 0xFF;                              /* dbdc band: auto                                     */
      g_kind = MK_UNI;
      return mt_mcu(MCU_UNI_CMD_ROC, 28, 0);
   }
   pl16(4, 1); pl16(6, 12);                         /* UNI_ROC_ABORT                                       */
   g_pl[9]  = g_roc_token;
   g_pl[10] = 0xFF;
   g_kind = MK_UNI;
   return mt_mcu(MCU_UNI_CMD_ROC, 16, 0);
}

static void roc_release(void)
{
   if (g_roc_held) {
      roc(0, 0, 0);
      g_roc_held = 0;
   }
}

/* mt792x_mac_set_timeing: CCK/OFDM timeouts, SIFS and slot time */
static void set_timing(UCHAR ch, int short_slot)
{
   ULONG icr = 0x00000968UL;                        /* EIFS 360, RIFS 2                                    */

   icr |= (ch > 14) ? 0x00100000UL : 0x000A0000UL;                  /* SIFS 16 / 10                      */
   icr |= (ch > 14 || short_slot) ? 0x09000000UL : 0x14000000UL;    /* slot 9 / 20                       */
   mt_set(0x00020C80UL, 0x00000300UL);              /* ARB_SCR: TX/RX off                                  */
   mt_wr(0x00021090UL, 0x003000E7UL);               /* TMAC_CDTR                                           */
   mt_wr(0x00021094UL, 0x001C003CUL);               /* TMAC_ODTR                                           */
   mt_wr(0x000210A4UL, icr);                        /* TMAC_ICR0                                           */
   mt_clr(0x00020C80UL, 0x00000300UL);
}

int hw_join_prepare(net_t *n)
{
   USHORT cap = get16((UCHAR FAR *)n->ie + 10);

   if (!g_hw_ready)
      return -1;
   roc_release();
   g_mnet = n;
   set_timing(n->ch, (cap & 0x0400) != 0);
   if (sta_rec(0, 0)) {
      dd_err("MT7921: STA_REC (AP, NONE) failed", g_uni_val[3]);
      return -1;
   }
   g_roc_token++;
   g_roc_granted = 0;
   if (roc(1, n->ch, 4000) >= ST_NOTX)
      return -1;
   g_roc_held = 1;
   if (hw_wait(&g_roc_granted, 1000)) {
      dd_err("MT7921: no ROC grant for channel", n->ch);
      roc_release();
      return -1;
   }
   return 0;
}

int hw_join_complete(void)
{
   if (!g_mnet)
      return -1;
   if (bss_info(1)) { dd_err("MT7921: BSS_INFO (connected) failed", g_uni_val[3]); return -1; }
   if (sta_rec(2, g_jaid)) { dd_err("MT7921: STA_REC (AP, ASSOC) failed", g_uni_val[3]); return -1; }
   if (vif_sta_rec()) { dd_err("MT7921: STA_REC (own BSS) failed", g_uni_val[3]); return -1; }
   if (!g_mnet->sec)
      roc_release();                                /* open network: no keys to wait for                   */
   return 0;
}

void hw_leave(void)
{
   roc_release();
   if (g_mnet && g_hw_ready)
      bss_info(0);
   g_mnet = 0;
}

/* ---- keys: STA_REC_KEY_V2 (mt76_connac_mcu_sta_key_tlv), CCMP only ----------------------------------------------- */
static int key_cmd(UCHAR FAR *key, USHORT pairwise, UCHAR idx, int add)
{
   UCHAR wcid = (UCHAR)(pairwise ? AP_WCID : OWN_WCID);

   pl_zero(52);
   g_pl[1] = wcid;
   pl16(2, 1);
   g_pl[4] = 1;
   g_pl[5] = (UCHAR)(pairwise ? 0 : 0x0E);
   pl16(8, 0x11);                                   /* STA_REC_KEY_V2                                      */
   g_pl[12] = (UCHAR)(add ? 0 : 1);                 /* SET_KEY = 0, DISABLE_KEY = 1                        */
   if (add) {
      pl16(10, 44);
      g_pl[13] = 1;                                 /* n_cipher                                            */
      g_pl[16] = 5;                                 /* MCU_CIPHER_AES_CCMP                                 */
      g_pl[17] = 36;                                /* cipher_len                                          */
      g_pl[18] = idx;
      g_pl[19] = 16;
      fmemcpy((UCHAR FAR *)g_pl + 20, key, 16);
      return uni(MCU_UNI_CMD_STA_REC_UPDATE, 52) ? -1 : 0;
   }
   pl16(10, 8);
   return uni(MCU_UNI_CMD_STA_REC_UPDATE, 16) ? -1 : 0;
}

int hw_key_set(UCHAR FAR *key, USHORT klen, USHORT pairwise, UCHAR idx)
{
   int rc;

   if (klen != 16 || !g_hw_ready)
      return -1;
   rc = key_cmd(key, pairwise, idx, 1);
   if (pairwise)
      roc_release();                                /* Linux keeps the JOIN ROC until the port is open     */
   return rc;
}

int hw_key_clear(USHORT pairwise, UCHAR idx)
{
   if (!g_hw_ready)
      return -1;
   return key_cmd((UCHAR FAR *)0, pairwise, idx, 0);
}

/* ---- aggregation: not yet (the station refuses ADDBA when hw_ba_start fails) --------------------------------------- */
int    hw_ba_start(UCHAR tid, USHORT ssn, USHORT win) { (void)tid; (void)ssn; (void)win; return -1; }
int    hw_ba_stop(UCHAR tid)                          { (void)tid; return 0; }
int    hw_txagg_ok(void)                              { return 0; }
USHORT hw_qos_sn(void)                                { return 0; }
void   hw_txagg_offer(UCHAR tid, USHORT win)          { (void)tid; (void)win; }
void   hw_txagg_stop(UCHAR tid)                       { (void)tid; }
