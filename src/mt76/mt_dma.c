/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * mt_dma.c - MT7921 back end: register access, WFDMA rings, MCU messages, the service routine and the
 * interrupt (Linux mt76: mt792x_dma.c, mt76_connac_mcu.c, dma.c, mt7921/pci.c).
 *
 * One consumer drains the receive rings: hw_service(). Answers to MCU commands are matched by sequence number
 * and copied to g_evt; mt_mcu() waits for them with hw_wait(), so it works polled and with the interrupt.
 */
#include "drv.h"
#include "mt.h"

RING g_rtx0  = { REG_TX_RING_BASE + 0x000, TX0_N };
RING g_rfwdl = { REG_TX_RING_BASE + 0x100, 16 };
RING g_rwm   = { REG_TX_RING_BASE + 0x110, 16 };
RING g_rrx0  = { REG_RX_RING_BASE + 0x000, 8 };
RING g_rrx2  = { REG_RX_RING_BASE + 0x020, 32 };
RING g_rrx4  = { REG_RX_RING_BASE + 0x040, 32 };
DMA  g_cmdbuf, g_bounce, g_txslot[TX0_N];

USHORT g_mt_on;
USHORT g_scan_done, g_roc_granted, g_evt_done;
UCHAR  g_roc_status, g_roc_ch, g_wait_seq;
static UCHAR g_wait_cid = 0xFF;                    /* UNI DEV/BSS/STA_REC: the answer must carry this cid */
USHORT g_rx_frames, g_rx_events, g_rx_other, g_tx_frames, g_tx_full;

UCHAR  g_kind = MK_LEGACY, g_setq = MCU_Q_NA, g_extcid;
UCHAR  g_pl[1200];
UCHAR  g_evt[1024];
USHORT g_evtlen;
static UCHAR  g_seq;
static USHORT g_irq_on, g_irq_masked;
static USHORT g_svc_lock, g_svc_again;
static USHORT g_dma_ok;

/* ---- registers ------------------------------------------------------------------------------------------ */
ULONG mt_rd(ULONG off)          { return mmio_rd(off); }
void  mt_wr(ULONG off, ULONG v) { mmio_wr(off, v); }
void  mt_set(ULONG off, ULONG b) { mmio_wr(off, mmio_rd(off) | b); }
void  mt_clr(ULONG off, ULONG b) { mmio_wr(off, mmio_rd(off) & ~b); }

/* mt7921_l1_rr / _wr: HIF_REMAP_L1 holds the upper 16 bits, the window at BAR0 + 0x40000 the lower */
static void l1_select(ULONG addr)
{
   ULONG r = mmio_rd(REG_HIF_REMAP_L1);

   r = (r & 0xFFFF0000UL) | (ULONG)HI16(addr);
   mmio_wr(REG_HIF_REMAP_L1, r);
   (void)mmio_rd(REG_HIF_REMAP_L1);
}

ULONG mt_l1rd(ULONG addr)
{
   l1_select(addr);
   return mmio_rd(REG_REMAP_BASE_L1 + (ULONG)LO16(addr));
}

void mt_l1wr(ULONG addr, ULONG v)
{
   l1_select(addr);
   mmio_wr(REG_REMAP_BASE_L1 + (ULONG)LO16(addr), v);
}

ULONG mt_poll(ULONG off, ULONG mask, ULONG val, ULONG limit)
{
   ULONG n;

   for (n = 1; n <= limit; n++)
      if ((mmio_rd(off) & mask) == val)
         return n;
   return 0;
}

/* ---- DMA memory -------------------------------------------------------------------------------------------- */
static int ring_alloc(RING *r, int rx)
{
   USHORT i;

   if (dma_get_al((USHORT)(r->n * 16), 256, &r->desc))
      return -1;
   if (rx)
      for (i = 0; i < r->n; i++)
         if (dma_get(RX_BUF, &r->buf[i]))
            return -1;
   return 0;
}

int mt_dma_alloc(void)
{
   USHORT i;

   if (g_dma_ok)
      return 0;
   if (ring_alloc(&g_rtx0, 0) || ring_alloc(&g_rfwdl, 0) || ring_alloc(&g_rwm, 0) ||
       ring_alloc(&g_rrx0, 1) || ring_alloc(&g_rrx2, 1) || ring_alloc(&g_rrx4, 1))
      return -1;
   if (dma_get(1280, &g_cmdbuf) || dma_get(4096, &g_bounce))
      return -1;
   for (i = 0; i < TX0_N; i++)
      if (dma_get(TX_SLOT, &g_txslot[i]))
         return -1;
   g_dma_ok = 1;
   return 0;
}

#define DESC(r, i)   (DPTR((r)->desc) + (i) * 16)

