/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * mt_fw.c - MT7921 back end: identity, firmware file, bring-up, status (Linux mt76: mt7921/pci.c, init.c,
 * mt792x_core.c, mt76_connac2 patch/RAM download in mt76_connac_mcu.c).
 *
 * The firmware is written to the device once per boot, the ROM patch first and the RAM code right after it:
 *     copy /b WIFI_MT7961_patch_mcu_1_2_hdr.bin + WIFI_RAM_CODE_MT7961_1.bin \DEV\WRND32$
 * The patch file ends where its last section ends, so the two can be told apart inside the blob.
 *
 * After a warm reboot the WiFi firmware is still running (a PCIe secondary bus reset does not stop it, and the
 * WFSYS reset leaves the WFDMA idle on the HP Victus this was written on): then the host side is set up again
 * around the running firmware, like Linux mt792x_wpdma_reinit_cond().
 */
#include "drv.h"
#include "mt.h"

extern UCHAR  g_d0_fc[4][2], g_d0_a[4][18];
extern USHORT g_d0_n;

USHORT g_hw_ready, g_hw_failed, g_hw_state;
UCHAR  g_hw_mac[6];
UCHAR  g_hw_nss = 2;
ULONG  g_patch_len, g_ram_len;
char   g_fwver[12], g_fwdate[16];
USHORT g_reattached, g_force_cold, g_resets;

static char  *g_devname = "MediaTek MT7921 (Filogic 330) PCIe WLAN";
static USHORT g_matched;

/* ---- identity ---------------------------------------------------------------------------------------------- */
int hw_pci_match(USHORT ven, USHORT dev, USHORT subven, USHORT subdev)
{
   (void)subven; (void)subdev;
   if (ven != 0x14C3 || (dev != 0x7961 && dev != 0x0608 && dev != 0x0616))
      return 0;
   if (dev == 0x0608 || dev == 0x0616)
      g_devname = "MediaTek MT7921K / RZ608 (AMD) PCIe WLAN";
   g_matched = 1;
   g_bar_len = 0x00100000UL;                        /* BAR0 is 1 MB: 16 windows                           */
   return 1;
}

char *hw_name(void)    { return g_devname; }
char *hw_fw_name(void) { return "WIFI_MT7961_patch_mcu_1_2_hdr.bin + WIFI_RAM_CODE_MT7961_1.bin"; }

/* mt76_connac2_patch_hdr: build_date[16] ("20260224110909a\n"), platform[4] = "ALPS" */
int hw_fw_is_mine(UCHAR FAR *h, USHORT n)
{
   if (n < 20)
      return 0;
   return h[0] >= '0' && h[0] <= '9' && h[16] == 'A' && h[17] == 'L' && h[18] == 'P' && h[19] == 'S';
}

UCHAR hw_vendor_oui(USHORT i)
{
   static const UCHAR mtk[3] = { 0x00, 0x0C, 0xE7 };        /* MediaTek; the adapter's own when known      */

   if (g_hw_mac[0] | g_hw_mac[1] | g_hw_mac[2])
      return g_hw_mac[i < 3 ? i : 0];
   return mtk[i < 3 ? i : 0];
}

/* ---- helpers ------------------------------------------------------------------------------------------------- */
static int fail(const char *what, ULONG v)
{
   g_hw_failed = 1;
   g_hw_ready = 0;
   dd_err(what, v);
   return -1;
}

static void step(USHORT n)
{
   g_hw_state = n;
   tstr("[mt "); th16(n); tstr("]");
}

static ULONG be32(UCHAR FAR *p)
{
   U32 u;

   u.b[0] = p[3]; u.b[1] = p[2]; u.b[2] = p[1]; u.b[3] = p[0];
   return u.l;
}

static UCHAR g_hb[128];                             /* header bytes from the blob (DGROUP)                 */

static int fw_get(ULONG off, USHORT n)
{
   return blob_read(&g_fwblob, off, (UCHAR FAR *)g_hb, n);
}

