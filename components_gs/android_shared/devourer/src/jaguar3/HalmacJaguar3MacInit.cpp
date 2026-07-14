#include "HalmacJaguar3MacInit.h"

#include <cstdint>

/* The Jaguar1 hal/hal_com_reg.h (pulled in transitively) #defines many of
 * these REG_* names as macros, which would clash with the 8822C constexpr
 * offsets below. Undefine them here so this translation unit uses the
 * 8822C values transcribed from the halmac reg headers. */
#undef REG_ACKTO
#undef REG_ACKTO_CCK
#undef REG_AFE_CTRL1
#undef REG_AMPDU_MAX_TIME_V1
#undef REG_ARFR0
#undef REG_ARFR1_V1
#undef REG_ARFR4
#undef REG_ARFR5
#undef REG_ARFRH0
#undef REG_ARFRH1_V1
#undef REG_ARFRH4
#undef REG_ARFRH5
#undef REG_AUTO_LLT_V1
#undef REG_BAR_MODE_CTRL
#undef REG_BAR_TX_CTRL
#undef REG_BBPSF_CTRL
#undef REG_BCN_CTRL
#undef REG_BCN_CTRL_CLINT0
#undef REG_BCNDMATIM
#undef REG_BCN_MAX_ERR
#undef REG_BCNQ1_BDNY_V1
#undef REG_BCNQ_BDNY_V1
#undef REG_BF0_TIME_SETTING
#undef REG_BF1_TIME_SETTING
#undef REG_BF_TIMEOUT_EN
#undef REG_CPUMGQ_PARAMETER
#undef REG_CR
#undef REG_CTS2TO
#undef REG_DARFRC
#undef REG_DARFRCH
#undef REG_DRVERLYINT
#undef REG_EDCA_BE_PARAM
#undef REG_EDCA_BK_PARAM
#undef REG_EDCA_VI_PARAM
#undef REG_EDCA_VO_PARAM
#undef REG_EIFS
#undef REG_FAST_EDCA_BEBK_SETTING
#undef REG_FAST_EDCA_VOVI_SETTING
#undef REG_FIFOPAGE_CTRL_2
#undef REG_FIFOPAGE_INFO_1
#undef REG_FIFOPAGE_INFO_2
#undef REG_FIFOPAGE_INFO_3
#undef REG_FIFOPAGE_INFO_4
#undef REG_FIFOPAGE_INFO_5
#undef REG_FWFF_CTRL
#undef REG_FWFF_PKT_INFO
#undef REG_FWHW_TXQ_CTRL
#undef REG_GENERAL_OPTION
#undef REG_H2C_HEAD
#undef REG_H2C_INFO
#undef REG_H2CQ_CSR
#undef REG_H2C_READ_ADDR
#undef REG_H2C_TAIL
#undef REG_INIRTS_RATE_SEL
#undef REG_LIFETIME_EN
#undef REG_MAC_SPEC_SIFS
#undef REG_MAR
#undef REG_MISC_CTRL
#undef REG_NAV_CTRL
#undef REG_PIFS
#undef REG_PRECNT_CTRL
#undef REG_PROT_MODE_CTRL
#undef REG_RARFRCH
#undef REG_RCR
#undef REG_RD_CTRL
#undef REG_RD_NAV_NXT
#undef REG_RESP_SIFS_CCK
#undef REG_RESP_SIFS_OFDM
#undef REG_RQPN_CTRL_2
#undef REG_RRSR
#undef REG_RXFF_BNDY
#undef REG_RXFLTMAP0
#undef REG_RXFLTMAP2
#undef REG_RX_PKT_LIMIT
#undef REG_RXPSF_CTRL
#undef REG_RXTSF_OFFSET_CCK
#undef REG_SIFS
#undef REG_SLOT
#undef REG_SND_PTCL_CTRL
#undef REG_SPEC_SIFS
#undef REG_TBTT_PROHIBIT
#undef REG_TCR
#undef REG_TIMER0_SRC_SEL
#undef REG_TXDMA_OFFSET_CHK
#undef REG_TXDMA_PQ_MAP
#undef REG_TX_HANG_CTRL
#undef REG_TXPAUSE
#undef REG_TX_PTCL_CTRL
#undef REG_USTIME_EDCA
#undef REG_USTIME_TSF
#undef REG_WMAC_CSIDMA_CFG
#undef REG_WMAC_FWPKT_CR
#undef REG_WMAC_OPTION_FUNCTION_1
#undef REG_WMAC_OPTION_FUNCTION_2
#undef REG_WMAC_TRXPTCL_CTL_H
#undef REG_RSV_CTRL
#undef REG_SYS_CFG2
#undef REG_PAD_CTRL1
#undef REG_LED_CFG
#undef REG_SYS_CFG1
#undef REG_GPIO_MUXCFG
#undef REG_CPU_DMEM_CON
#undef REG_SYS_FUNC_EN
#undef REG_CR_EXT
#undef REG_MCUFW_CTRL
#undef REG_ANAPAR_MAC_0
#undef REG_RF_CTRL
#undef REG_WLRF1
#undef REG_RXDMA_MODE
#undef REG_USB_USBSTAT

/* Faithful port of HalMAC init_mac_cfg for RTL8822C (USB, 3 bulk-OUT, NORMAL
 * trx mode). Source: OpenHD/rtl88x2cu hal/halmac/halmac_88xx/halmac_8822c/
 * halmac_init_8822c.c + halmac_init_88xx.c. Register offsets and WLAN_* timing
 * constants are transcribed from the halmac reg/wlan-config headers; composite
 * (multi-macro) values were resolved with the C preprocessor against those
 * headers and are annotated with their source expression. */

