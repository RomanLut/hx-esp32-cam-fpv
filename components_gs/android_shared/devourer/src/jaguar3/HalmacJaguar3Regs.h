#ifndef HALMAC_8822C_REGS_H
#define HALMAC_8822C_REGS_H

#include <cstdint>

/* Focused subset of Realtek HalMAC register / bit / firmware-header constants
 * needed by the Jaguar3 DLFW (firmware-download) state machine. Transcribed
 * verbatim from the vendor halmac headers (libc0607/rtl88x2cu-20230728):
 *   halmac_reg2.h / halmac_reg_8822c.h  — register offsets
 *   halmac_bit2.h / halmac_bit_8822c.h  — bit masks
 *   halmac_fw_info.h                    — WLAN_FW_HDR_* field offsets
 *   halmac_88xx/halmac_88xx_cfg.h       — sizes / OCP bases / poll counts
 *
 * Kept as a focused header (not a wholesale halmac import) per devourer's
 * minimal-subset philosophy. Only what HalmacJaguar3Fw uses lives here.
 *
 * Several of these names (REG_CR, REG_BCN_CTRL, ...) are also #define macros in
 * the Realtek hal_com_reg.h pulled in via RtlAdapter.h. The preprocessor
 * ignores namespaces, so we #undef them first and re-introduce them as scoped
 * constexpr constants. This header is included ONLY by HalmacJaguar3Fw.cpp (which
 * uses no other definition of these names), so the #undef is local and safe. */
#undef REG_SYS_FUNC_EN
#undef REG_SYS_CLK_CTRL
#undef REG_RSV_CTRL
#undef REG_MCUFW_CTRL
#undef REG_MCU_TST_CFG
#undef REG_CR
#undef REG_TXDMA_PQ_MAP
#undef REG_HMETFR
#undef REG_TXDMA_STATUS
#undef REG_RQPN_CTRL_2
#undef REG_FIFOPAGE_INFO_1
#undef REG_BCN_CTRL
#undef REG_CPU_DMEM_CON
#undef REG_FW_DBG6
#undef REG_FW_DBG7
#undef REG_DDMA_CH0SA
#undef REG_DDMA_CH0DA
#undef REG_DDMA_CH0CTRL
#undef REG_H2CQ_CSR
#undef REG_FIFOPAGE_CTRL_2
#undef REG_FWHW_TXQ_CTRL

namespace jaguar3::halmac {

/* --- registers --- */
constexpr uint16_t REG_SYS_FUNC_EN   = 0x0002;
constexpr uint16_t REG_SYS_CLK_CTRL  = 0x0008;
constexpr uint16_t REG_RSV_CTRL      = 0x001C;
constexpr uint16_t REG_MCUFW_CTRL    = 0x0080;
constexpr uint16_t REG_MCU_TST_CFG   = 0x0084;
constexpr uint16_t REG_CR            = 0x0100;
constexpr uint16_t REG_TXDMA_PQ_MAP  = 0x010C;
constexpr uint16_t REG_HMETFR        = 0x01CC;
constexpr uint16_t REG_TXDMA_STATUS  = 0x0210;
constexpr uint16_t REG_RQPN_CTRL_2   = 0x022C;
constexpr uint16_t REG_FIFOPAGE_INFO_1 = 0x0230;
constexpr uint16_t REG_BCN_CTRL      = 0x0550;
constexpr uint16_t REG_CPU_DMEM_CON  = 0x1080;
constexpr uint16_t REG_FW_DBG6       = 0x10F8;
constexpr uint16_t REG_FW_DBG7       = 0x10FC;
constexpr uint16_t REG_DDMA_CH0SA    = 0x1200;
constexpr uint16_t REG_DDMA_CH0DA    = 0x1204;
constexpr uint16_t REG_DDMA_CH0CTRL  = 0x1208;
constexpr uint16_t REG_H2CQ_CSR      = 0x1330;
constexpr uint16_t REG_FIFOPAGE_CTRL_2 = 0x0204; /* BCN/rsvd-page head, BIT15=dl */
constexpr uint16_t REG_FWHW_TXQ_CTRL = 0x0420;

/* rsvd-page download */
constexpr uint16_t BIT_MASK_BCN_HEAD_1_V1 = 0xfff;
constexpr uint8_t QSEL_BEACON = 0x10; /* TX-desc QSEL for a beacon/rsvd-page pkt */

/* --- bit masks --- */
constexpr uint32_t BIT_HCI_TXDMA_EN  = 1u << 0;
constexpr uint32_t BIT_TXDMA_EN      = 1u << 2;
constexpr uint32_t BIT_FW_DW_RDY     = 1u << 14;
constexpr uint32_t BIT_DDMACH0_CHKSUM_CONT      = 1u << 24;
constexpr uint32_t BIT_DDMACH0_RESET_CHKSUM_STS = 1u << 25;
constexpr uint32_t BIT_DDMACH0_CHKSUM_STS       = 1u << 27;
constexpr uint32_t BIT_DDMACH0_CHKSUM_EN        = 1u << 29;
constexpr uint32_t BIT_DDMACH0_OWN              = 1u << 31;
constexpr uint32_t BIT_MASK_DDMACH0_DLEN        = 0x3ffff;
constexpr uint32_t BIT_IMEM_DW_OK    = 1u << 3;
constexpr uint32_t BIT_IMEM_CHKSUM_OK = 1u << 4;
constexpr uint32_t BIT_DMEM_DW_OK    = 1u << 5;
constexpr uint32_t BIT_DMEM_CHKSUM_OK = 1u << 6;

/* --- misc constants --- */
constexpr uint32_t HALMAC_DMA_MAPPING_HIGH = 3;
constexpr uint32_t OCPBASE_TXBUF_88XX = 0x18780000;
constexpr uint32_t OCPBASE_DMEM_88XX  = 0x00200000;
constexpr uint32_t HALMC_DDMA_POLLING_COUNT = 1000;
constexpr uint32_t TX_DESC_SIZE_88XX  = 48;
constexpr uint32_t ILLEGAL_KEY_GROUP  = 0xFAAAAA00;

/* --- WLAN firmware header field byte offsets (header is WLAN_FW_HDR_SIZE) --- */
constexpr uint32_t WLAN_FW_HDR_SIZE        = 64;
constexpr uint32_t WLAN_FW_HDR_CHKSUM_SIZE = 8;
constexpr uint32_t WLAN_FW_HDR_MEM_USAGE   = 24;
constexpr uint32_t WLAN_FW_HDR_DMEM_ADDR   = 32;
constexpr uint32_t WLAN_FW_HDR_DMEM_SIZE   = 36;
constexpr uint32_t WLAN_FW_HDR_IMEM_SIZE   = 48;
constexpr uint32_t WLAN_FW_HDR_EMEM_SIZE   = 52;
constexpr uint32_t WLAN_FW_HDR_EMEM_ADDR   = 56;
constexpr uint32_t WLAN_FW_HDR_IMEM_ADDR   = 60;

} /* namespace jaguar3::halmac */

#endif /* HALMAC_8822C_REGS_H */