/* the patch ends where its last section ends; the RAM code follows */
static int fw_split(void)
{
   ULONG nreg, i, end = 0;

   if (g_fwblob.len < 96 + 64 || fw_get(0, 96))
      return -1;
   nreg = be32((UCHAR FAR *)g_hb + 44);
   if (nreg == 0 || nreg > 8)
      return -1;
   for (i = 0; i < nreg; i++) {
      ULONG e;

      if (fw_get(96UL + ((ULONG)(USHORT)i << 6), 64))
         return -1;
      e = be32((UCHAR FAR *)g_hb + 4) + be32((UCHAR FAR *)g_hb + 8);
      if (e > end)
         end = e;
   }
   if (end < 160 || end + 36 >= g_fwblob.len)
      return -1;
   g_patch_len = end;
   g_ram_len = g_fwblob.len - end;
   return 0;
}

/* send len bytes of the blob from off on the firmware download ring, 4 KB at a time through the bounce buffer */
static int fw_send(ULONG off, ULONG len)
{
   while (len) {
      USHORT k = (len > 4096UL) ? 4096 : (USHORT)len;

      if (blob_read(&g_fwblob, off, DPTR(g_bounce), k) || mt_txsend(&g_rfwdl, g_bounce.phys, k))
         return -1;
      off += k;
      len -= k;
   }
   return 0;
}

static USHORT target(ULONG addr, ULONG len, ULONG mode)
{
   pl32(0, addr);
   pl32(4, len);
   pl32(8, mode);
   return mt_mcu((addr == 0x00200000UL || addr == 0x00900000UL) ? MCU_CMD_PATCH_START : MCU_CMD_TARGET_ADDR_LEN,
                 12, 1);
}

/* ---- ROM patch (mt76_connac2_load_patch) ---------------------------------------------------------------------- */
static int load_patch(void)
{
   ULONG nreg, i;
   USHORT st;
   int rc = 0;

   /* semaphore: 0 NOT_DL_SEM_FAIL, 1 IS_DL, 2 NOT_DL_SEM_SUCCESS, 3 REL_SEM_SUCCESS */
   pl32(0, 1);
   st = mt_mcu(MCU_CMD_PATCH_SEM, 4, 1);
   if (st == 0) {                                   /* held by an earlier run that stopped half way        */
      pl32(0, 0);
      (void)mt_mcu(MCU_CMD_PATCH_SEM, 4, 1);
      pl32(0, 1);
      st = mt_mcu(MCU_CMD_PATCH_SEM, 4, 1);
   }
   if (st == 1)
      return 0;                                     /* the patch is in already                             */
   if (st != 2)
      return fail("MT7921: patch semaphore, status", st);

   fw_get(0, 96);
   nreg = be32((UCHAR FAR *)g_hb + 44);
   for (i = 0; i < nreg && !rc; i++) {
      ULONG offs, addr, len, key, mode = 0x80000000UL;          /* DL_MODE_NEED_RSP                      */

      fw_get(96UL + ((ULONG)(USHORT)i << 6), 64);
      offs = be32((UCHAR FAR *)g_hb + 4);
      addr = be32((UCHAR FAR *)g_hb + 12);
      len  = be32((UCHAR FAR *)g_hb + 16);
      key  = be32((UCHAR FAR *)g_hb + 20);
      if (key != 0xFFFFFFFFUL) {
         if (g_hb[20] == 1)                         /* ENC_TYPE (bits 31:24) AES: ENCRYPT | RESET_IV | key */
            mode |= 0x00000009UL | (((ULONG)g_hb[23] << 1) & 0x00000006UL);
         else if (g_hb[20] == 2)                    /* scramble                                            */
            mode |= 0x00000049UL;
      }
      if (offs + len > g_patch_len) {
         rc = fail("MT7921: patch section outside the file", offs);
         break;
      }
      st = target(addr, len, mode);
      if (st >= ST_NOTX) {
         rc = fail("MT7921: patch download start, status", st);
         break;
      }
      if (fw_send(offs, len))
         rc = fail("MT7921: patch download stuck at", offs);
   }
   if (!rc) {
      pl32(0, 0);                                   /* PATCH_FINISH, no CRC check                          */
      st = mt_mcu(MCU_CMD_PATCH_FINISH, 4, 1);
      if (st != 0)
         rc = fail("MT7921: patch finish, status", st);
   }
   pl32(0, 0);                                      /* always release the semaphore                        */
   st = mt_mcu(MCU_CMD_PATCH_SEM, 4, 1);
   if (!rc && st != 3)
      rc = fail("MT7921: patch semaphore release, status", st);
   return rc;
}

