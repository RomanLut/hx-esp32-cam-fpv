#include "HalmacJaguar2MacInit.h"

#include <chrono>
#include <cstring>
#include <thread>
#include <utility>

#include "FrameParserJaguar2.h" /* H2C-queue txdesc macros + checksum */

/* These names are also #define macros in the Realtek hal_com_reg.h pulled in via
 * RtlAdapter.h; #undef them so the scoped constexpr below compile (this .cpp
 * uses no other definition of them). */
#undef REG_RSV_CTRL
#undef REG_RF_CTRL
#undef REG_GPIO_MUXCFG
#undef REG_LED_CFG
#undef REG_PAD_CTRL1
#undef REG_SYS_FUNC_EN
#undef REG_SYS_CFG1
#undef REG_SYS_CFG2
#undef REG_WLRF1
#undef REG_MCUFW_CTRL
#undef REG_ANAPAR_MAC_0
#undef REG_CPU_DMEM_CON
#undef REG_CR_EXT
#undef REG_CR
#undef REG_TXDMA_PQ_MAP
#undef REG_RXFF_BNDY
#undef REG_FIFOPAGE_CTRL_2
#undef REG_AUTO_LLT_V1
#undef REG_TXDMA_OFFSET_CHK
#undef REG_RQPN_CTRL_2
#undef REG_FIFOPAGE_INFO_1
#undef REG_FIFOPAGE_INFO_2
#undef REG_FIFOPAGE_INFO_3
#undef REG_FIFOPAGE_INFO_4
#undef REG_FIFOPAGE_INFO_5
#undef REG_H2C_HEAD
#undef REG_H2C_TAIL
#undef REG_H2C_READ_ADDR
#undef REG_H2C_INFO
#undef REG_FWFF_CTRL
#undef REG_FWFF_PKT_INFO
#undef REG_FWHW_TXQ_CTRL
#undef REG_BCNQ_BDNY_V1
#undef REG_BCNQ1_BDNY_V1
#undef REG_WMAC_CSIDMA_CFG
#undef REG_H2CQ_CSR
#undef REG_WMAC_FWPKT_CR
/* Post-DLFW MAC-cfg registers (halmac_init_8822b.c) */
#undef REG_SW_AMPDU_BURST_MODE_CTRL
#undef REG_AMPDU_MAX_TIME_V1
#undef REG_TX_HANG_CTRL
#undef REG_PROT_MODE_CTRL
#undef REG_BAR_MODE_CTRL
#undef REG_FAST_EDCA_VOVI_SETTING
#undef REG_FAST_EDCA_BEBK_SETTING
#undef REG_INIRTS_RATE_SEL
#undef REG_TIMER0_SRC_SEL
#undef REG_TXPAUSE
#undef REG_SLOT
#undef REG_PIFS
#undef REG_SIFS
#undef REG_EDCA_VO_PARAM
#undef REG_EDCA_VI_PARAM
#undef REG_RD_NAV_NXT
#undef REG_RXTSF_OFFSET_CCK
#undef REG_BCN_CTRL
#undef REG_TBTT_PROHIBIT
#undef REG_DRVERLYINT
#undef REG_BCNDMATIM
#undef REG_TX_PTCL_CTRL
#undef REG_RXFLTMAP0
#undef REG_RXFLTMAP2
#undef REG_RCR
#undef REG_RX_PKT_LIMIT
#undef REG_TCR
#undef REG_WMAC_TRXPTCL_CTL
#undef REG_SND_PTCL_CTRL
#undef REG_WMAC_OPTION_FUNCTION
#undef REG_RXDMA_MODE
#undef REG_USB_USBSTAT