static void ring_init_tx(RING *r)
{
   USHORT i;
   UCHAR FAR *d;

   for (i = 0; i < r->n; i++) {
      d = DESC(r, i);
      put32(d + 0, 0);
      put32(d + 4, 0x80000000UL);                  /* DMA_DONE: free                                     */
      put32(d + 8, 0);
      put32(d + 12, 0);
   }
   mmio_wr(r->regs + 0x8, 0);                       /* cpu_idx                                             */
   mmio_wr(r->regs + 0xC, 0);                       /* dma_idx                                             */
   mmio_wr(r->regs + 0x4, r->n);                    /* ring size                                           */
   mmio_wr(r->regs + 0x0, r->desc.phys);
   r->head = r->tail = 0;
}

static void ring_init_rx(RING *r)
{
   USHORT i;
   UCHAR FAR *d;

   for (i = 0; i < r->n; i++) {
      d = DESC(r, i);
      put32(d + 0, r->buf[i].phys);
      put32(d + 4, (ULONG)RX_BUF << 16);            /* SD_LEN0                                             */
      put32(d + 8, 0);
      put32(d + 12, 0);
   }
   mmio_wr(r->regs + 0x8, 0);
   mmio_wr(r->regs + 0xC, 0);
   mmio_wr(r->regs + 0x4, r->n);
   mmio_wr(r->regs + 0x0, r->desc.phys);
   r->head = (USHORT)(r->n - 1);                    /* n-1 buffers handed to the device                     */
   r->tail = 0;
   mmio_wr(r->regs + 0x8, r->head);
}

void mt_rings_init(void)
{
   ring_init_tx(&g_rtx0);
   mmio_wr(REG_TX_EXT_CTRL, 0x00000004UL);
   ring_init_tx(&g_rwm);
   ring_init_tx(&g_rfwdl);
   ring_init_rx(&g_rrx0);
   ring_init_rx(&g_rrx4);
   ring_init_rx(&g_rrx2);
   g_seq = 0;
}

/* mt792x_dma_disable(force = true) */
void mt_dma_disable(void)
{
   mt_clr(REG_GLO_CFG, 0x00000001UL | 0x00000004UL | 0x00008000UL |   /* TX/RX_DMA_EN, CHAIN_EN           */
                       0x10000000UL | 0x08000000UL | 0x00200000UL);   /* OMIT_TX/RX_INFO, OMIT_RX_INFO_PFET2 */
   mt_poll(REG_GLO_CFG, 0x0000000AUL, 0, 100000UL);                   /* TX/RX_DMA_BUSY                    */
   mt_clr(REG_GLO_CFG_EXT0, 0x00000040UL);          /* TX_DMASHDL_EN                                      */
   mt_set(REG_DMASHDL_SW_CONTROL, 0x10000000UL);    /* DMASHDL_BYPASS                                     */
   mt_clr(REG_WFDMA0_RST, 0x00000030UL);            /* LOGIC + DMASHDL reset                              */
   mt_set(REG_WFDMA0_RST, 0x00000030UL);
}

/* mt792x_dma_enable() with the MT7921 prefetch values; interrupts stay off until mt_irq_on() */
void mt_dma_enable(void)
{
   static const ULONG rx_pf[5] = { 0x00000004UL, 0x00400004UL, 0x00800004UL, 0x00C00004UL, 0x01000004UL };
   static const UCHAR rx_at[5] = { 0x00, 0x08, 0x0C, 0x10, 0x14 };
   USHORT i;

   for (i = 0; i < 5; i++)
      mmio_wr(REG_RX_EXT_CTRL + rx_at[i], rx_pf[i]);
   for (i = 0; i < 7; i++)                           /* TX0..TX6: 0x01400004 + 0x00400000 * i              */
      mmio_wr(REG_TX_EXT_CTRL + (ULONG)(i * 4), 0x01400004UL + ((ULONG)i << 22));
   mmio_wr(REG_TX_EXT_CTRL + 0x40, 0x03400004UL);   /* TX16 (FWDL)                                        */
   mmio_wr(REG_TX_EXT_CTRL + 0x44, 0x03800004UL);   /* TX17 (MCU WM)                                      */
   mmio_wr(REG_RST_DTX_PTR, 0xFFFFFFFFUL);
   mmio_wr(REG_PRI_DLY_INT_CFG0, 0);
   mt_set(REG_GLO_CFG, 0x00000040UL | 0x00001000UL | 0x40000000UL | 0x10000000UL | 0x00000030UL |
                       0x00000800UL | 0x00002000UL | 0x00008000UL | 0x00200000UL);
   mt_set(REG_GLO_CFG, 0x00000001UL | 0x00000004UL);  /* TX_DMA_EN, RX_DMA_EN                             */
   mt_set(REG_WFDMA_DUMMY_CR, 0x00000002UL);        /* NEED_REINIT                                        */
   mt_set(REG_MCU2HOST_SW_INT_ENA, 0x00000001UL);   /* WAKE_RX_PCIE                                       */
   g_mt_on = 1;
}