/* ---- RAM code (mt76_connac2_load_ram): 36-byte trailer at the end, 40-byte region records before it ------- */
static int load_ram(void)
{
   ULONG tr = g_fwblob.len - 36, base, offset = g_patch_len, override = 0;
   USHORT nreg, i, st;

   if (fw_get(tr, 36))
      return fail("MT7921: RAM code trailer", tr);
   nreg = g_hb[2];
   fmemcpy((UCHAR FAR *)g_fwver, (UCHAR FAR *)g_hb + 7, 10);
   fmemcpy((UCHAR FAR *)g_fwdate, (UCHAR FAR *)g_hb + 17, 14);
   if (nreg == 0 || nreg > 8)
      return fail("MT7921: RAM code regions", nreg);
   base = tr - (ULONG)(USHORT)(nreg * 40);

   for (i = 0; i < nreg; i++) {
      ULONG addr, len, mode;
      UCHAR feat;

      if (fw_get(base + (ULONG)(USHORT)(i * 40), 40))
         return fail("MT7921: RAM code region", i);
      addr = get32((UCHAR FAR *)g_hb + 16);
      len  = get32((UCHAR FAR *)g_hb + 20);
      feat = g_hb[24];
      if (offset + len > base)
         return fail("MT7921: RAM code region outside the file", i);
      if (!(feat & 0x40)) {                         /* not FW_FEATURE_NON_DL                               */
         mode = 0x80000000UL;
         if (feat & 0x01) mode |= 0x00000009UL;     /* ENCRYPT + RESET_SEC_IV                              */
         if (feat & 0x10) mode |= 0x00000040UL;     /* ENCRY_MODE_SEL                                      */
         mode |= (ULONG)(feat & 0x06);              /* key index                                           */
         if (feat & 0x20) override = addr;
         st = target(addr, len, mode);
         if (st >= ST_NOTX)
            return fail("MT7921: RAM download start, status", st);
         if (fw_send(offset, len))
            return fail("MT7921: RAM download stuck at", offset);
      }
      offset += len;
   }
   pl32(0, override ? 1UL : 0UL);                   /* FW_START_REQ                                        */
   pl32(4, override);
   st = mt_mcu(MCU_CMD_FW_START, 8, 1);
   if (st >= ST_NOTX)
      return fail("MT7921: FW_START, status", st);
   if (!mt_poll(REG_CONN_ON_MISC, 3, 3, 3000000UL))   /* N9 ready, about 1.5 s                              */
      return fail("MT7921: the firmware did not start, CONN_ON_MISC", mt_rd(REG_CONN_ON_MISC));
   return 0;
}

/* ---- ownership ------------------------------------------------------------------------------------------------ */
static int own_handshake(void)
{
   USHORT i;
   ULONG n = 0;

   for (i = 0; i < 10 && !n; i++) {                 /* fw own                                              */
      mt_wr(REG_CONN_ON_LPCTL, 0x00000001UL);
      n = mt_poll(REG_CONN_ON_LPCTL, 4, 4, 100000UL);
   }
   if (!n)
      return -1;
   for (i = 0, n = 0; i < 10 && !n; i++) {          /* driver own                                          */
      mt_wr(REG_CONN_ON_LPCTL, 0x00000002UL);
      n = mt_poll(REG_CONN_ON_LPCTL, 4, 0, 100000UL);
   }
   return n ? 0 : -1;
}

/* mt7921e_driver_own via the L1 window, then L0s off */
static int mcu_own(void)
{
   ULONG n;

   mt_l1wr(L1_TOP_LPCR_HOST_BAND0, 0x00000002UL);
   for (n = 0; n < 500000UL; n++)
      if (!(mt_l1rd(L1_TOP_LPCR_HOST_BAND0) & 1UL))
         break;
   if (n >= 500000UL)
      return -1;
   mt_set(REG_PCIE_MAC_PM, 0x00000100UL);           /* L0S_DIS                                             */
   return 0;
}