namespace jaguar2 {

namespace {
/* Common-88xx MAC init registers/constants (same addresses as the Jaguar3
 * set — verified 88xx-common; the halmac init flow is shared). */
constexpr uint16_t REG_RSV_CTRL       = 0x001C;
constexpr uint16_t REG_RF_CTRL        = 0x001F;
constexpr uint16_t REG_GPIO_MUXCFG    = 0x0040;
constexpr uint16_t REG_LED_CFG        = 0x004C;
constexpr uint16_t REG_PAD_CTRL1      = 0x0064;
constexpr uint16_t REG_SYS_FUNC_EN    = 0x0002;
constexpr uint16_t REG_SYS_CFG1       = 0x00F0;
constexpr uint16_t REG_SYS_CFG2       = 0x00FC;
constexpr uint16_t REG_WLRF1          = 0x00EC;
constexpr uint16_t REG_MCUFW_CTRL     = 0x0080;
constexpr uint16_t REG_ANAPAR_MAC_0   = 0x1018;
constexpr uint16_t REG_CPU_DMEM_CON   = 0x1080;
constexpr uint16_t REG_CR_EXT         = 0x1100;

constexpr uint8_t SYS_FUNC_EN_VAL         = 0xD8;
constexpr uint8_t WLAN_PHY_REQ_DELAY      = 0xC;
constexpr uint8_t CHIP_VER_B_CUT          = 1;

/* TRX / priority-queue registers (88xx-common) */
constexpr uint16_t REG_CR                = 0x0100;
constexpr uint16_t REG_TXDMA_PQ_MAP      = 0x010C;
constexpr uint16_t REG_RXFF_BNDY         = 0x011C;
constexpr uint16_t REG_FIFOPAGE_CTRL_2   = 0x0204;
constexpr uint16_t REG_AUTO_LLT_V1       = 0x0208;
constexpr uint16_t REG_TXDMA_OFFSET_CHK  = 0x020C;
constexpr uint16_t REG_RQPN_CTRL_2       = 0x022C;
constexpr uint16_t REG_FIFOPAGE_INFO_1   = 0x0230;
constexpr uint16_t REG_FIFOPAGE_INFO_2   = 0x0234;
constexpr uint16_t REG_FIFOPAGE_INFO_3   = 0x0238;
constexpr uint16_t REG_FIFOPAGE_INFO_4   = 0x023C;
constexpr uint16_t REG_FIFOPAGE_INFO_5   = 0x0240;
constexpr uint16_t REG_H2C_HEAD          = 0x0244;
constexpr uint16_t REG_H2C_TAIL          = 0x0248;
constexpr uint16_t REG_H2C_READ_ADDR     = 0x024C;
constexpr uint16_t REG_H2C_INFO          = 0x0254;
constexpr uint16_t REG_FWFF_CTRL         = 0x029C;
constexpr uint16_t REG_FWFF_PKT_INFO     = 0x02A0;
constexpr uint16_t REG_FWHW_TXQ_CTRL     = 0x0420;
constexpr uint16_t REG_BCNQ_BDNY_V1      = 0x0424;
constexpr uint16_t REG_BCNQ1_BDNY_V1     = 0x0456;
constexpr uint16_t REG_WMAC_FWPKT_CR     = 0x0601;
constexpr uint16_t REG_WMAC_CSIDMA_CFG   = 0x169C;
constexpr uint16_t REG_H2CQ_CSR          = 0x1330;

/* Post-DLFW MAC-cfg registers + values (halmac_init_8822b.c) */
constexpr uint16_t REG_SW_AMPDU_BURST_MODE_CTRL = 0x04BC;
constexpr uint16_t REG_AMPDU_MAX_TIME_V1        = 0x0455;
constexpr uint16_t REG_TX_HANG_CTRL             = 0x045E;
constexpr uint16_t REG_PROT_MODE_CTRL           = 0x04C8;
constexpr uint16_t REG_BAR_MODE_CTRL            = 0x04CC;
constexpr uint16_t REG_FAST_EDCA_VOVI_SETTING   = 0x1448;
constexpr uint16_t REG_FAST_EDCA_BEBK_SETTING   = 0x144C;
constexpr uint16_t REG_INIRTS_RATE_SEL          = 0x0480;
constexpr uint16_t REG_TIMER0_SRC_SEL           = 0x05B4;
constexpr uint16_t REG_TXPAUSE                  = 0x0522;
constexpr uint16_t REG_SLOT                     = 0x051B;
constexpr uint16_t REG_PIFS                     = 0x0512;
constexpr uint16_t REG_SIFS                     = 0x0514;
constexpr uint16_t REG_EDCA_VO_PARAM            = 0x0500;
constexpr uint16_t REG_EDCA_VI_PARAM            = 0x0504;
constexpr uint16_t REG_RD_NAV_NXT               = 0x0544;
constexpr uint16_t REG_RXTSF_OFFSET_CCK         = 0x055E;
constexpr uint16_t REG_BCN_CTRL                 = 0x0550;
constexpr uint16_t REG_TBTT_PROHIBIT            = 0x0540;
constexpr uint16_t REG_DRVERLYINT               = 0x0558;
constexpr uint16_t REG_BCNDMATIM                = 0x0559;
constexpr uint16_t REG_TX_PTCL_CTRL             = 0x0520;
constexpr uint16_t REG_RXFLTMAP0                = 0x06A0;
constexpr uint16_t REG_RXFLTMAP2                = 0x06A4;
constexpr uint16_t REG_RCR                      = 0x0608;
constexpr uint16_t REG_RX_PKT_LIMIT             = 0x060C;
constexpr uint16_t REG_TCR                      = 0x0604;
constexpr uint16_t REG_WMAC_TRXPTCL_CTL         = 0x0668;
constexpr uint16_t REG_SND_PTCL_CTRL            = 0x0718;
constexpr uint16_t REG_WMAC_OPTION_FUNCTION     = 0x07D0;
constexpr uint16_t REG_RXDMA_MODE               = 0x0290;
constexpr uint16_t REG_USB_USBSTAT              = 0xFE11;

constexpr uint8_t  BIT_EN_EOF_V1                = 1u << 2;
constexpr uint8_t  BIT_EN_BCN_FUNCTION          = 1u << 3;
constexpr uint8_t  BIT_R_DISABLE_CHECK_VHTSIGB_CRC = 1u << 6;

constexpr uint8_t  WLAN_SLOT_TIME    = 0x09;
constexpr uint8_t  WLAN_PIFS_TIME    = 0x19;
constexpr uint32_t WLAN_SIFS_CFG     = 0x0A | (0x0Eu << 8) | (0x10u << 16) | (0x10u << 24);
constexpr uint16_t WLAN_VO_TXOP_LIMIT = 0x186;
constexpr uint16_t WLAN_VI_TXOP_LIMIT = 0x3BC;
constexpr uint32_t WLAN_NAV_CFG      = 0x05u | (0x1Bu << 16);
constexpr uint16_t WLAN_RX_TSF_CFG   = 0x30 | (0x30u << 8);
constexpr uint32_t WLAN_TBTT_TIME    = 0x04u | (0x064u << 8);
constexpr uint8_t  WLAN_DRV_EARLY_INT = 0x04;
constexpr uint8_t  WLAN_BCN_DMA_TIME  = 0x02;
constexpr uint32_t WLAN_RX_FILTER0   = 0x0FFFFFFF;
constexpr uint16_t WLAN_RX_FILTER2   = 0xFFFF;
constexpr uint32_t WLAN_RCR_CFG      = 0xE400220E;
constexpr uint8_t  WLAN_RXPKT_MAX_SZ_512 = (12288u >> 9);
constexpr uint8_t  WLAN_TX_FUNC_CFG1 = 0x30;
constexpr uint8_t  WLAN_TX_FUNC_CFG2 = 0x30;
constexpr uint8_t  WLAN_MAC_OPT_NORM_FUNC1 = 0x98;
constexpr uint32_t WLAN_MAC_OPT_FUNC2 = 0x30810041;
constexpr uint8_t  WLAN_AMPDU_MAX_TIME = 0x70;
constexpr uint32_t WLAN_RTS_LEN_TH = 0xFF;
constexpr uint32_t WLAN_RTS_TX_TIME_TH = 0x08;
constexpr uint32_t WLAN_MAX_AGG_PKT_LIMIT = 0x20;
constexpr uint32_t WLAN_RTS_MAX_AGG_PKT_LIMIT = 0x20;
constexpr uint8_t  WLAN_FAST_EDCA_VO_TH = 0x06;
constexpr uint8_t  WLAN_FAST_EDCA_VI_TH = 0x06;
constexpr uint8_t  WLAN_FAST_EDCA_BE_TH = 0x06;
constexpr uint8_t  WLAN_FAST_EDCA_BK_TH = 0x06;
constexpr uint16_t WLAN_BAR_RETRY_LIMIT = 0x01;
constexpr uint16_t WLAN_RA_TRY_RATE_AGG_LIMIT = 0x08;

constexpr uint8_t MAC_TRX_ENABLE = 0x0F; /* HCI_TXDMA|HCI_RXDMA|TXDMA|RXDMA */
constexpr uint8_t BIT_FWEN = 0x80;
constexpr uint8_t BIT_AUTO_INIT_LLT_V1 = 0x01;
constexpr uint8_t BLK_DESC_NUM = 0x3;

/* fifo allocation — 8822B/8822C TX_FIFO/page numbers. The 8821C has a
 * physically smaller FIFO (65536 TX / 16384 RX) and different page counts;
 * those are selected per-variant via fifo_params() below. */
constexpr uint32_t TX_FIFO_SIZE = 262144;
constexpr uint32_t RX_FIFO_SIZE = 24576;
constexpr uint32_t TX_PAGE_SHIFT = 7; /* 128B pages */
constexpr uint32_t C2H_PKT_BUF = 256;
constexpr uint16_t RSVD_PG_DRV_NUM = 16;
constexpr uint16_t RSVD_PG_H2C_EXTRAINFO_NUM = 24;
constexpr uint16_t RSVD_PG_H2C_STATICINFO_NUM = 8;
constexpr uint16_t RSVD_PG_H2CQ_NUM = 8;
constexpr uint16_t RSVD_PG_CPU_INSTRUCTION_NUM = 0;
constexpr uint16_t RSVD_PG_FW_TXBUF_NUM = 4;
constexpr uint16_t RSVD_PG_CSIBUF_NUM = 50;
constexpr uint16_t PG_HQ = 64, PG_NQ = 64, PG_LQ = 64, PG_EXQ = 0, PG_GAP = 1;

/* Per-variant FIFO/page allocation. Defaults above are 8822B/8822C; 8821C
 * values are from reference/8821cu halmac_8821c_cfg.h (FIFO sizes) +
 * halmac_init_8821c.c (RSVD_PG_CSIBUF_NUM=0, HALMAC_PG_NUM_3BULKOUT_8821C =
 * hq/nq/lq/exq/gap 16/16/16/0/1). */
struct FifoParams {
  uint32_t tx_fifo_size, rx_fifo_size;
  uint16_t rsvd_csibuf_num, pg_hq, pg_nq, pg_lq, pg_exq, pg_gap;
};
constexpr FifoParams fifo_params(ChipVariant v, bool is_usb = true) {
  /* 8821C PCIe (RTL8821CE) page allocation differs only in the extra queue:
   * rtw88 page_table_8821c[1] (PCIE) = hq/nq/lq/exq/gap 16/16/16/14/1 vs the
   * 3-bulkout USB 16/16/16/0/1. */
  if (v == ChipVariant::C8821C)
    return is_usb ? FifoParams{65536, 16384, 0, 16, 16, 16, 0, 1}
                  : FifoParams{65536, 16384, 0, 16, 16, 16, 14, 1};
  return FifoParams{TX_FIFO_SIZE, RX_FIFO_SIZE, RSVD_PG_CSIBUF_NUM,
                    PG_HQ, PG_NQ, PG_LQ, PG_EXQ, PG_GAP};
}

} /* namespace */

HalmacJaguar2MacInit::HalmacJaguar2MacInit(RtlAdapter device, Logger_t logger,
                                           ChipVariant variant)
    : _device{std::move(device)}, _logger{std::move(logger)},
      _variant{variant} {}

void HalmacJaguar2MacInit::enable_bb_rf(bool enable) {
  if (enable) {
    _device.rtw_write8(REG_SYS_FUNC_EN,
                       _device.rtw_read8(REG_SYS_FUNC_EN) | 0x03);
    _device.rtw_write8(REG_RF_CTRL, _device.rtw_read8(REG_RF_CTRL) | 0x07);
    _device.rtw_write32(REG_WLRF1,
                        _device.rtw_read32(REG_WLRF1) | (0x7u << 24));
  } else {
    _device.rtw_write8(REG_SYS_FUNC_EN,
                       _device.rtw_read8(REG_SYS_FUNC_EN) & ~0x03);
    _device.rtw_write8(REG_RF_CTRL, _device.rtw_read8(REG_RF_CTRL) & ~0x07);
    _device.rtw_write32(REG_WLRF1,
                        _device.rtw_read32(REG_WLRF1) & ~(0x7u << 24));
  }
}

void HalmacJaguar2MacInit::pre_init_system_cfg() {
  _device.rtw_write8(REG_RSV_CTRL, 0);

  /* USB: REG_SYS_CFG2+3 == 0x20 workaround (0xFExx is USB-page register space —
   * undefined over PCIe MMIO, hence the gate). */
  if (_device.is_usb() && _device.rtw_read8(REG_SYS_CFG2 + 3) == 0x20)
    _device.rtw_write8(0xFE5B, _device.rtw_read8(0xFE5B) | (1u << 4));

  /* PCIe: rtw_mac_pre_system_cfg sets REG_HCI_OPT_CTRL BIT_USB_SUS_DIS
   * (0x74[8]) — yes, on PCIe too in rtw88. */
  if (!_device.is_usb())
    _device.rtw_write32(0x0074, _device.rtw_read32(0x0074) | (1u << 8));

  /* pinmux */
  uint32_t v = _device.rtw_read32(REG_PAD_CTRL1) & ~((1u << 28) | (1u << 29));
  v |= (1u << 28) | (1u << 29);
  _device.rtw_write32(REG_PAD_CTRL1, v);

  v = _device.rtw_read32(REG_LED_CFG) & ~((1u << 25) | (1u << 26));
  _device.rtw_write32(REG_LED_CFG, v);

  v = _device.rtw_read32(REG_GPIO_MUXCFG) & ~(1u << 2);
  v |= (1u << 2);
  _device.rtw_write32(REG_GPIO_MUXCFG, v);

  enable_bb_rf(false);
  _logger->info("Jaguar2: pre_init_system_cfg done");
}

void HalmacJaguar2MacInit::init_system_cfg(ChannelWidth_t bw, uint8_t cut) {
  /* bw unused by design: halmac cfg_bw_88xx treats HALMAC_BW_5/10 identically
   * to 20 MHz at the MAC (REG_WMAC_TRXPTCL_CTL bits 7|8 cleared), so 5/10 MHz
   * narrowband needs no MAC delta for monitor/injection use — the re-clock is
   * pure PHY (set_channel_bw). Vendor SIFS scaling / CCK strip is AP/STA-only,
   * same policy as Jaguar3. */
  (void)bw;
  (void)WLAN_PHY_REQ_DELAY;
  /* NB: init_system_cfg_8822b differs from _8822c — it sets ONLY
   * BIT_WL_PLATFORM_RST in REG_CPU_DMEM_CON (NOT BIT_DDMA_EN), and does NOT
   * write PHY_REQ_DELAY. Setting DDMA_EN here (the 8822c value) wedged the
   * DLFW rsvd-page TX FIFO on 8822B. */
  uint32_t v = _device.rtw_read32(REG_CPU_DMEM_CON);
  v |= (1u << 16); /* BIT_WL_PLATFORM_RST only */
  _device.rtw_write32(REG_CPU_DMEM_CON, v);

  _device.rtw_write8(REG_SYS_FUNC_EN + 1,
                     _device.rtw_read8(REG_SYS_FUNC_EN + 1) | SYS_FUNC_EN_VAL);

  /* disable boot-from-flash */
  uint32_t tmp = _device.rtw_read32(REG_MCUFW_CTRL);
  if (tmp & (1u << 20)) { /* BIT_BOOT_FSPI_EN */
    _device.rtw_write32(REG_MCUFW_CTRL, tmp & ~(1u << 20));
    _device.rtw_write32(REG_GPIO_MUXCFG,
                        _device.rtw_read32(REG_GPIO_MUXCFG) & ~(1u << 19));
  }

  /* ANAPAR_MAC_0 B-cut analog fix is 8822B-lineage only; vendor
   * init_system_cfg_8821c never touches 0x1018. Gate on variant so the 8821C
   * matches its vendor init exactly (it would otherwise fire on a B-cut part). */
  if (_variant == ChipVariant::C8822B && cut == CHIP_VER_B_CUT)
    _device.rtw_write8(REG_ANAPAR_MAC_0,
                       _device.rtw_read8(REG_ANAPAR_MAC_0) & ~0x07);
  _logger->info("Jaguar2: init_system_cfg done (bw={} cut={})",
                static_cast<int>(bw), cut);
}

/* txdma_queue_mapping (NORMAL, USB 3-bulkout) + priority-queue page alloc.
 * Ported from HalmacJaguar3MacInit::init_trx_cfg (88xx-common; 8822B page numbers
 * match 8822C). */
bool HalmacJaguar2MacInit::init_trx_cfg(bool set_bcn_boundary) {
  _set_bcn_boundary = set_bcn_boundary;
  uint16_t pqmap = (3u << 14) | (3u << 12) | (1u << 10) | (1u << 8) |
                   (2u << 6) | (2u << 4); /* VO/VI->NQ, BE/BK->LQ, MG/HI->HQ */
  /* PCIe queue mapping (rtw88 rqpn_table_8821c[1]): vo/vi->NQ, be/bk->LQ,
   * mg->EXQ, hi->HQ — MG rides the extra queue (whose 14 pages the PCIe
   * fifo_params allocate) instead of sharing HQ. */
  if (!_device.is_usb())
    pqmap = (3u << 14) | (0u << 12) | (1u << 10) | (1u << 8) | (2u << 6) |
            (2u << 4);
  _device.rtw_write16(REG_TXDMA_PQ_MAP, pqmap);

  uint8_t en_fwff = _device.rtw_read8(REG_WMAC_FWPKT_CR) & BIT_FWEN;
  if (en_fwff)
    _device.rtw_write8(REG_WMAC_FWPKT_CR,
                       _device.rtw_read8(REG_WMAC_FWPKT_CR) & ~BIT_FWEN);
  _device.rtw_write8(REG_CR, 0);
  _device.rtw_write16(REG_FWFF_CTRL, _device.rtw_read16(REG_FWFF_PKT_INFO));
  _device.rtw_write8(REG_CR, MAC_TRX_ENABLE);
  if (en_fwff)
    _device.rtw_write8(REG_WMAC_FWPKT_CR,
                       _device.rtw_read8(REG_WMAC_FWPKT_CR) | BIT_FWEN);
  _device.rtw_write32(REG_H2CQ_CSR, 1u << 31);

  if (!priority_queue_cfg())
    return false;
  init_h2c();
  return true;
}

bool HalmacJaguar2MacInit::priority_queue_cfg() {
  const FifoParams fp = fifo_params(_variant, _device.is_usb());
  const uint16_t tx_fifo_pg_num = fp.tx_fifo_size >> TX_PAGE_SHIFT; /* 2048 */
  const uint16_t rsvd_pg_num =
      RSVD_PG_DRV_NUM + RSVD_PG_H2C_EXTRAINFO_NUM + RSVD_PG_H2C_STATICINFO_NUM +
      RSVD_PG_H2CQ_NUM + RSVD_PG_CPU_INSTRUCTION_NUM + RSVD_PG_FW_TXBUF_NUM +
      fp.rsvd_csibuf_num; /* 110 */
  const uint16_t acq_pg_num = tx_fifo_pg_num - rsvd_pg_num; /* 1938 */
  const uint16_t rsvd_boundary = acq_pg_num;
  _rsvd_boundary = rsvd_boundary;

  uint16_t cur = tx_fifo_pg_num;
  cur -= fp.rsvd_csibuf_num;
  const uint16_t rsvd_csibuf_addr = cur;
  cur -= RSVD_PG_FW_TXBUF_NUM;
  cur -= RSVD_PG_CPU_INSTRUCTION_NUM;
  cur -= RSVD_PG_H2CQ_NUM;

  const uint16_t pub_pg =
      acq_pg_num - fp.pg_hq - fp.pg_nq - fp.pg_lq - fp.pg_exq - fp.pg_gap;

  _device.rtw_write16(REG_FIFOPAGE_INFO_1, fp.pg_hq);
  _device.rtw_write16(REG_FIFOPAGE_INFO_2, fp.pg_lq);
  _device.rtw_write16(REG_FIFOPAGE_INFO_3, fp.pg_nq);
  _device.rtw_write16(REG_FIFOPAGE_INFO_4, fp.pg_exq);
  _device.rtw_write16(REG_FIFOPAGE_INFO_5, pub_pg);
  _device.rtw_write32(REG_RQPN_CTRL_2,
                      _device.rtw_read32(REG_RQPN_CTRL_2) | (1u << 31));

  _device.rtw_write16(REG_WMAC_CSIDMA_CFG, rsvd_csibuf_addr);
  _device.rtw_write8(REG_FWHW_TXQ_CTRL + 2,
                     _device.rtw_read8(REG_FWHW_TXQ_CTRL + 2) | (1u << 4));
  if (_set_bcn_boundary) {
    _device.rtw_write16(REG_FIFOPAGE_CTRL_2, rsvd_boundary);
    _device.rtw_write16(REG_BCNQ_BDNY_V1, rsvd_boundary);
    _device.rtw_write16(REG_FIFOPAGE_CTRL_2 + 2, rsvd_boundary);
    _device.rtw_write16(REG_BCNQ1_BDNY_V1, rsvd_boundary);
  }

  _device.rtw_write32(REG_RXFF_BNDY, fp.rx_fifo_size - C2H_PKT_BUF - 1);

  uint8_t v8 = _device.rtw_read8(REG_AUTO_LLT_V1);
  v8 &= ~(0xf << 4);
  v8 |= (BLK_DESC_NUM << 4);
  _device.rtw_write8(REG_AUTO_LLT_V1, v8);
  _device.rtw_write8(REG_AUTO_LLT_V1 + 3, BLK_DESC_NUM);
  _device.rtw_write8(REG_TXDMA_OFFSET_CHK + 1,
                     _device.rtw_read8(REG_TXDMA_OFFSET_CHK + 1) | (1u << 1));

  _device.rtw_write8(REG_AUTO_LLT_V1,
                     _device.rtw_read8(REG_AUTO_LLT_V1) | BIT_AUTO_INIT_LLT_V1);
  uint32_t cnt = 1000;
  while (_device.rtw_read8(REG_AUTO_LLT_V1) & BIT_AUTO_INIT_LLT_V1) {
    if (--cnt == 0) {
      _logger->error("Jaguar2: LLT auto-init timeout");
      return false;
    }
  }
  _device.rtw_write8(REG_CR + 3, 0); /* transfer mode NORMAL */
  _logger->info("Jaguar2: priority_queue_cfg done (rsvd_boundary={})",
                rsvd_boundary);
  return true;
}

bool HalmacJaguar2MacInit::send_h2c_pkt(const uint8_t pkt[32]) {
  /* PLTFM_SEND_H2C_PKT (rtl8822bu_halmac.c): 48-byte txdesc with TXPKTSIZE=32
   * + QSEL 0x13 (H2C queue), checksum, no OFFSET field — byte-verified against
   * a usbmon capture of the kernel driver's GENERAL_INFO packet (desc
   * 0x00000020 / 0x00001300 / ... / checksum 0x1320). */
  uint8_t frame[TXDESC_SIZE_8822B + 32] = {0};
  SET_TX_DESC_TXPKTSIZE_8822B(frame, 32);
  SET_TX_DESC_QSEL_8822B(frame, 0x13);
  cal_txdesc_chksum_8822b(frame);
  std::memcpy(frame + TXDESC_SIZE_8822B, pkt, 32);
  const int got = _device.bulk_send_sync_ep(
      _device.first_bulk_out_ep(), frame, static_cast<int>(sizeof(frame)),
      1000);
  return got == static_cast<int>(sizeof(frame));
}

bool HalmacJaguar2MacInit::send_fw_general_info(uint8_t rfe_type, bool r2t2r,
                                                uint8_t cut_ver,
                                                uint8_t package_type) {
  /* halmac h2c-pkt wire header (set_h2c_pkt_hdr_88xx, byte-verified via
   * usbmon): {0x01, CMD_ID_FW_OFFLOAD=0xff, sub_cmd, 0, total_len, 0,
   * seq_lo, seq_hi}; content from offset 8. */
  const FifoParams fp = fifo_params(_variant, _device.is_usb());
  const uint16_t tx_fifo_pg_num = fp.tx_fifo_size >> TX_PAGE_SHIFT;
  const uint16_t rsvd_pg_num =
      RSVD_PG_DRV_NUM + RSVD_PG_H2C_EXTRAINFO_NUM + RSVD_PG_H2C_STATICINFO_NUM +
      RSVD_PG_H2CQ_NUM + RSVD_PG_CPU_INSTRUCTION_NUM + RSVD_PG_FW_TXBUF_NUM +
      fp.rsvd_csibuf_num;
  const uint16_t rsvd_boundary =
      static_cast<uint16_t>(tx_fifo_pg_num - rsvd_pg_num);
  const uint16_t fw_txbuf_addr = static_cast<uint16_t>(
      tx_fifo_pg_num - fp.rsvd_csibuf_num - RSVD_PG_FW_TXBUF_NUM);

  /* GENERAL_INFO (0x0d): FW TX-buffer page offset above the rsvd boundary
   * (proc_send_general_info_88xx: rsvd_fw_txbuf_addr - rsvd_boundary). */
  uint8_t gen[32] = {0};
  gen[0] = 0x01;
  gen[1] = 0xff;
  gen[2] = 0x0d; /* SUB_CMD_ID_GENERAL_INFO */
  gen[4] = 8 + 4;
  gen[6] = _h2c_seq++;
  gen[10] = static_cast<uint8_t>(fw_txbuf_addr - rsvd_boundary);
  if (!send_h2c_pkt(gen)) {
    _logger->error("Jaguar2: GENERAL_INFO H2C send failed");
    return false;
  }

  /* PHYDM_INFO (0x11): rfe / rf-type (halmac enum: 2T2R=2, 1T1R=4) / cut /
   * per-nibble RX|TX antenna masks / ext_pa / package type / mp_mode. */
  uint8_t phy[32] = {0};
  phy[0] = 0x01;
  phy[1] = 0xff;
  phy[2] = 0x11; /* SUB_CMD_ID_PHYDM_INFO */
  phy[4] = 8 + 8;
  phy[6] = _h2c_seq++;
  phy[8] = rfe_type;
  phy[9] = r2t2r ? 0x02 : 0x04;
  phy[10] = cut_ver;
  phy[11] = r2t2r ? 0x33 : 0x11; /* rx_ant | tx_ant<<4 */
  phy[12] = 0x00; /* ext_pa */
  phy[13] = package_type;
  phy[14] = 0x00; /* mp_mode */
  if (!send_h2c_pkt(phy)) {
    _logger->error("Jaguar2: PHYDM_INFO H2C send failed");
    return false;
  }
  /* FW-consumption check: the MAC advances H2C_HEAD as packets enter the
   * queue and the FW advances H2C_READ_ADDR as it consumes them — equal
   * pointers mean the FW has read everything (get_h2c_buf_free_space_88xx
   * semantics). */
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  const uint32_t head = _device.rtw_read32(REG_H2C_HEAD) & 0x3ffff;
  const uint32_t rd = _device.rtw_read32(REG_H2C_READ_ADDR) & 0x3ffff;
  _logger->info("Jaguar2: FW general/phydm info H2C sent (rfe={} rf={} cut={} "
                "pkg={} fw_txbuf_off={} h2cq head=0x{:x} read=0x{:x}{})",
                rfe_type, r2t2r ? "2T2R" : "1T1R", cut_ver, package_type,
                fw_txbuf_addr - rsvd_boundary, head, rd,
                head == rd ? " CONSUMED" : " PENDING");
  return true;
}

void HalmacJaguar2MacInit::init_h2c() {
  const FifoParams fp = fifo_params(_variant, _device.is_usb());
  const uint16_t tx_fifo_pg_num = fp.tx_fifo_size >> TX_PAGE_SHIFT;
  uint16_t cur = tx_fifo_pg_num - fp.rsvd_csibuf_num - RSVD_PG_FW_TXBUF_NUM -
                 RSVD_PG_CPU_INSTRUCTION_NUM - RSVD_PG_H2CQ_NUM;
  const uint32_t h2cq_addr = static_cast<uint32_t>(cur) << TX_PAGE_SHIFT;
  const uint32_t h2cq_size = static_cast<uint32_t>(RSVD_PG_H2CQ_NUM)
                             << TX_PAGE_SHIFT;

  uint32_t v = (_device.rtw_read32(REG_H2C_HEAD) & 0xFFFC0000) | h2cq_addr;
  _device.rtw_write32(REG_H2C_HEAD, v);
  v = (_device.rtw_read32(REG_H2C_READ_ADDR) & 0xFFFC0000) | h2cq_addr;
  _device.rtw_write32(REG_H2C_READ_ADDR, v);
  v = (_device.rtw_read32(REG_H2C_TAIL) & 0xFFFC0000) | (h2cq_addr + h2cq_size);
  _device.rtw_write32(REG_H2C_TAIL, v);

  uint8_t v8 = static_cast<uint8_t>((_device.rtw_read8(REG_H2C_INFO) & 0xFC) | 0x01);
  _device.rtw_write8(REG_H2C_INFO, v8);
  v8 = static_cast<uint8_t>((_device.rtw_read8(REG_H2C_INFO) & 0xFB) | 0x04);
  _device.rtw_write8(REG_H2C_INFO, v8);
  v8 = static_cast<uint8_t>((_device.rtw_read8(REG_TXDMA_OFFSET_CHK + 1) & 0x7f) | 0x80);
  _device.rtw_write8(REG_TXDMA_OFFSET_CHK + 1, v8);
}

/* Post-DLFW MAC config. Ports init_mac_cfg_88xx (trx -> protocol -> edca ->
 * wmac) using the 8822B-specific bodies. init_trx_cfg sets the beacon boundary
 * here (set_bcn_boundary=true) — the download already booted, so the rsvd-page
 * download no longer needs it deferred. */
bool HalmacJaguar2MacInit::init_mac_cfg(ChannelWidth_t bw) {
  (void)bw;
  if (!init_trx_cfg(/*set_bcn_boundary=*/true))
    return false;
  /* halmac cfg_mac_clk_88xx (run by every kernel cfg_bw): MAC clock select
   * 80 MHz (REG_AFE_CTRL1 0x24[21:20]=0) + the TSF/EDCA microsecond-tick
   * dividers (0x55c/0x638 = 80). The chip reset defaults are WRONG for the
   * running clock (0x55c=0x64, 0x638=0x28 read back on the 8822BU): with the
   * µs ticks desynced 1.25-2x, ordinary 20 MHz frames survive but the 2-4x
   * longer-in-air 5/10 MHz narrowband frames hit mistimed MAC RX limits
   * (kernel-parity register diff; the working kernel runs 0x50/0x50). */
  _device.rtw_write32(0x0024,
                      _device.rtw_read32(0x0024) & ~((1u << 20) | (1u << 21)));
  _device.rtw_write8(0x055c, 80);
  _device.rtw_write8(0x0638, 80);
  init_protocol_cfg();
  init_edca_cfg();
  init_wmac_cfg();
  _logger->info("Jaguar2: init_mac_cfg done");
  return true;
}

/* init_protocol_cfg_8822b / _8821c */
void HalmacJaguar2MacInit::init_protocol_cfg() {
  /* The SW_AMPDU_BURST_MODE_CTRL (0x04BC) BIT6 clear is 8822B-only
   * (halmac_init_8822b.c:759); the 8821C init_protocol_cfg starts at
   * REG_AMPDU_MAX_TIME_V1 with no 0x04BC write. */
  if (_variant != ChipVariant::C8821C)
    _device.rtw_write8(REG_SW_AMPDU_BURST_MODE_CTRL,
                       _device.rtw_read8(REG_SW_AMPDU_BURST_MODE_CTRL) &
                           ~(1u << 6));
  _device.rtw_write8(REG_AMPDU_MAX_TIME_V1, WLAN_AMPDU_MAX_TIME);
  _device.rtw_write8(REG_TX_HANG_CTRL,
                     _device.rtw_read8(REG_TX_HANG_CTRL) | BIT_EN_EOF_V1);

  /* 8821C caps aggregation at 0x10 packets (halmac_init_8821c.c); 8822B at
   * 0x20 (halmac_init_8822b.c). */
  const uint32_t max_agg =
      _variant == ChipVariant::C8821C ? 0x10u : WLAN_MAX_AGG_PKT_LIMIT;
  const uint32_t rts_max_agg =
      _variant == ChipVariant::C8821C ? 0x10u : WLAN_RTS_MAX_AGG_PKT_LIMIT;
  uint32_t v = WLAN_RTS_LEN_TH | (WLAN_RTS_TX_TIME_TH << 8) |
               (max_agg << 16) | (rts_max_agg << 24);
  _device.rtw_write32(REG_PROT_MODE_CTRL, v);
  _device.rtw_write16(REG_BAR_MODE_CTRL + 2,
                      static_cast<uint16_t>(WLAN_BAR_RETRY_LIMIT |
                                            (WLAN_RA_TRY_RATE_AGG_LIMIT << 8)));
  _device.rtw_write8(REG_FAST_EDCA_VOVI_SETTING, WLAN_FAST_EDCA_VO_TH);
  _device.rtw_write8(REG_FAST_EDCA_VOVI_SETTING + 2, WLAN_FAST_EDCA_VI_TH);
  _device.rtw_write8(REG_FAST_EDCA_BEBK_SETTING, WLAN_FAST_EDCA_BE_TH);
  _device.rtw_write8(REG_FAST_EDCA_BEBK_SETTING + 2, WLAN_FAST_EDCA_BK_TH);
  _device.rtw_write8(REG_INIRTS_RATE_SEL,
                     _device.rtw_read8(REG_INIRTS_RATE_SEL) | (1u << 5));

  /* 8821C-only: pre-transmit-count control (init_protocol_cfg_8821c). The 8822B
   * init_protocol_cfg has no REG_PRECNT_CTRL write. pre_txcnt =
   * WLAN_PRE_TXCNT_TIME_TH (0x1E4) | BIT_EN_PRECNT (BIT11) = 0x9E4. */
  if (_variant == ChipVariant::C8821C) {
    _device.rtw_write8(0x04E5, 0xE4); /* REG_PRECNT_CTRL[7:0] */
    _device.rtw_write8(0x04E6, 0x09); /* REG_PRECNT_CTRL[15:8] */
  }
}

/* init_edca_cfg_8822b (20 MHz path) */
void HalmacJaguar2MacInit::init_edca_cfg() {
  _device.rtw_write8(REG_TIMER0_SRC_SEL,
                     _device.rtw_read8(REG_TIMER0_SRC_SEL) &
                         ~((1u << 4) | (1u << 5) | (1u << 6)));
  _device.rtw_write16(REG_TXPAUSE, 0x0000);
  _device.rtw_write8(REG_SLOT, WLAN_SLOT_TIME);
  _device.rtw_write8(REG_PIFS, WLAN_PIFS_TIME);
  _device.rtw_write32(REG_SIFS, WLAN_SIFS_CFG);
  _device.rtw_write16(REG_EDCA_VO_PARAM + 2, WLAN_VO_TXOP_LIMIT);
  _device.rtw_write16(REG_EDCA_VI_PARAM + 2, WLAN_VI_TXOP_LIMIT);
  _device.rtw_write32(REG_RD_NAV_NXT, WLAN_NAV_CFG);
  _device.rtw_write16(REG_RXTSF_OFFSET_CCK, WLAN_RX_TSF_CFG);
  _device.rtw_write8(REG_BCN_CTRL,
                     _device.rtw_read8(REG_BCN_CTRL) | BIT_EN_BCN_FUNCTION);
  _device.rtw_write32(REG_TBTT_PROHIBIT, WLAN_TBTT_TIME);
  _device.rtw_write8(REG_DRVERLYINT, WLAN_DRV_EARLY_INT);
  _device.rtw_write8(REG_BCNDMATIM, WLAN_BCN_DMA_TIME);
  _device.rtw_write8(REG_TX_PTCL_CTRL + 1,
                     _device.rtw_read8(REG_TX_PTCL_CTRL + 1) & ~(1u << 4));
}

/* init_wmac_cfg_8822b (init_low_pwr is a no-op on 8822B) */
void HalmacJaguar2MacInit::init_wmac_cfg() {
  /* Group-address hash filter wide open (REG_MAR = all-ones), as the Jaguar3
   * init_wmac_cfg and halmac init_wmac_cfg_88xx do. With MAR at 0, every
   * broadcast/multicast frame — i.e. all beacons — is dropped regardless of
   * RCR AB/AM. The USB bring-up got away without it because the chip's warm
   * state carried a set MAR; the PCIe path (post card-disable / FLR) starts
   * from 0 and received no management frames until this write
   * (hardware-bisected on the RTL8821CE, 2026-07-07). */
  _device.rtw_write32(0x0620 /* REG_MAR */, 0xFFFFFFFF);
  _device.rtw_write32(0x0624, 0xFFFFFFFF);
  _device.rtw_write32(REG_RXFLTMAP0, WLAN_RX_FILTER0);
  _device.rtw_write16(REG_RXFLTMAP2, WLAN_RX_FILTER2);
  /* RXFLTMAP1 (0x06A2) is the control-frame subtype filter. halmac's
   * init_wmac_cfg leaves it at reset; the vendor driver's init_misc then sets
   * it to 0x0400 (PS-Poll only, station mode). enable_rx() sets the RCR ACF
   * (accept-control-frames) bit, which this register gates a second time — so
   * for a promiscuous monitor all control subtypes must pass, matching the
   * already wide-open RXFLTMAP0/2. 8821C-only to keep the 8822B path
   * byte-identical. */
  if (_variant == ChipVariant::C8821C)
    _device.rtw_write16(0x06A2, 0xFFFF);
  _device.rtw_write32(REG_RCR, WLAN_RCR_CFG);
  _device.rtw_write8(REG_RX_PKT_LIMIT, WLAN_RXPKT_MAX_SZ_512);
  /* 8821C-only: CCK ACK timeout (init_wmac_cfg_8821c REG_ACKTO_CCK =
   * WLAN_ACK_TO_CCK 0x40). The 8822B init_wmac_cfg has no such write. */
  if (_variant == ChipVariant::C8821C)
    _device.rtw_write8(0x0639, 0x40);
  _device.rtw_write8(REG_TCR + 2, WLAN_TX_FUNC_CFG2);
  _device.rtw_write8(REG_TCR + 1, WLAN_TX_FUNC_CFG1);
  _device.rtw_write8(REG_WMAC_TRXPTCL_CTL + 4,
                     _device.rtw_read8(REG_WMAC_TRXPTCL_CTL + 4) | (1u << 1));
  _device.rtw_write8(REG_SND_PTCL_CTRL,
                     _device.rtw_read8(REG_SND_PTCL_CTRL) |
                         BIT_R_DISABLE_CHECK_VHTSIGB_CRC);
  _device.rtw_write32(REG_WMAC_OPTION_FUNCTION + 8, WLAN_MAC_OPT_FUNC2);
  _device.rtw_write8(REG_WMAC_OPTION_FUNCTION + 4, WLAN_MAC_OPT_NORM_FUNC1);
}

/* init_usb_cfg_88xx + RX-DMA aggregation (USB), ported from the working jaguar3
 * path — required so received frames flush into the bulk-IN endpoint. */
void HalmacJaguar2MacInit::init_usb_cfg() {
  uint8_t v = static_cast<uint8_t>((1u << 1) | (0x3u << 2)); /* DMA_MODE|BURST_CNT */
  if (_device.rtw_read8(REG_SYS_CFG2 + 3) == 0x20)
    v |= (0x0u << 4); /* USB3 */
  else if ((_device.rtw_read8(REG_USB_USBSTAT) & 0x3) == 0x1)
    v |= (0x1u << 4); /* USB2 HS */
  else
    v |= (0x2u << 4); /* USB2 FS */
  _device.rtw_write8(REG_RXDMA_MODE, v);
  _device.rtw_write16(REG_TXDMA_OFFSET_CHK,
                      _device.rtw_read16(REG_TXDMA_OFFSET_CHK) | (1u << 9));

  /* cfg_usb_rx_agg_88xx (USB mode), verbatim: enable BIT_RXDMA_AGG_EN (0x010C[2]),
   * clear the USB-mode bit (0x0283[7]), clear EN_PRE_CALC (0x0280 BIT29), and set
   * the agg page-threshold/timeout (SS: size 5 / timeout 0xA). */
  constexpr uint16_t kRxdmaAggPgTh = 0x0280;
  uint8_t dma_usb_agg = _device.rtw_read8(kRxdmaAggPgTh + 3);
  uint8_t agg_enable = _device.rtw_read8(REG_TXDMA_PQ_MAP);
  agg_enable |= (1u << 2);
  dma_usb_agg &= ~(1u << 7);
  uint32_t v32 = _device.rtw_read32(kRxdmaAggPgTh);
  _device.rtw_write32(kRxdmaAggPgTh, v32 & ~(1u << 29));
  _device.rtw_write8(REG_TXDMA_PQ_MAP, agg_enable);
  _device.rtw_write8(kRxdmaAggPgTh + 3, dma_usb_agg);
  const uint8_t ss = (_device.rtw_read8(REG_SYS_CFG2 + 3) == 0x20);
  _device.rtw_write16(kRxdmaAggPgTh,
                      static_cast<uint16_t>(0x05 | ((ss ? 0x0A : 0x20) << 8)));
  _logger->info("Jaguar2: init_usb_cfg (RXDMA_MODE=0x{:02x}, RX-agg EN)", v);
}

/* PCIe interface-PHY config (rtw_pci_phy_cfg): the 8821C gen1 intf table is a
 * single MDIO write {addr 0x09, val 0x6380} (gen2 is empty), issued through
 * the REG_MDIO_V1 (0x3F4) / REG_PCIE_MIX_CFG (0x3F8) window — normal MAC
 * registers, so plain rtw_read/write works. ASPM/CLKREQ tuning is skipped for
 * bring-up (power saving only; the transport already clears ASPM in LNKCTL). */
void HalmacJaguar2MacInit::pcie_phy_cfg() {
  constexpr uint16_t REG_MDIO_V1 = 0x03F4;
  constexpr uint16_t REG_PCIE_MIX_CFG = 0x03F8;
  const uint8_t addr = 0x09;
  const uint16_t data = 0x6380;
  _device.rtw_write16(REG_MDIO_V1, data);
  /* page = (addr < 32 ? 0 : 1) + G1 page offset (0). */
  _device.rtw_write8(REG_PCIE_MIX_CFG, addr & 0x1F);
  _device.rtw_write8(REG_PCIE_MIX_CFG + 3, 0);
  _device.rtw_write32(REG_PCIE_MIX_CFG,
                      _device.rtw_read32(REG_PCIE_MIX_CFG) | (1u << 5));
  for (int i = 0; i < 20; i++) {
    if ((_device.rtw_read32(REG_PCIE_MIX_CFG) & (1u << 5)) == 0) {
      _logger->info("Jaguar2: PCIe MDIO gen1 cfg applied (0x09=0x6380)");
      return;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(10));
  }
  _logger->warn("Jaguar2: PCIe MDIO write did not complete (non-fatal)");
}

} /* namespace jaguar2 */