namespace jaguar3 {

namespace {
/* ---- register offsets (halmac_reg_88xx.h / _8822c) ---- */
constexpr uint16_t REG_CR = 0x0100;
constexpr uint16_t REG_TXDMA_PQ_MAP = 0x010C;
constexpr uint16_t REG_RXFF_BNDY = 0x011C;
constexpr uint16_t REG_FIFOPAGE_CTRL_2 = 0x0204;
constexpr uint16_t REG_AUTO_LLT_V1 = 0x0208;
constexpr uint16_t REG_TXDMA_OFFSET_CHK = 0x020C;
constexpr uint16_t REG_RQPN_CTRL_2 = 0x022C;
constexpr uint16_t REG_FIFOPAGE_INFO_1 = 0x0230;
constexpr uint16_t REG_FIFOPAGE_INFO_2 = 0x0234;
constexpr uint16_t REG_FIFOPAGE_INFO_3 = 0x0238;
constexpr uint16_t REG_FIFOPAGE_INFO_4 = 0x023C;
constexpr uint16_t REG_FIFOPAGE_INFO_5 = 0x0240;
constexpr uint16_t REG_H2C_HEAD = 0x0244;
constexpr uint16_t REG_H2C_TAIL = 0x0248;
constexpr uint16_t REG_H2C_READ_ADDR = 0x024C;
constexpr uint16_t REG_H2C_INFO = 0x0254;
constexpr uint16_t REG_FWFF_CTRL = 0x029C;
constexpr uint16_t REG_FWFF_PKT_INFO = 0x02A0;
constexpr uint16_t REG_FWHW_TXQ_CTRL = 0x0420;
constexpr uint16_t REG_BCNQ_BDNY_V1 = 0x0424;
constexpr uint16_t REG_SPEC_SIFS = 0x0428;
constexpr uint16_t REG_DARFRC = 0x0430;
constexpr uint16_t REG_DARFRCH = 0x0434;
constexpr uint16_t REG_RARFRCH = 0x043C;
constexpr uint16_t REG_RRSR = 0x0440;
constexpr uint16_t REG_ARFR0 = 0x0444;
constexpr uint16_t REG_ARFRH0 = 0x0448;
constexpr uint16_t REG_ARFR1_V1 = 0x044C;
constexpr uint16_t REG_ARFRH1_V1 = 0x0450;
constexpr uint16_t REG_AMPDU_MAX_TIME_V1 = 0x0455;
constexpr uint16_t REG_BCNQ1_BDNY_V1 = 0x0456;
constexpr uint16_t REG_LIFETIME_EN = 0x0426;
constexpr uint16_t REG_TX_HANG_CTRL = 0x045E;
constexpr uint16_t REG_INIRTS_RATE_SEL = 0x0480;
constexpr uint16_t REG_ARFR4 = 0x049C;
constexpr uint16_t REG_ARFRH4 = 0x04A0;
constexpr uint16_t REG_ARFR5 = 0x04A4;
constexpr uint16_t REG_ARFRH5 = 0x04A8;
constexpr uint16_t REG_PROT_MODE_CTRL = 0x04C8;
constexpr uint16_t REG_BAR_MODE_CTRL = 0x04CC;
constexpr uint16_t REG_PRECNT_CTRL = 0x04E5;
constexpr uint16_t REG_EDCA_VO_PARAM = 0x0500;
constexpr uint16_t REG_EDCA_VI_PARAM = 0x0504;
constexpr uint16_t REG_EDCA_BE_PARAM = 0x0508;
constexpr uint16_t REG_EDCA_BK_PARAM = 0x050C;
constexpr uint16_t REG_PIFS = 0x0512;
constexpr uint16_t REG_SIFS = 0x0514;
constexpr uint16_t REG_SLOT = 0x051B;
constexpr uint16_t REG_TX_PTCL_CTRL = 0x0520;
constexpr uint16_t REG_TXPAUSE = 0x0522;
constexpr uint16_t REG_RD_CTRL = 0x0524;
constexpr uint16_t REG_BAR_TX_CTRL = 0x0530;
constexpr uint16_t REG_TBTT_PROHIBIT = 0x0540;
constexpr uint16_t REG_RD_NAV_NXT = 0x0544;
constexpr uint16_t REG_BCN_CTRL = 0x0550;
constexpr uint16_t REG_DRVERLYINT = 0x0558;
constexpr uint16_t REG_BCN_CTRL_CLINT0 = 0x0551;
constexpr uint16_t REG_USTIME_TSF = 0x055C;
constexpr uint16_t REG_BCNDMATIM = 0x0559;
constexpr uint16_t REG_BCN_MAX_ERR = 0x055D;
constexpr uint16_t REG_RXTSF_OFFSET_CCK = 0x055E;
constexpr uint16_t REG_MISC_CTRL = 0x0577;
constexpr uint16_t REG_TIMER0_SRC_SEL = 0x05B4;
constexpr uint16_t REG_TCR = 0x0604;
constexpr uint16_t REG_RCR = 0x0608;
constexpr uint16_t REG_RX_PKT_LIMIT = 0x060C;
constexpr uint16_t REG_MAR = 0x0620;
constexpr uint16_t REG_USTIME_EDCA = 0x0638;
constexpr uint16_t REG_ACKTO_CCK = 0x0639;
constexpr uint16_t REG_MAC_SPEC_SIFS = 0x063A;
constexpr uint16_t REG_RESP_SIFS_CCK = 0x063C;
constexpr uint16_t REG_RESP_SIFS_OFDM = 0x063E;
constexpr uint16_t REG_ACKTO = 0x0640;
constexpr uint16_t REG_CTS2TO = 0x0641;
constexpr uint16_t REG_EIFS = 0x0642;
constexpr uint16_t REG_NAV_CTRL = 0x0650;
constexpr uint16_t REG_WMAC_TRXPTCL_CTL_H = 0x066C;
constexpr uint16_t REG_RXFLTMAP0 = 0x06A0;
constexpr uint16_t REG_RXFLTMAP2 = 0x06A4;
constexpr uint16_t REG_BBPSF_CTRL = 0x06DC;
constexpr uint16_t REG_SND_PTCL_CTRL = 0x0718;
constexpr uint16_t REG_WMAC_OPTION_FUNCTION_1 = 0x07D4;
constexpr uint16_t REG_WMAC_OPTION_FUNCTION_2 = 0x07D8;
constexpr uint16_t REG_AFE_CTRL1 = 0x0024;
constexpr uint16_t REG_CPUMGQ_PARAMETER = 0x1518;
constexpr uint16_t REG_RXPSF_CTRL = 0x1610;
constexpr uint16_t REG_GENERAL_OPTION = 0x1664;
constexpr uint16_t REG_WMAC_CSIDMA_CFG = 0x169C;
constexpr uint16_t REG_H2CQ_CSR = 0x1330;
constexpr uint16_t REG_BF0_TIME_SETTING = 0x1428;
constexpr uint16_t REG_BF1_TIME_SETTING = 0x142C;
constexpr uint16_t REG_BF_TIMEOUT_EN = 0x1430;
constexpr uint16_t REG_FAST_EDCA_VOVI_SETTING = 0x1448;
constexpr uint16_t REG_FAST_EDCA_BEBK_SETTING = 0x144C;
constexpr uint16_t REG_WMAC_FWPKT_CR = 0x0601;
/* system-cfg registers (pre_init / init_system_cfg / enable_bb_rf) */
constexpr uint16_t REG_RSV_CTRL = 0x001C;
constexpr uint16_t REG_RF_CTRL = 0x001F;
constexpr uint16_t REG_GPIO_MUXCFG = 0x0040;
constexpr uint16_t REG_LED_CFG = 0x004C;
constexpr uint16_t REG_PAD_CTRL1 = 0x0064;
constexpr uint16_t REG_MCUFW_CTRL = 0x0080;
constexpr uint16_t REG_WLRF1 = 0x00EC;
constexpr uint16_t REG_SYS_CFG1 = 0x00F0;
constexpr uint16_t REG_SYS_CFG2 = 0x00FC;
constexpr uint16_t REG_SYS_FUNC_EN = 0x0002;
constexpr uint16_t REG_ANAPAR_MAC_0 = 0x1018;
constexpr uint16_t REG_CPU_DMEM_CON = 0x1080;
constexpr uint16_t REG_CR_EXT = 0x1100;
constexpr uint16_t REG_RXDMA_MODE = 0x0290;
constexpr uint16_t REG_USB_USBSTAT = 0xFE11;
constexpr uint8_t SYS_FUNC_EN_VAL = 0xD8;
constexpr uint8_t WLAN_PHY_REQ_DELAY = 0xC;
constexpr uint8_t WLAN_PHY_REQ_DELAY_5M = 0xE;
constexpr uint8_t WLAN_PHY_REQ_DELAY_10M = 0xA;
constexpr uint8_t CHIP_VER_B_CUT = 1;

/* ---- bit / value constants ---- */
constexpr uint8_t MAC_TRX_ENABLE = 0x0F; /* HCI_TXDMA|HCI_RXDMA|TXDMA|RXDMA */
constexpr uint8_t BIT_FWEN = 0x80;       /* BIT(7) of REG_WMAC_FWPKT_CR */
constexpr uint8_t BIT_AUTO_INIT_LLT_V1 = 0x01;
constexpr uint8_t BLK_DESC_NUM = 0x3;

/* ---- fifo allocation (TX_FIFO_SIZE_8822C / RSVD_PG_* / TX page shift) ---- */
constexpr uint32_t TX_FIFO_SIZE_8822C = 262144;
constexpr uint32_t RX_FIFO_SIZE_8822C = 24576;
constexpr uint32_t TX_PAGE_SHIFT = 7; /* 128B pages */
constexpr uint32_t C2H_PKT_BUF = 256;
constexpr uint16_t RSVD_PG_DRV_NUM = 16;
constexpr uint16_t RSVD_PG_H2C_EXTRAINFO_NUM = 24;
constexpr uint16_t RSVD_PG_H2C_STATICINFO_NUM = 8;
constexpr uint16_t RSVD_PG_H2CQ_NUM = 8;
constexpr uint16_t RSVD_PG_CPU_INSTRUCTION_NUM = 0;
constexpr uint16_t RSVD_PG_FW_TXBUF_NUM = 4;
constexpr uint16_t RSVD_PG_CSIBUF_NUM = 50;
/* NORMAL/3-bulkout page numbers (HALMAC_PG_NUM_3BULKOUT_8822C) */
constexpr uint16_t PG_HQ = 64, PG_NQ = 64, PG_LQ = 64, PG_EXQ = 0, PG_GAP = 1;

/* ---- WLAN_* protocol/timing constants (halmac wlan-config) ---- */
constexpr uint8_t WLAN_TXQ_RPT_EN = 0x1F;
constexpr uint8_t WLAN_AMPDU_MAX_TIME = 0x70;
constexpr uint16_t WLAN_PRE_TXCNT_TIME_TH = 0x1E4;
constexpr uint16_t WLAN_PRE_TXCNT = WLAN_PRE_TXCNT_TIME_TH | 0x800; /* |BIT_EN_PRECNT(BIT11) */
constexpr uint32_t WLAN_MAX_AGG_PKT_LIMIT = 0x3F;
constexpr uint32_t WLAN_RTS_MAX_AGG_PKT_LIMIT = 0x20;
constexpr uint32_t WLAN_RTS_LEN_TH = 0xFF;
constexpr uint32_t WLAN_RTS_TX_TIME_TH = 0x08;
constexpr uint16_t WLAN_BAR_RETRY_LIMIT = 0x01;
constexpr uint16_t WLAN_RA_TRY_RATE_AGG_LIMIT = 0x08;
constexpr uint8_t WLAN_FAST_EDCA_VO_TH = 0x06;
constexpr uint8_t WLAN_FAST_EDCA_VI_TH = 0x06;
constexpr uint8_t WLAN_FAST_EDCA_BE_TH = 0x06;
constexpr uint8_t WLAN_FAST_EDCA_BK_TH = 0x06;
constexpr uint32_t BITS_RRSR_RSC_8822C = 0x3u << 21;

constexpr uint32_t WLAN_DATA_RATE_FB_CNT_1_4 = 0x01000000;
constexpr uint32_t WLAN_DATA_RATE_FB_CNT_5_8 = 0x08070504;
constexpr uint32_t WLAN_RTS_RATE_FB_CNT_5_8 = 0x08070504;
constexpr uint32_t WLAN_DATA_RATE_FB_RATE0 = 0xFE01F010;
constexpr uint32_t WLAN_DATA_RATE_FB_RATE0_H = 0x40000000;
constexpr uint32_t WLAN_RTS_RATE_FB_RATE1 = 0x003FF010;
constexpr uint32_t WLAN_RTS_RATE_FB_RATE1_H = 0x40000000;
constexpr uint32_t WLAN_RTS_RATE_FB_RATE4 = 0x0600F010;
constexpr uint32_t WLAN_RTS_RATE_FB_RATE4_H = 0x400003E0;
constexpr uint32_t WLAN_RTS_RATE_FB_RATE5 = 0x0600F015;
constexpr uint32_t WLAN_RTS_RATE_FB_RATE5_H = 0x000000E0;

/* SIFS / RESP-SIFS (composite values resolved via cpp): */
constexpr uint8_t WLAN_SIFS_CCK_CTX = 0x0A, WLAN_SIFS_CCK_IRX = 0x0A;
constexpr uint8_t WLAN_SIFS_OFDM_CTX = 0x0E, WLAN_SIFS_OFDM_IRX = 0x0E;
constexpr uint16_t WLAN_SIFS_DUR_TUNE = 0x100A;   /* normal */
constexpr uint32_t WLAN_SIFS_CFG = 0x100A0E0A;    /* normal */
constexpr uint8_t WLAN_SIFS_OFDM_CTX_5M = 0x38, WLAN_SIFS_OFDM_IRX_5M = 0x38;
constexpr uint16_t WLAN_SIFS_DUR_TUNE_5M = 0x400A;
constexpr uint32_t WLAN_SIFS_CFG_5M = 0x400A380A;
constexpr uint16_t WLAN_SIFS_OFDM_5M = 0x40;
constexpr uint8_t WLAN_SIFS_OFDM_CTX_10M = 0x1C, WLAN_SIFS_OFDM_IRX_10M = 0x1C;
constexpr uint16_t WLAN_SIFS_DUR_TUNE_10M = 0x200A;
constexpr uint32_t WLAN_SIFS_CFG_10M = 0x200A1C0A;
constexpr uint16_t WLAN_SIFS_OFDM_10M = 0x20;

/* EDCA / slot / pifs / tbtt (normal + 5M + 10M) */
constexpr uint32_t WLAN_EDCA_VO_PARAM = 0x002FA226, WLAN_EDCA_VI_PARAM = 0x005EA328;
constexpr uint32_t WLAN_EDCA_BE_PARAM = 0x005EA42B, WLAN_EDCA_BK_PARAM = 0x0000A44F;
constexpr uint8_t WLAN_SLOT_TIME = 0x09, WLAN_PIFS_TIME = 0x1C;
constexpr uint32_t WLAN_TBTT_TIME = 0x6404;
constexpr uint32_t WLAN_EDCA_VO_PARAM_5M = 0x002FA27A, WLAN_EDCA_VI_PARAM_5M = 0x005EA382;
constexpr uint32_t WLAN_EDCA_BE_PARAM_5M = 0x005EA48E, WLAN_EDCA_BK_PARAM_5M = 0x0000A4D3;
constexpr uint8_t WLAN_SLOT_TIME_5M = 0x15, WLAN_PIFS_TIME_5M = 0x55;
constexpr uint32_t WLAN_TBTT_TIME_5M = 0x0001900F;
constexpr uint8_t WLAN_CPUMGQ_AIFS_5M = 0x72;
constexpr uint32_t WLAN_EDCA_VO_PARAM_10M = 0x002FA242, WLAN_EDCA_VI_PARAM_10M = 0x005EA346;
constexpr uint32_t WLAN_EDCA_BE_PARAM_10M = 0x005EA44C, WLAN_EDCA_BK_PARAM_10M = 0x0000A47B;
constexpr uint8_t WLAN_SLOT_TIME_10M = 0x0D, WLAN_PIFS_TIME_10M = 0x2D;
constexpr uint32_t WLAN_TBTT_TIME_10M = 0x0000C808;
constexpr uint8_t WLAN_CPUMGQ_AIFS_10M = 0x3E;

constexpr uint32_t WLAN_NAV_CFG = 0x05 | (0x1B << 16); /* RDG_NAV | TXOP_NAV<<16 */
constexpr uint16_t WLAN_RX_TSF_CFG = 0x30 | (0x30 << 8); /* CCK | OFDM<<8 */
constexpr uint8_t WLAN_DRV_EARLY_INT = 0x04;
constexpr uint8_t WLAN_BCN_CTRL_CLT0 = 0x10;
constexpr uint8_t WLAN_BCN_DMA_TIME = 0x02;
constexpr uint8_t WLAN_BCN_MAX_ERR = 0xFF;
constexpr uint8_t MAC_CLK_SPEED = 80;

/* WMAC */
constexpr uint8_t WLAN_ACK_TO = 0x21, WLAN_ACK_TO_CCK = 0x6A;
constexpr uint16_t WLAN_EIFS_DUR_TUNE = 0x40;
constexpr uint8_t WLAN_ACK_TO_5M = 0x75, WLAN_CTS2TO_5M = 0x50;
constexpr uint16_t WLAN_EIFS_DUR_TUNE_5M = 0xE2;
constexpr uint8_t WLAN_ACK_TO_10M = 0x3D, WLAN_CTS2TO_10M = 0x28;
constexpr uint16_t WLAN_EIFS_DUR_TUNE_10M = 0x76;
constexpr uint8_t WLAN_RESP_TXRATE = 0x84;
constexpr uint8_t WLAN_NAV_MAX = 0xC8;
constexpr uint8_t WLAN_BAR_ACK_TYPE = 0x05;
constexpr uint32_t WLAN_RX_FILTER0 = 0xFFFFFFFF;
constexpr uint16_t WLAN_RX_FILTER2 = 0xFFFF;
constexpr uint32_t WLAN_RCR_CFG = 0xE410220E;
constexpr uint8_t WLAN_RXPKT_MAX_SZ_512 = 12288 >> 9; /* =24 */
constexpr uint8_t WLAN_TX_FUNC_CFG2 = 0x30, WLAN_TX_FUNC_CFG1 = 0x30;
constexpr uint32_t WLAN_MAC_OPT_FUNC2 = 0xB1810041;
constexpr uint8_t WLAN_MAC_OPT_NORM_FUNC1 = 0x98;

constexpr uint16_t BIT_EN_BCN_FUNCTION = 0x08; /* BIT(3) of REG_BCN_CTRL */
constexpr uint16_t BIT_EN_TXCTS_IN_RXNAV_V1 = 0x02; /* BIT(1) */
constexpr uint16_t BIT_DUMMY_FCS_READY_MASK_EN = 0x200; /* BIT(9) */
constexpr uint16_t BIT_RXFIFO_GNT_CUT = 0x100;          /* BIT(8) */
constexpr uint8_t BIT_R_DISABLE_CHECK_VHTSIGB_CRC = 0x40; /* BIT(6) */

inline bool is_5m(ChannelWidth_t b) { return b == CHANNEL_WIDTH_5; }
inline bool is_10m(ChannelWidth_t b) { return b == CHANNEL_WIDTH_10; }
} /* namespace */

HalmacJaguar3MacInit::HalmacJaguar3MacInit(RtlAdapter device, Logger_t logger)
    : _device{device}, _logger{logger} {}

/* enable_bb_rf_88xx */
void HalmacJaguar3MacInit::enable_bb_rf(bool enable) {
  if (enable) {
    _device.rtw_write8(REG_SYS_FUNC_EN,
                       _device.rtw_read8(REG_SYS_FUNC_EN) | 0x03);
    _device.rtw_write8(REG_RF_CTRL,
                       _device.rtw_read8(REG_RF_CTRL) | 0x07);
    _device.rtw_write32(REG_WLRF1,
                        _device.rtw_read32(REG_WLRF1) |
                            (0x7u << 24)); /* BIT24|25|26 */
  } else {
    _device.rtw_write8(REG_SYS_FUNC_EN,
                       _device.rtw_read8(REG_SYS_FUNC_EN) & ~0x03);
    _device.rtw_write8(REG_RF_CTRL,
                       _device.rtw_read8(REG_RF_CTRL) & ~0x07);
    _device.rtw_write32(REG_WLRF1,
                        _device.rtw_read32(REG_WLRF1) & ~(0x7u << 24));
  }
}

/* pre_init_system_cfg_8822c (USB path) */
void HalmacJaguar3MacInit::pre_init_system_cfg() {
  _device.rtw_write8(REG_RSV_CTRL, 0);

  /* USB: REG_SYS_CFG2+3 == 0x20 workaround */
  if (_device.rtw_read8(REG_SYS_CFG2 + 3) == 0x20)
    _device.rtw_write8(0xFE5B, _device.rtw_read8(0xFE5B) | (1u << 4));

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

  if (_device.rtw_read8(REG_SYS_CFG1 + 2) & (1u << 4))
    _logger->error("Jaguar3: chip in TEST mode (SYS_CFG1)");
  _logger->info("Jaguar3: pre_init_system_cfg done (ported)");
}

/* init_usb_cfg_88xx: USB RX-DMA mode/burst + TX drop-data. REG_RXDMA_MODE
 * governs how the chip DMAs RX into the bulk-IN endpoint — without it the host's
 * bulk-IN reads time out (no frames delivered). */
void HalmacJaguar3MacInit::init_usb_cfg() {
  uint8_t v = static_cast<uint8_t>((1u << 1) | (0x3u << 2)); /* DMA_MODE|BURST_CNT */
  if (_device.rtw_read8(REG_SYS_CFG2 + 3) == 0x20)
    v |= (0x0u << 4); /* USB3 burst */
  else if ((_device.rtw_read8(REG_USB_USBSTAT) & 0x3) == 0x1)
    v |= (0x1u << 4); /* USB2 HS */
  else
    v |= (0x2u << 4); /* USB2 FS */
  _device.rtw_write8(REG_RXDMA_MODE, v);
  _device.rtw_write16(REG_TXDMA_OFFSET_CHK,
                      _device.rtw_read16(REG_TXDMA_OFFSET_CHK) |
                          (1u << 9)); /* BIT_DROP_DATA_EN */

  /* Enable RX-DMA aggregation (port of halmac cfg_usb_rx_agg_88xx, USB mode).
   * The kernel 8822eu driver writes these; devourer didn't. On 8822E silicon the
   * RX-DMA engine does not flush received frames into the bulk-IN endpoint unless
   * aggregation is enabled (the 8822C does by default), so without this the EU
   * delivers zero RX bytes (usbmon: bulk-IN URBs complete empty) even with the RF
   * in RX mode. Matches the kernel's values: 0x280=size0x5|timeout0x20<<8,
   * 0x10C[2] BIT_RXDMA_AGG_EN, 0x283 USB-mode (bit7 clear). */
  constexpr uint16_t kTxdmaPqMap = 0x010C;
  constexpr uint16_t kRxdmaAggPgTh = 0x0280;
  uint8_t dma_usb_agg = _device.rtw_read8(kRxdmaAggPgTh + 3);
  uint8_t agg_enable = _device.rtw_read8(kTxdmaPqMap);
  agg_enable |= (1u << 2);     /* BIT_RXDMA_AGG_EN */
  dma_usb_agg &= ~(1u << 7);   /* HALMAC_RX_AGG_MODE_USB */
  uint32_t v32 = _device.rtw_read32(kRxdmaAggPgTh);
  _device.rtw_write32(kRxdmaAggPgTh, v32 & ~(1u << 29)); /* clear BIT_EN_PRE_CALC */
  _device.rtw_write8(kTxdmaPqMap, agg_enable);
  _device.rtw_write8(kRxdmaAggPgTh + 3, dma_usb_agg);
  _device.rtw_write16(kRxdmaAggPgTh,
                      static_cast<uint16_t>(0x05 | (0x20 << 8))); /* size|timeout */

  _logger->info("Jaguar3: init_usb_cfg (RXDMA_MODE=0x{:02x}, RX-agg EN) — RX DMA to bulk-IN",
                v);
}

/* init_system_cfg_8822c */
void HalmacJaguar3MacInit::init_system_cfg(ChannelWidth_t bw, uint8_t cut) {
  uint32_t v = _device.rtw_read32(REG_CPU_DMEM_CON);
  v |= (1u << 16) | (1u << 8); /* BIT_WL_PLATFORM_RST | BIT_DDMA_EN */
  _device.rtw_write32(REG_CPU_DMEM_CON, v);

  _device.rtw_write8(REG_SYS_FUNC_EN + 1,
                     _device.rtw_read8(REG_SYS_FUNC_EN + 1) | SYS_FUNC_EN_VAL);

  /* PHY_REQ_DELAY 0x1100[27:24] by bandwidth */
  uint8_t d = _device.rtw_read8(REG_CR_EXT + 3) & 0xF0;
  if (is_5m(bw))
    d |= WLAN_PHY_REQ_DELAY_5M;
  else if (is_10m(bw))
    d |= WLAN_PHY_REQ_DELAY_10M;
  else
    d |= WLAN_PHY_REQ_DELAY;
  _device.rtw_write8(REG_CR_EXT + 3, d);

  /* disable boot-from-flash */
  uint32_t tmp = _device.rtw_read32(REG_MCUFW_CTRL);
  if (tmp & (1u << 20)) { /* BIT_BOOT_FSPI_EN */
    _device.rtw_write32(REG_MCUFW_CTRL, tmp & ~(1u << 20));
    _device.rtw_write32(REG_GPIO_MUXCFG,
                        _device.rtw_read32(REG_GPIO_MUXCFG) &
                            ~(1u << 19)); /* BIT_FSPI_EN */
  }

  if (cut == CHIP_VER_B_CUT)
    _device.rtw_write8(REG_ANAPAR_MAC_0,
                       _device.rtw_read8(REG_ANAPAR_MAC_0) & ~0x07);
  _logger->info("Jaguar3: init_system_cfg done (ported, bw={} cut={})",
                static_cast<int>(bw), cut);
}

bool HalmacJaguar3MacInit::init_mac_cfg(ChannelWidth_t bw) {
  if (!init_trx_cfg())
    return false;
  init_protocol_cfg();
  init_edca_cfg(bw);
  init_wmac_cfg(bw);
  _logger->info("Jaguar3: HalMAC init_mac_cfg done (ported, bw={})",
                static_cast<int>(bw));
  return true;
}

/* init_trx_cfg_8822c */
bool HalmacJaguar3MacInit::init_trx_cfg() {
  /* txdma_queue_mapping_8822c: NORMAL trx, USB 3-bulkout. pq_map =
   * VO/VI->NQ(2), BE/BK->LQ(1), MG/HI->HQ(3). REG_TXDMA_PQ_MAP packs each as
   * 2-bit fields: VOQ<<4 VIQ<<6 BEQ<<8 BKQ<<10 MGQ<<12 HIQ<<14. */
  uint16_t pqmap = (3u << 14) | (3u << 12) | (1u << 10) | (1u << 8) |
                   (2u << 6) | (2u << 4); /* = 0xF5A0 */
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

/* priority_queue_cfg_8822c + set_trx_fifo_info_8822c + pg_num_parser */
bool HalmacJaguar3MacInit::priority_queue_cfg() {
  const uint16_t tx_fifo_pg_num = TX_FIFO_SIZE_8822C >> TX_PAGE_SHIFT; /* 2048 */
  const uint16_t rsvd_pg_num =
      RSVD_PG_DRV_NUM + RSVD_PG_H2C_EXTRAINFO_NUM + RSVD_PG_H2C_STATICINFO_NUM +
      RSVD_PG_H2CQ_NUM + RSVD_PG_CPU_INSTRUCTION_NUM + RSVD_PG_FW_TXBUF_NUM +
      RSVD_PG_CSIBUF_NUM; /* 110 */
  const uint16_t acq_pg_num = tx_fifo_pg_num - rsvd_pg_num; /* 1938 */
  const uint16_t rsvd_boundary = acq_pg_num;                /* 1938 */

  uint16_t cur = tx_fifo_pg_num;
  cur -= RSVD_PG_CSIBUF_NUM;
  const uint16_t rsvd_csibuf_addr = cur; /* 1998 */
  cur -= RSVD_PG_FW_TXBUF_NUM;
  cur -= RSVD_PG_CPU_INSTRUCTION_NUM;
  cur -= RSVD_PG_H2CQ_NUM;
  const uint16_t rsvd_h2cq_addr = cur; /* 1986 */

  const uint16_t pub_pg =
      acq_pg_num - PG_HQ - PG_NQ - PG_LQ - PG_EXQ - PG_GAP; /* 1745 */

  _device.rtw_write16(REG_FIFOPAGE_INFO_1, PG_HQ);
  _device.rtw_write16(REG_FIFOPAGE_INFO_2, PG_LQ);
  _device.rtw_write16(REG_FIFOPAGE_INFO_3, PG_NQ);
  _device.rtw_write16(REG_FIFOPAGE_INFO_4, PG_EXQ);
  _device.rtw_write16(REG_FIFOPAGE_INFO_5, pub_pg);
  _device.rtw_write32(REG_RQPN_CTRL_2,
                      _device.rtw_read32(REG_RQPN_CTRL_2) | (1u << 31));

  _device.rtw_write16(REG_FIFOPAGE_CTRL_2, rsvd_boundary);
  _device.rtw_write16(REG_WMAC_CSIDMA_CFG, rsvd_csibuf_addr);
  _device.rtw_write8(REG_FWHW_TXQ_CTRL + 2,
                     _device.rtw_read8(REG_FWHW_TXQ_CTRL + 2) | (1u << 4));
  _device.rtw_write16(REG_BCNQ_BDNY_V1, rsvd_boundary);
  _device.rtw_write16(REG_FIFOPAGE_CTRL_2 + 2, rsvd_boundary);
  _device.rtw_write16(REG_BCNQ1_BDNY_V1, rsvd_boundary);

  _device.rtw_write32(REG_RXFF_BNDY,
                      RX_FIFO_SIZE_8822C - C2H_PKT_BUF - 1); /* 0x5EFF */

  /* USB: block descriptor number + TXDMA offset check */
  uint8_t v8 = _device.rtw_read8(REG_AUTO_LLT_V1);
  v8 &= ~(0xf << 4);
  v8 |= (BLK_DESC_NUM << 4);
  _device.rtw_write8(REG_AUTO_LLT_V1, v8);
  _device.rtw_write8(REG_AUTO_LLT_V1 + 3, BLK_DESC_NUM);
  _device.rtw_write8(REG_TXDMA_OFFSET_CHK + 1,
                     _device.rtw_read8(REG_TXDMA_OFFSET_CHK + 1) | (1u << 1));

  /* auto-init LLT, poll for completion */
  _device.rtw_write8(REG_AUTO_LLT_V1,
                     _device.rtw_read8(REG_AUTO_LLT_V1) | BIT_AUTO_INIT_LLT_V1);
  uint32_t cnt = 1000;
  while (_device.rtw_read8(REG_AUTO_LLT_V1) & BIT_AUTO_INIT_LLT_V1) {
    if (--cnt == 0) {
      _logger->error("Jaguar3: init LLT auto-init timeout");
      return false;
    }
  }

  /* transfer mode NORMAL = 0 */
  _device.rtw_write8(REG_CR + 3, 0);
  return true;
}

/* init_h2c_8822c */
void HalmacJaguar3MacInit::init_h2c() {
  const uint16_t tx_fifo_pg_num = TX_FIFO_SIZE_8822C >> TX_PAGE_SHIFT;
  uint16_t cur = tx_fifo_pg_num - RSVD_PG_CSIBUF_NUM - RSVD_PG_FW_TXBUF_NUM -
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

void HalmacJaguar3MacInit::init_txq_ctrl() {
  uint8_t v8 = _device.rtw_read8(REG_FWHW_TXQ_CTRL);
  v8 |= (0x80 & ~0x02 & ~0x04); /* BIT(7) & ~BIT(1) & ~BIT(2) */
  _device.rtw_write8(REG_FWHW_TXQ_CTRL, v8);
  _device.rtw_write8(REG_FWHW_TXQ_CTRL + 1, WLAN_TXQ_RPT_EN);
}

void HalmacJaguar3MacInit::init_sifs_ctrl(ChannelWidth_t bw) {
  if (is_5m(bw)) {
    _device.rtw_write16(REG_RESP_SIFS_OFDM,
                        WLAN_SIFS_OFDM_CTX_5M | (WLAN_SIFS_OFDM_IRX_5M << 8));
    _device.rtw_write16(REG_SPEC_SIFS, WLAN_SIFS_DUR_TUNE_5M);
    _device.rtw_write32(REG_SIFS, WLAN_SIFS_CFG_5M);
    _device.rtw_write16(REG_MAC_SPEC_SIFS + 1, WLAN_SIFS_OFDM_5M);
  } else if (is_10m(bw)) {
    _device.rtw_write16(REG_RESP_SIFS_OFDM,
                        WLAN_SIFS_OFDM_CTX_10M | (WLAN_SIFS_OFDM_IRX_10M << 8));
    _device.rtw_write16(REG_SPEC_SIFS, WLAN_SIFS_DUR_TUNE_10M);
    _device.rtw_write32(REG_SIFS, WLAN_SIFS_CFG_10M);
    _device.rtw_write16(REG_MAC_SPEC_SIFS + 1, WLAN_SIFS_OFDM_10M);
  } else {
    _device.rtw_write16(REG_RESP_SIFS_OFDM,
                        WLAN_SIFS_OFDM_CTX | (WLAN_SIFS_OFDM_IRX << 8));
    _device.rtw_write16(REG_SPEC_SIFS, WLAN_SIFS_DUR_TUNE);
    _device.rtw_write32(REG_SIFS, WLAN_SIFS_CFG);
  }
  _device.rtw_write16(REG_RESP_SIFS_CCK,
                      WLAN_SIFS_CCK_CTX | (WLAN_SIFS_CCK_IRX << 8));
}

void HalmacJaguar3MacInit::init_rate_fallback_ctrl() {
  _device.rtw_write32(REG_DARFRC, WLAN_DATA_RATE_FB_CNT_1_4);
  _device.rtw_write32(REG_DARFRCH, WLAN_DATA_RATE_FB_CNT_5_8);
  _device.rtw_write32(REG_RARFRCH, WLAN_RTS_RATE_FB_CNT_5_8);
  _device.rtw_write32(REG_ARFR0, WLAN_DATA_RATE_FB_RATE0);
  _device.rtw_write32(REG_ARFRH0, WLAN_DATA_RATE_FB_RATE0_H);
  _device.rtw_write32(REG_ARFR1_V1, WLAN_RTS_RATE_FB_RATE1);
  _device.rtw_write32(REG_ARFRH1_V1, WLAN_RTS_RATE_FB_RATE1_H);
  _device.rtw_write32(REG_ARFR4, WLAN_RTS_RATE_FB_RATE4);
  _device.rtw_write32(REG_ARFRH4, WLAN_RTS_RATE_FB_RATE4_H);
  _device.rtw_write32(REG_ARFR5, WLAN_RTS_RATE_FB_RATE5);
  _device.rtw_write32(REG_ARFRH5, WLAN_RTS_RATE_FB_RATE5_H);
}

/* init_protocol_cfg_8822c */
void HalmacJaguar3MacInit::init_protocol_cfg() {
  init_txq_ctrl();
  init_sifs_ctrl(CHANNEL_WIDTH_20); /* protocol-cfg path always uses base SIFS;
                                       bw SIFS is re-applied in edca/sifs above */
  init_rate_fallback_ctrl();

  _device.rtw_write8(REG_AMPDU_MAX_TIME_V1, WLAN_AMPDU_MAX_TIME);
  _device.rtw_write8(REG_TX_HANG_CTRL,
                     _device.rtw_read8(REG_TX_HANG_CTRL) | 0x04); /* BIT_EN_EOF_V1 */

  _device.rtw_write8(REG_PRECNT_CTRL, static_cast<uint8_t>(WLAN_PRE_TXCNT & 0xFF));
  _device.rtw_write8(REG_PRECNT_CTRL + 1, static_cast<uint8_t>(WLAN_PRE_TXCNT >> 8));

  uint32_t v = WLAN_RTS_LEN_TH | (WLAN_RTS_TX_TIME_TH << 8) |
               (WLAN_MAX_AGG_PKT_LIMIT << 16) | (WLAN_RTS_MAX_AGG_PKT_LIMIT << 24);
  _device.rtw_write32(REG_PROT_MODE_CTRL, v);
  _device.rtw_write16(REG_BAR_MODE_CTRL + 2,
                      WLAN_BAR_RETRY_LIMIT | (WLAN_RA_TRY_RATE_AGG_LIMIT << 8));
  _device.rtw_write8(REG_FAST_EDCA_VOVI_SETTING, WLAN_FAST_EDCA_VO_TH);
  _device.rtw_write8(REG_FAST_EDCA_VOVI_SETTING + 2, WLAN_FAST_EDCA_VI_TH);
  _device.rtw_write8(REG_FAST_EDCA_BEBK_SETTING, WLAN_FAST_EDCA_BE_TH);
  _device.rtw_write8(REG_FAST_EDCA_BEBK_SETTING + 2, WLAN_FAST_EDCA_BK_TH);

  _device.rtw_write8(REG_LIFETIME_EN,
                     _device.rtw_read8(REG_LIFETIME_EN) & ~(1u << 5));

  v = _device.rtw_read32(REG_BF0_TIME_SETTING) & ~(1u << 29);
  _device.rtw_write32(REG_BF0_TIME_SETTING, v | (1u << 28));
  v = _device.rtw_read32(REG_BF1_TIME_SETTING) & ~(1u << 29);
  _device.rtw_write32(REG_BF1_TIME_SETTING, v | (1u << 28));
  v = _device.rtw_read32(REG_BF_TIMEOUT_EN) & ~(1u << 0) & ~(1u << 1);
  _device.rtw_write32(REG_BF_TIMEOUT_EN, v);

  v = _device.rtw_read32(REG_RRSR) & ~BITS_RRSR_RSC_8822C;
  _device.rtw_write32(REG_RRSR, v);
  _device.rtw_write8(REG_INIRTS_RATE_SEL,
                     _device.rtw_read8(REG_INIRTS_RATE_SEL) | (1u << 5));
}

void HalmacJaguar3MacInit::cfg_mac_clk() {
  _device.rtw_write32(REG_AFE_CTRL1,
                      _device.rtw_read32(REG_AFE_CTRL1) & ~(0x3u << 20));
  _device.rtw_write8(REG_USTIME_TSF, MAC_CLK_SPEED);
  _device.rtw_write8(REG_USTIME_EDCA, MAC_CLK_SPEED);
}

/* init_edca_cfg_8822c */
void HalmacJaguar3MacInit::init_edca_cfg(ChannelWidth_t bw) {
  if (is_5m(bw)) {
    _device.rtw_write8(REG_SLOT, WLAN_SLOT_TIME_5M);
    _device.rtw_write8(REG_PIFS, WLAN_PIFS_TIME_5M);
    _device.rtw_write32(REG_TBTT_PROHIBIT, WLAN_TBTT_TIME_5M);
    _device.rtw_write32(REG_EDCA_VO_PARAM, WLAN_EDCA_VO_PARAM_5M);
    _device.rtw_write32(REG_EDCA_VI_PARAM, WLAN_EDCA_VI_PARAM_5M);
    _device.rtw_write32(REG_EDCA_BE_PARAM, WLAN_EDCA_BE_PARAM_5M);
    _device.rtw_write32(REG_EDCA_BK_PARAM, WLAN_EDCA_BK_PARAM_5M);
    _device.rtw_write8(REG_CPUMGQ_PARAMETER, WLAN_CPUMGQ_AIFS_5M);
  } else if (is_10m(bw)) {
    _device.rtw_write8(REG_SLOT, WLAN_SLOT_TIME_10M);
    _device.rtw_write8(REG_PIFS, WLAN_PIFS_TIME_10M);
    _device.rtw_write32(REG_TBTT_PROHIBIT, WLAN_TBTT_TIME_10M);
    _device.rtw_write32(REG_EDCA_VO_PARAM, WLAN_EDCA_VO_PARAM_10M);
    _device.rtw_write32(REG_EDCA_VI_PARAM, WLAN_EDCA_VI_PARAM_10M);
    _device.rtw_write32(REG_EDCA_BE_PARAM, WLAN_EDCA_BE_PARAM_10M);
    _device.rtw_write32(REG_EDCA_BK_PARAM, WLAN_EDCA_BK_PARAM_10M);
    _device.rtw_write8(REG_CPUMGQ_PARAMETER, WLAN_CPUMGQ_AIFS_10M);
  } else {
    _device.rtw_write8(REG_SLOT, WLAN_SLOT_TIME);
    _device.rtw_write8(REG_PIFS, WLAN_PIFS_TIME);
    _device.rtw_write32(REG_TBTT_PROHIBIT, WLAN_TBTT_TIME);
    _device.rtw_write32(REG_EDCA_VO_PARAM, WLAN_EDCA_VO_PARAM);
    _device.rtw_write32(REG_EDCA_VI_PARAM, WLAN_EDCA_VI_PARAM);
    _device.rtw_write32(REG_EDCA_BE_PARAM, WLAN_EDCA_BE_PARAM);
    _device.rtw_write32(REG_EDCA_BK_PARAM, WLAN_EDCA_BK_PARAM);
  }

  _device.rtw_write8(REG_TX_PTCL_CTRL + 1,
                     _device.rtw_read8(REG_TX_PTCL_CTRL + 1) & ~(1u << 4));
  _device.rtw_write8(REG_RD_CTRL + 1,
                     _device.rtw_read8(REG_RD_CTRL + 1) | 0x07);
  cfg_mac_clk();
  _device.rtw_write8(REG_MISC_CTRL,
                     _device.rtw_read8(REG_MISC_CTRL) | 0x0B); /* BIT3|BIT1|BIT0 */
  _device.rtw_write8(REG_TIMER0_SRC_SEL,
                     _device.rtw_read8(REG_TIMER0_SRC_SEL) & ~0x70);
  _device.rtw_write16(REG_TXPAUSE, 0x0000);
  _device.rtw_write32(REG_RD_NAV_NXT, WLAN_NAV_CFG);
  _device.rtw_write16(REG_RXTSF_OFFSET_CCK, WLAN_RX_TSF_CFG);
  _device.rtw_write8(REG_BCN_CTRL,
                     _device.rtw_read8(REG_BCN_CTRL) | BIT_EN_BCN_FUNCTION);
  _device.rtw_write8(REG_DRVERLYINT, WLAN_DRV_EARLY_INT);
  _device.rtw_write8(REG_BCN_CTRL_CLINT0, WLAN_BCN_CTRL_CLT0);
  _device.rtw_write8(REG_BCNDMATIM, WLAN_BCN_DMA_TIME);
  _device.rtw_write8(REG_BCN_MAX_ERR, WLAN_BCN_MAX_ERR);
  _device.rtw_write8(REG_BAR_TX_CTRL,
                     _device.rtw_read8(REG_BAR_TX_CTRL) | 0x01);
}

/* init_wmac_cfg_8822c (halmac_init_low_pwr omitted — USB-LPS power feature) */
void HalmacJaguar3MacInit::init_wmac_cfg(ChannelWidth_t bw) {
  if (is_5m(bw)) {
    _device.rtw_write8(REG_ACKTO, WLAN_ACK_TO_5M);
    _device.rtw_write8(REG_CTS2TO, WLAN_CTS2TO_5M);
    _device.rtw_write16(REG_EIFS, WLAN_EIFS_DUR_TUNE_5M);
  } else if (is_10m(bw)) {
    _device.rtw_write8(REG_ACKTO, WLAN_ACK_TO_10M);
    _device.rtw_write8(REG_CTS2TO, WLAN_CTS2TO_10M);
    _device.rtw_write16(REG_EIFS, WLAN_EIFS_DUR_TUNE_10M);
  } else {
    _device.rtw_write8(REG_ACKTO, WLAN_ACK_TO);
    _device.rtw_write16(REG_EIFS, WLAN_EIFS_DUR_TUNE);
  }

  _device.rtw_write32(REG_MAR, 0xFFFFFFFF);
  _device.rtw_write32(REG_MAR + 4, 0xFFFFFFFF);
  _device.rtw_write8(REG_BBPSF_CTRL + 2, WLAN_RESP_TXRATE);
  _device.rtw_write8(REG_ACKTO_CCK, WLAN_ACK_TO_CCK);
  _device.rtw_write8(REG_NAV_CTRL + 2, WLAN_NAV_MAX);
  _device.rtw_write8(REG_WMAC_TRXPTCL_CTL_H,
                     _device.rtw_read8(REG_WMAC_TRXPTCL_CTL_H) |
                         BIT_EN_TXCTS_IN_RXNAV_V1);
  _device.rtw_write8(REG_WMAC_TRXPTCL_CTL_H + 2, WLAN_BAR_ACK_TYPE);
  _device.rtw_write32(REG_RXFLTMAP0, WLAN_RX_FILTER0);
  _device.rtw_write16(REG_RXFLTMAP2, WLAN_RX_FILTER2);
  _device.rtw_write32(REG_RCR, WLAN_RCR_CFG);
  _device.rtw_write8(REG_RXPSF_CTRL + 2,
                     _device.rtw_read8(REG_RXPSF_CTRL + 2) | 0x0e);
  _device.rtw_write8(REG_RX_PKT_LIMIT, WLAN_RXPKT_MAX_SZ_512);
  _device.rtw_write8(REG_TCR + 2, WLAN_TX_FUNC_CFG2);
  _device.rtw_write8(REG_TCR + 1, WLAN_TX_FUNC_CFG1);
  _device.rtw_write16(REG_GENERAL_OPTION,
                      _device.rtw_read16(REG_GENERAL_OPTION) |
                          BIT_DUMMY_FCS_READY_MASK_EN | BIT_RXFIFO_GNT_CUT);
  _device.rtw_write8(REG_SND_PTCL_CTRL,
                     _device.rtw_read8(REG_SND_PTCL_CTRL) |
                         BIT_R_DISABLE_CHECK_VHTSIGB_CRC);
  _device.rtw_write32(REG_WMAC_OPTION_FUNCTION_2, WLAN_MAC_OPT_FUNC2);
  _device.rtw_write8(REG_WMAC_OPTION_FUNCTION_1, WLAN_MAC_OPT_NORM_FUNC1);
}

} /* namespace jaguar3 */