/* mt7921_mcu_get_nic_capability: TLVs of { le32 type, le32 len, data }; type 7 = MAC address */
static int nic_cap(void)
{
   USHORT st, n, i, p;

   g_setq = MCU_Q_SET;
   st = mt_mcu(MCU_CE_CMD_GET_NIC_CAPAB, 0, 1);
   if (st >= ST_NOTX || g_evtlen < 40)
      return -1;
   n = evt16(36);
   for (i = 0, p = 40; i < n && p + 8 <= g_evtlen; i++) {
      USHORT type = evt16(p), len = evt16((USHORT)(p + 4));

      p += 8;
      if (p + len > g_evtlen)
         break;
      if (type == 7 && len >= 6)
         fmemcpy((UCHAR FAR *)g_hw_mac, (UCHAR FAR *)g_evt + p, 6);
      p = (USHORT)(p + len);
   }
   return (g_hw_mac[0] | g_hw_mac[1] | g_hw_mac[2] | g_hw_mac[3]) ? 0 : -1;
}

/* channel list (Netherlands / CE) for SET_CHAN_DOMAIN: { le16 hw_value, le16 pad, le32 flags } */
static const UCHAR g_ch2[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13 };
static const UCHAR g_ch5[] = { 36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140 };

/* Linux init_hardware (EFUSE buffer mode), start (MAC enable, regulatory, RX path), add_interface */
static int init_cmds(void)
{
   USHORT st, p, k;

   pl_zero(4);
   g_pl[1] = 1;                                     /* EE_FORMAT_WHOLE, buffer mode EFUSE                  */
   g_kind = MK_EXT; g_extcid = MCU_EXT_CMD_EFUSE_BUFFER_MODE;
   if ((st = mt_mcu(0xED, 4, 1)) >= ST_NOTX)
      return fail("MT7921: EFUSE_BUFFER_MODE, status", st);

   pl_zero(4);
   g_pl[0] = 1;                                     /* band 0 enable                                       */
   g_kind = MK_EXT; g_extcid = MCU_EXT_CMD_MAC_INIT_CTRL;
   if ((st = mt_mcu(0xED, 4, 1)) >= ST_NOTX)
      return fail("MT7921: MAC_INIT_CTRL, status", st);

   p = 12;
   pl_zero((USHORT)(12 + (sizeof(g_ch2) + sizeof(g_ch5)) * 8));
   g_pl[0] = 'N'; g_pl[1] = 'L';
   g_pl[5] = 3; g_pl[6] = 3;                        /* bw_2g, bw_5g                                        */
   g_pl[8] = sizeof(g_ch2);
   g_pl[9] = sizeof(g_ch5);
   for (k = 0; k < sizeof(g_ch2); k++, p += 8)
      g_pl[p] = g_ch2[k];
   for (k = 0; k < sizeof(g_ch5); k++, p += 8) {
      g_pl[p] = g_ch5[k];
      if (g_ch5[k] >= 52)
         g_pl[p + 4] = 0x0A;                        /* NO_IR | RADAR: passive only                         */
   }
   g_setq = MCU_Q_SET;
   if ((st = mt_mcu(MCU_CE_CMD_SET_CHAN_DOMAIN, p, 0)) >= ST_NOTX)
      return fail("MT7921: SET_CHAN_DOMAIN, status", st);

   pl_zero(76);                                     /* SET_RX_PATH: ch 1, 20 MHz, 2 TX streams, RX mask 3   */
   g_pl[0] = 1; g_pl[1] = 1; g_pl[3] = 2; g_pl[4] = 3;
   g_kind = MK_EXT; g_extcid = MCU_EXT_CMD_SET_RX_PATH;
   if ((st = mt_mcu(0xED, 76, 1)) >= ST_NOTX)
      return fail("MT7921: SET_RX_PATH, status", st);

   if (mt_dev_bss_init())
      return fail("MT7921: DEV_INFO / BSS_INFO not taken by the DMA", 0UL);
   return 0;
}

/* ---- bring-up -------------------------------------------------------------------------------------------------- */
/* mt792x_wfsys_reset: WFSYS_SW_RST_B (bit 0) low for 50 ms, then wait for WFSYS_SW_INIT_DONE (bit 4) */
static int wfsys_reset(void)
{
   USHORT i;

   mt_l1wr(0x18000140UL, mt_l1rd(0x18000140UL) & ~1UL);
   dd_msleep(50);
   mt_l1wr(0x18000140UL, mt_l1rd(0x18000140UL) | 1UL);
   for (i = 0; i < 50; i++) {
      if (mt_l1rd(0x18000140UL) & 0x10UL)
         return 0;
      dd_msleep(10);
   }
   return -1;
}