/* ---- transmit (MCU and firmware rings): queue one buffer, wait until the DMA has fetched it ------------- */
int mt_txsend(RING *r, ULONG phys, USHORT len)
{
   UCHAR FAR *d = DESC(r, r->head);
   ULONG n;

   put32(d + 0, phys);
   put32(d + 8, 0);
   put32(d + 12, 0);
   put32(d + 4, ((ULONG)(len | 0x4000)) << 16);     /* SD_LEN0, LAST_SEC0                                 */
   r->head = (USHORT)((r->head + 1) % r->n);
   mmio_wr(r->regs + 0x8, r->head);
   for (n = 0; n < 1000000UL; n++)
      if (mmio_rd(r->regs + 0xC) == (ULONG)r->head)
         return 0;
   dd_err("MT7921: TX ring stuck, dma_idx", mmio_rd(r->regs + 0xC));
   return -1;
}

/* ---- MCU messages (mt76_connac2_mcu_fill_message) ---------------------------------------------------------- */
void pl_zero(USHORT n)              { fmemset((UCHAR FAR *)g_pl, 0, n); }
void pl16(USHORT at, USHORT v)      { put16((UCHAR FAR *)g_pl + at, v); }
void pl32(USHORT at, ULONG v)       { put32((UCHAR FAR *)g_pl + at, v); }
ULONG  evt32(USHORT at)             { return get32((UCHAR FAR *)g_evt + at); }
USHORT evt16(USHORT at)             { return get16((UCHAR FAR *)g_evt + at); }

/* Returns the status byte of the answer (0..255), ST_NOEVT if none came, ST_NOTX if the command was not sent.
 * The kind (g_kind), set_query (g_setq) and ext_cid (g_extcid) go back to their defaults afterwards. */
USHORT mt_mcu(UCHAR cid, USHORT plen, int wait)
{
   USHORT hdr = (USHORT)((g_kind == MK_UNI) ? 48 : 64);
   USHORT total = (USHORT)(hdr + plen);
   UCHAR FAR *b = DPTR(g_cmdbuf);
   UCHAR seq;

   seq = (UCHAR)(++g_seq & 0x0F);
   if (!seq)
      seq = (UCHAR)(++g_seq & 0x0F);
   fmemset(b, 0, 64);
   put32(b + 0, 0x41000000UL | total);              /* TXD0: Q_IDX 0x20, PKT_FMT CMD, TX_BYTES             */
   put32(b + 4, 0x80010000UL);                      /* TXD1: LONG_FORMAT, HDR_FORMAT CMD                   */
   if (g_kind == MK_UNI) {
      put16(b + 32, (USHORT)(total - 32));          /* uni_txd: len, cid                                   */
      put16(b + 34, cid);
      b[37] = 0xA0;                                 /* pkt_type                                            */
      b[39] = seq;
      b[43] = 0x07;                                 /* option: ACK | UNI | SET (byte 42 is s2d_index)      */
   } else {
      put16(b + 32, (USHORT)(total - 32));          /* mcu_txd: len, pq_id                                 */
      put16(b + 34, 0x8000);
      b[36] = (UCHAR)((g_kind == MK_EXT) ? 0xED : cid);
      b[37] = 0xA0;
      b[38] = (UCHAR)((g_kind == MK_EXT) ? MCU_Q_SET : g_setq);
      b[39] = seq;
      if (g_kind == MK_EXT) {
         b[41] = g_extcid;
         b[43] = 1;                                 /* ext_cid_ack (byte 42 is s2d_index)                  */
      }
   }
   /* like mt7921_mcu_parse_response: an answer to DEV_INFO / BSS_INFO / STA_REC_UPDATE whose cid does not
    * match is skipped (-EAGAIN) and the wait goes on */
   g_wait_cid = (UCHAR)((g_kind == MK_UNI && cid >= MCU_UNI_CMD_DEV_INFO_UPDATE &&
                         cid <= MCU_UNI_CMD_STA_REC_UPDATE) ? cid : 0xFF);
   g_kind = MK_LEGACY;
   g_setq = MCU_Q_NA;
   fmemcpy(b + hdr, (UCHAR FAR *)g_pl, plen);

   g_evt_done = 0;
   g_wait_seq = (UCHAR)(wait ? seq : 0);
   if (mt_txsend(&g_rwm, g_cmdbuf.phys, total)) {
      g_wait_seq = 0;
      return ST_NOTX;
   }
   if (!wait)
      return 0;
   if (hw_wait(&g_evt_done, 3000) || !g_evt_done) {
      g_wait_seq = 0;
      return ST_NOEVT;
   }
   return g_evt[32];
}