int hw_up(void)
{
   USHORT cmd, attempt;
   ULONG chip;

   g_hw_failed = 0;
   g_hw_ready = 0;
   step(1);
   if (!g_matched)
      return fail("MT7921: no supported adapter", 0UL);
   if (!g_fwblob.done)
      return fail("MT7921: the firmware files have not been written to the device", 0UL);
   step(2);
   if (fw_split())
      return fail("MT7921: the firmware is not patch + RAM code (copy /b patch + ram)", g_fwblob.len);
   step(3);
   cmd = pci_cfg_rd16(0x04);                        /* memory decode on, bus master still off              */
   pci_cfg_wr16(0x04, (USHORT)((cmd | 0x0002) & ~0x0004));
   step(4);
   if (bar_map())
      return fail("MT7921: BAR0 could not be mapped", g_bar0);
   step(5);
   if (mt_dma_alloc())
      return fail("MT7921: not enough DMA memory for the rings", (ULONG)dma_free_bytes());
   step(6);
   if (hook_init() || irq_hook())
      tstr("[mt: no interrupt: polled]");
   step(7);
   /* Attempt 0 reuses a firmware that is still running (warm reboot). If that firmware does not answer, or the
    * last bring-up failed, or the firmware died while running (g_force_cold), attempt 1 resets the WiFi
    * subsystem (mt792x_wfsys_reset) and loads patch and RAM code again, as Linux mt7921e_mac_reset. */
   for (attempt = (USHORT)(g_force_cold ? 1 : 0); attempt < 2; attempt++) {
      g_hw_failed = 0;
      g_mcu_fail = 0;
      if (attempt) {
         tstr("[mt: WiFi subsystem reset, firmware loaded again]"); tnl();
         g_resets++;
         if (wfsys_reset())
            tstr("[mt: WFSYS reset: no INIT_DONE]");
      }
      if (own_handshake()) {
         fail("MT7921: the chip did not hand over (CONN_ON_LPCTL)", mt_rd(REG_CONN_ON_LPCTL));
         continue;
      }
      chip = mt_l1rd(HW_CHIPID);
      if (LO16(chip) != 0x7961)
         return fail("MT7921: unexpected chip id", chip);

      g_reattached = (USHORT)(!attempt && (mt_rd(REG_CONN_ON_MISC) & 3UL) == 3UL);
      mt_wr(REG_HOST_INT_ENA, 0);
      mt_wr(REG_PCIE_MAC_INT_ENABLE, 0x000000FFUL);
      mt_dma_disable();                             /* stale rings of a previous session stop first        */
      mt_rings_init();
      pci_enable();                                 /* bus master, INTx, ASPM off                          */
      mt_dma_enable();
      if (!g_reattached)
         mt_wr(REG_SWDEF_MODE, 0);                  /* normal mode                                         */
      if (mcu_own()) {
         fail("MT7921: driver own (LPCR_HOST) timed out", mt_l1rd(L1_TOP_LPCR_HOST_BAND0));
         continue;
      }
      if (!g_reattached) {
         step(8);
         if (load_patch())
            continue;
         step(9);
         if (load_ram())
            continue;
      } else
         tstr("[mt: firmware still running: reattached]");
      step(0x0A);
      if (nic_cap()) {
         fail("MT7921: no MAC address from the firmware (NIC capability)", 0UL);
         continue;
      }
      break;
   }
   g_force_cold = 0;
   if (attempt >= 2) {
      g_force_cold = 1;                             /* 'echo up' tries the reset again                     */
      return -1;
   }
   if (!g_cfgmac) {
      fmemcpy((UCHAR FAR *)g_mymac, (UCHAR FAR *)g_hw_mac, 6);
      ndis_set_mac(g_mymac);
   }
   step(0x0B);
   if (init_cmds())
      return -1;
   mt_irq_on();
   step(0x64);
   g_hw_ready = 1;
   tstr("[mt: ready]"); tnl();
   return 0;
}

int hw_dead(void)
{
   if (g_mcu_fail < MCU_DEAD)
      return 0;
   g_force_cold = 1;
   return 1;
}

void hw_down(void)
{
   if (!g_hw_ready && !g_hw_state)
      return;
   if (g_joined)
      sta_leave();
   g_hw_ready = 0;
   if (g_mt_on) {
      mt_irq_off();
      mt_dma_disable();
      g_mt_on = 0;
   }
   g_hw_state = 0;
}

/* ---- status lines ----------------------------------------------------------------------------------------------- */
void hw_status(void (*ps)(const char *), void (*ph)(ULONG v, USHORT digits), void (*nl)(void))
{
   static char ver[12], date[16];

   fmemcpy((UCHAR FAR *)ver, (UCHAR FAR *)g_fwver, 10);
   fmemcpy((UCHAR FAR *)date, (UCHAR FAR *)g_fwdate, 14);
   ps("CHIP=MT7921 STATE="); ph(g_hw_state, 4);
   ps(g_hw_ready ? " READY" : (g_hw_failed ? " FAILED" : " not started"));
   ps(g_reattached ? " (firmware reused after warm reboot)" : "");
   ps(" RESETS="); ph(g_resets, 2); ps(" MCUFAIL="); ph(g_mcu_fail, 2); nl();
   ps("RXBA (block-ack firmware commands) on="); ph(g_rxba, 1); ps(" sent="); ph(g_ba_cmds, 4);
   ps(" ring-busy="); ph(g_ba_busy, 4); ps(" queue-full="); ph(g_ba_lost, 4); nl();
   ps("PATCH="); ph(g_patch_len, 8); ps(" RAM="); ph(g_ram_len, 8);
   if (ver[0]) { ps(" FWVER="); ps(ver); ps(" "); ps(date); }
   nl();
   ps("IRQ="); ps(g_irq_hooked ? "hooked" : "polled");
   ps(" RXFRAMES="); ph(g_rx_frames, 4); ps(" RXEVT="); ph(g_rx_events, 4); ps(" RXOTHER="); ph(g_rx_other, 4);
   ps(" TXFRAMES="); ph(g_tx_frames, 4); ps(" TXFULL="); ph(g_tx_full, 4);
   ps(" ROC="); ps(g_roc_held ? "held" : "-"); nl();
   ps("DEVINFO="); ph(g_uni_st[0], 4); ps("/"); ph(g_uni_val[0], 8);
   ps(" BSSINFO="); ph(g_uni_st[1], 4); ps("/"); ph(g_uni_val[1], 8);
   ps(" LASTUNI="); ph(g_uni_st[3], 4); ps("/"); ph(g_uni_val[3], 8);
   ps("  (status FFFF = no answer, FFFE = not sent; value = firmware status)"); nl();
   {
      USHORT i, n = (USHORT)(g_uh_n < 8 ? g_uh_n : 8);
      USHORT m = (USHORT)(g_txs_n < 8 ? g_txs_n : 8);

      ps("TXS (TX status, oldest first; frame control/error: 0 = ACKed, 1 = no ACK, 2 RTS, 4 queue timeout):");
      for (i = 0; i < m; i++) {
         USHORT k = (USHORT)((g_txs_n - m + i) & 7);

         ps(" "); ph(g_txs_fc[k], 2); ps("/"); ph(g_txs_err[k], 1);
      }
      ps("  total="); ph(g_txs_n, 4);
      nl();
      ps("TXSUM data ACKed="); ph(g_txs_dok, 4); ps(" data NOT ACKed="); ph(g_txs_dbad, 4);
      ps(" other ACKed="); ph(g_txs_mok, 4); ps(" other NOT ACKed="); ph(g_txs_mbad, 4);
      ps("  refused: not running="); ph(g_txe_state, 4); ps(" too long="); ph(g_txe_len, 4);
      ps(" busy="); ph(g_txe_lock, 4); ps(" ring full="); ph(g_tx_full, 4);
      nl();
      for (i = 0; i < 2; i++) {
         USHORT j;
         static const char *nm[6] = { " FCSerr=", " ICVerr=", " nokey=", " decrypted=", " prot-undecr=", " plain=" };

         ps(i ? "RXSEC group:  " : "RXSEC unicast:");
         for (j = 0; j < 6; j++) { ps(nm[j]); ph(g_rxs[i][j], 4); }
         nl();
      }
      for (i = 0; i < g_rxs_n; i++) {
         USHORT j;

         ps("RXSEC bad: wcid="); ph(g_rxs_d1[i] & 0x3FFUL, 3); ps(" rxd1="); ph(g_rxs_d1[i], 8);
         ps(" fc1="); ph(g_rxs_fc[i], 2); ps(" a1/a2/a3=");
         for (j = 0; j < 18; j++) {
            if (j && j % 6 == 0) ps(" ");
            ph(g_rxs_a[i][j], 2);
         }
         nl();
      }
      ps("TIM (beacons of our AP) beacons="); ph(g_tim_bcn, 4); ps(" our-AID-set="); ph(g_tim_set, 4);
      ps(" null-sent="); ph(g_tim_null, 4); ps(" aid="); ph(g_jaid, 4); ps(" last-bitmapctl="); ph(g_tim_last, 2);
      nl();
      ps("RXMODE (data from our AP; CCK OFDM HT-MF HT-GF VHT+) unicast=");
      for (i = 0; i < 5; i++) { ph(g_rxm[0][i], 4); ps(" "); }
      ps(" group=");
      for (i = 0; i < 5; i++) { ph(g_rxm[1][i], 4); ps(" "); }
      ps(" FWHT="); ph(g_fwht, 1); nl();
      ps("RXWIDTH (unicast from our AP) 20MHz="); ph(g_rxbw[0], 4); ps(" 40MHz="); ph(g_rxbw[1], 4);
      ps(" 80MHz="); ph(g_rxbw[2], 4); ps(" 160MHz="); ph(g_rxbw[3], 4); ps(" two-streams="); ph(g_rx2ss, 4); nl();
      ps("ACTION (from our AP, last 4: category action ...) n="); ph(g_act_n, 4);
      for (i = 0; i < 4 && i < g_act_n; i++) {
         USHORT j;

         ps(" ");
         for (j = 0; j < 6; j++) ph(g_act[i][j], 2);
      }
      nl();
      ps("HANDSHAKE (R rx eapol / T tx eapol: key info, S tx status, K pairwise / G group key; +ms) keywait=");
      ph(g_key_wait, 4); ps("ms:");
      {
         USHORT s0 = (USHORT)(g_ev_n > 16 ? g_ev_n - 16 : 0), j;

         for (j = s0; j < g_ev_n; j++) {
            USHORT k = (USHORT)(j & 15);
            static char c[2];

            c[0] = (char)g_ev_c[k]; c[1] = 0;
            ps(" "); ps(c); ph(g_ev_v[k], 4); ps("+"); ph(g_ev_t[k] - g_ev_t[s0 & 15], 4);
         }
      }
      nl();
      ps("ECHO (our own broadcasts sent back by the AP = it decrypted our frames)="); ph(g_echo, 4);
      ps(" AP-WMM="); ph(g_jnet ? g_jnet->wmm : 0xFF, 2); nl();
      ps("DROP0 (data not from our AP; fc0 fc1 addr1 addr2 addr3; BSSID=");
      for (i = 0; i < 6; i++) { ph(g_jbssid[i], 2); if (i < 5) ps(":"); }
      ps("), count="); ph(g_d0_n, 4); nl();
      for (i = 0; i < 4 && i < g_d0_n; i++) {
         USHORT j;

         ps("  "); ph(g_d0_fc[i][0], 2); ps(" "); ph(g_d0_fc[i][1], 2);
         for (j = 0; j < 18; j++) {
            if (j % 6 == 0) ps(" ");
            ph(g_d0_a[i][j], 2);
         }
         nl();
      }
      ps("UNIHIST (oldest first; cid 01 DEV 02 BSS 03 STA_REC, tag, status/firmware):");
      for (i = 0; i < n; i++) {
         USHORT k = (USHORT)((g_uh_n - n + i) & 7);

         ps(" "); ph(g_uh_cid[k], 2); ps("."); ph(g_uh_tag[k], 2); ps("="); ph(g_uh_st[k], 4);
         ps("/"); ph(g_uh_val[k], 8);
      }
      nl();
   }
}