/* ---- receive ------------------------------------------------------------------------------------------------- */
static void rx_buffer(UCHAR FAR *b, USHORT len)
{
   UCHAR type = (UCHAR)(b[3] >> 3);

   if (type == 2 || (type == 7 && (b[2] & 0x0F) == 1)) {      /* NORMAL frame / NORMAL_MCU (firmware RX)     */
      g_rx_frames++;
      mt_rx_frame(b, len);
      return;
   }
   if (type != 7 || len < 32) {                                 /* TX status, TX free, ...                     */
      g_rx_other++;
      return;
   }
   g_rx_events++;
   if (g_wait_seq && b[29] == g_wait_seq && (g_wait_cid == 0xFF || (len >= 44 && b[36] == g_wait_cid))) {
      g_evtlen = (len > sizeof(g_evt)) ? (USHORT)sizeof(g_evt) : len;
      fmemcpy((UCHAR FAR *)g_evt, b, g_evtlen);
      g_wait_seq = 0;
      g_evt_done = 1;
   } else if (b[28] == MCU_UNI_EVENT_ROC && len >= 50) {         /* grant TLV at +40                           */
      g_roc_status = b[46];
      g_roc_ch = b[47];
      g_roc_granted = 1;
   } else if (b[28] == MCU_EVENT_SCAN_DONE) {
      g_scan_done = 1;
   }
}

static void rx_ring(RING *r)
{
   UCHAR FAR *d;
   USHORT guard;
   ULONG c;

   for (guard = 0; guard < r->n; guard++) {
      d = DESC(r, r->tail);
      c = get32(d + 4);
      if (!(c & 0x80000000UL))                      /* DMA_DONE                                            */
         break;
      rx_buffer(DPTR(r->buf[r->tail]), (USHORT)((c >> 16) & 0x3FFF));
      put32(d + 4, (ULONG)RX_BUF << 16);            /* re-arm                                              */
      r->head = r->tail;
      r->tail = (USHORT)((r->tail + 1) % r->n);
      mmio_wr(r->regs + 0x8, r->head);
   }
}

static void service_once(void)
{
   ULONG s = mmio_rd(REG_HOST_INT_STA);

   if (s == 0xFFFFFFFFUL)
      return;
   if (s) {
      mmio_wr(REG_HOST_INT_STA, s);
      if (s & INT_MCU_CMD)
         mmio_wr(REG_MCU_CMD, mmio_rd(REG_MCU_CMD));
   }
   rx_ring(&g_rrx0);
   rx_ring(&g_rrx4);
   rx_ring(&g_rrx2);
   sta_rx_tick();
}

void hw_service(void)
{
   USHORT id = (USHORT)(rt_cpu_id() + 1);

   if (!g_mt_on)
      return;
   if (rt_lock_try(&g_svc_lock, id)) {
      g_svc_again = 1;                              /* the owner runs once more                            */
      return;
   }
   do {
      g_svc_again = 0;
      service_once();
   } while (g_svc_again);
   rt_lock_release(&g_svc_lock);
}

int hw_wait(USHORT *flag, USHORT ms)
{
   ULONG t0 = dd_ms();

   for (;;) {
      hw_service();
      if (*flag)
         return 0;
      if ((long)(dd_ms() - t0) > (long)ms)
         return -1;
      dd_msleep(2);
   }
}

/* ---- interrupt --------------------------------------------------------------------------------------------- */
void mt_irq_on(void)
{
   if (!g_irq_hooked)
      return;
   mmio_wr(REG_PCIE_MAC_INT_ENABLE, 0x000000FFUL);
   g_irq_on = 1;
   g_irq_masked = 0;
   mmio_wr(REG_HOST_INT_ENA, INT_MASK);
}

void mt_irq_off(void)
{
   g_irq_on = 0;
   mmio_wr(REG_HOST_INT_ENA, 0);
}

/* INTERRUPT TIME: is the (shared) line ours? Then mask the device until the context hook has serviced it. */
USHORT hw_irq(void)
{
   ULONG s;

   if (!g_irq_on || !g_mt_on)
      return 0;
   s = mmio_rd(REG_HOST_INT_STA);
   if (s == 0xFFFFFFFFUL || !(s & INT_MASK))
      return 0;
   mmio_wr(REG_HOST_INT_ENA, 0);
   g_irq_masked = 1;
   return 1;
}

void hw_hook(void)
{
   hw_service();
   if (g_irq_masked && g_irq_on) {
      g_irq_masked = 0;
      mmio_wr(REG_HOST_INT_ENA, INT_MASK);
   }
}
