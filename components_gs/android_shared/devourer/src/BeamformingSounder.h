/* BeamformingSounder — Realtek 802.11ac explicit-sounding helpers for the
 * two-adapter "self-sounding" CSI path, shared across chip generations.
 *
 * With one adapter as beamformer and another as beamformee, an NDPA +
 * hardware-NDP exchange makes the beamformee emit a VHT Compressed Beamforming
 * report over the air, which a monitor RX captures — giving per-subcarrier
 * channel state the chip otherwise terminates in hardware. See
 * docs/beamforming-self-sounding.md.
 *
 * WHY THIS IS GENERATION-NEUTRAL: the MAC sounding registers are byte-identical
 * across the whole Realtek AC family — kSndPtclCtrl (0x718),
 * kBfmer0Info (0x6E4), REG_CSI_RPT_PARAM_BW20 (0x6F4), kBfmeeSel
 * (0x714), kTxbfCtrl (0x42C) are the same on 8192E/8703B/8723D/8812A/
 * 8814A/8822B/8822C/8822E (verified against every vendor include tree). The
 * beamformer arm (arm_sounder) is therefore fully shared. Only the beamformee
 * PHY details differ per generation, and those are captured in BfeeConfig:
 *   - the sounding-protocol control byte (0xCB Jaguar-1 / 0xDB Jaguar-2/3);
 *   - the BB CSI-content register 0x9B4 is CSI content on Jaguar-1 but the
 *     NARROWBAND CLOCK DIVIDER on Jaguar-3 (RadioManagementJaguar3.h) — it
 *     MUST NOT be written there, so csi_content_reg is 0 on Jaguar-2/3;
 *   - Jaguar-2/3 additionally gate the responder behind RX-filter bits
 *     (accept NDPA + action-no-ack) and an own-AID register (0x1680).
 *
 * Register values transcribed from the vendor beamformee-entry functions:
 *   Jaguar-1   hal_txbf_jaguar_enter()  (haltxbfjaguar.c)
 *   Jaguar-2/3 hal_txbf_8822b_enter()   (haltxbf8822b.c, shared 8822B/C/E)
 * Entry 0, P_AID = 0 (unassociated sounding — no CAM/macid entry).
 */
#ifndef BEAMFORMING_SOUNDER_H
#define BEAMFORMING_SOUNDER_H

#include <cstdint>

#include "RtlAdapter.h"

namespace devourer {
namespace bf {

/* Shared MAC sounding registers (identical across the Realtek AC family).
 * k-prefixed to avoid colliding with the REG_* preprocessor macros in the
 * vendor hal_com_reg.h (macros ignore namespaces). */
enum : uint16_t {
  kTxbfCtrl        = 0x042C,
  kRxFltMap0       = 0x06A0, /* +1 bit6 = accept action-no-ack           */
  kRxFltMap1       = 0x06A2, /* [5:4] = accept NDPA + BF report poll      */
  kBfmer0Info      = 0x06E4,
  kCsiRptParam20   = 0x06F4,
  kCsiRptParam40   = 0x06F8,
  kCsiRptParam80   = 0x06FC,
  kBfmeeSel        = 0x0714,
  kSndPtclCtrl     = 0x0718,
  kSndNdpStandby   = 0x071B,
  kBbCsiContent    = 0x09B4, /* Jaguar-1 only (clock divider on Jaguar-3) */
  kOurAid          = 0x1680, /* Jaguar-2/3 only: our AID [11:0]           */
  kBbpsfCtrl       = 0x06DC, /* CSI GID select (BIT30 + gid BIT12)        */
  kCsiRrsr         = 0x1678, /* CSI response-rate set (= 0x550)           */
};

enum : uint8_t {
  kTxbfNdpaEnables = 0x10 | 0x40 | 0x80, /* 0x42F BIT4|6|7: NDPA TX enable */
  kRxFlt0ActNoAck  = 0x40,               /* 0x6A1 bit6                     */
  kRxFlt1NdpaPoll  = 0x30,               /* 0x6A2 [5:4]                    */
};

/* Per-generation beamformee knobs. */
struct BfeeConfig {
  uint8_t  snd_ptcl_ctrl;      /* 0xCB Jaguar-1 / 0xDB Jaguar-2/3            */
  uint8_t  ndp_standby;        /* 0x50                                       */
  uint32_t csi_rpt_param;      /* report matrix dims (1T1R VHT SU = 0x0109)  */
  bool     csi_rpt_param_16;   /* 16-bit @0x6F4 (Jg2/3) vs 32-bit x3 (Jg1)   */
  uint16_t csi_content_reg;    /* 0x9B4 on Jaguar-1; 0 = skip on Jaguar-2/3  */
  uint32_t csi_content_val;
  bool     set_our_aid;        /* write 0 to 0x1680 (Jaguar-2/3)             */
  bool     set_rxflt;          /* enable NDPA/action-no-ack RX (Jaguar-2/3)  */
};

/* Jaguar-1 (8812/8814/8821): BB CSI-content at 0x9B4, 32-bit params, no
 * RX-filter poke (monitor mode already accepts the frames). */
constexpr BfeeConfig kBfeeJaguar1{
    /*snd_ptcl_ctrl*/ 0xCB, /*ndp_standby*/ 0x50,
    /*csi_rpt_param*/ 0x01080108, /*csi_rpt_param_16*/ false,
    /*csi_content_reg*/ kBbCsiContent, /*csi_content_val*/ 0x01081008,
    /*set_our_aid*/ false, /*set_rxflt*/ false};

/* Jaguar-2/3 (8822B/C/E): 0xDB, 16-bit param at 0x6F4, own-AID + RX-filter
 * gates, and NO 0x9B4 write (that address is the narrowband clock divider). */
constexpr BfeeConfig kBfeeJaguar23{
    /*snd_ptcl_ctrl*/ 0xDB, /*ndp_standby*/ 0x50,
    /*csi_rpt_param*/ 0x0109, /*csi_rpt_param_16*/ true,
    /*csi_content_reg*/ 0, /*csi_content_val*/ 0,
    /*set_our_aid*/ true, /*set_rxflt*/ true};

/* Beamformer side (shared): arm the MAC sounding engine so a
 * TX-descriptor-marked NDPA is followed by a hardware-generated NDP.
 * Beamformee entry 0, P_AID = 0 (unassociated). The one per-generation byte is
 * the sounding-protocol control (0x718): 0xCB on Jaguar-1
 * (hal_txbf_jaguar_enter), 0xDB on Jaguar-2/3 (hal_txbf_8822b_enter). */
inline void arm_sounder(RtlAdapter& dev, uint8_t snd_ptcl_ctrl = 0xCB) {
  dev.rtw_write8(kSndPtclCtrl, snd_ptcl_ctrl);
  dev.rtw_write8(kSndNdpStandby, 0x50);
  dev.rtw_write16(kTxbfCtrl, 0x0000);                 /* P_AID = 0 */
  uint8_t v = dev.rtw_read8(kTxbfCtrl + 3);
  dev.rtw_write8(kTxbfCtrl + 3, v | kTxbfNdpaEnables);
  uint8_t s = dev.rtw_read8(kBfmeeSel + 3);
  dev.rtw_write8(kBfmeeSel + 3, (s & 0x03) | 0x60);
  dev.rtw_write16(kBfmeeSel, 0x0200);                 /* entry-0 select */
}

/* Beamformee side: arm the hardware CSI responder to reply to NDPA+NDP from
 * `beamformer_mac` (matched against the NDPA TA), no association required.
 * `cfg` selects the per-generation register recipe. */
inline void arm_beamformee(RtlAdapter& dev, const uint8_t beamformer_mac[6],
                           const BfeeConfig& cfg) {
  dev.rtw_write8(kSndPtclCtrl, cfg.snd_ptcl_ctrl);
  dev.rtw_write8(kSndNdpStandby, cfg.ndp_standby);
  for (uint16_t i = 0; i < 6; ++i)
    dev.rtw_write8(kBfmer0Info + i, beamformer_mac[i]);
  if (cfg.set_our_aid)
    dev.rtw_write16(kOurAid, 0x0000);                 /* our AID = 0 */
  if (cfg.set_rxflt) {
    uint8_t r0 = dev.rtw_read8(kRxFltMap0 + 1);
    dev.rtw_write8(kRxFltMap0 + 1, r0 | kRxFlt0ActNoAck);
    uint8_t r1 = dev.rtw_read8(kRxFltMap1);
    dev.rtw_write8(kRxFltMap1, r1 | kRxFlt1NdpaPoll);
  }
  if (cfg.csi_rpt_param_16) {
    dev.rtw_write16(kCsiRptParam20,
                    static_cast<uint16_t>(cfg.csi_rpt_param));
  } else {
    dev.rtw_write32(kCsiRptParam20, cfg.csi_rpt_param);
    dev.rtw_write32(kCsiRptParam40, cfg.csi_rpt_param);
    dev.rtw_write32(kCsiRptParam80, cfg.csi_rpt_param);
  }
  if (cfg.csi_content_reg)
    dev.rtw_write32(cfg.csi_content_reg, cfg.csi_content_val);
}

/* Beamformer-side steering entry — the TX-BF *apply* half, layered on top of
 * arm_sounder() (which already set SND_PTCL_CTRL, the TXBF_CTRL NDPA-enables +
 * P_AID=0, and BFMEE_SEL entry-0/TXUSER_ID0). This configures the entry so the
 * WMAC accepts the beamformee's Compressed Beamforming Report and builds the V
 * matrix from it IN HARDWARE (software never touches coefficients — see
 * reference/rtl88x2cu/hal/rtl8822c/rtl8822c_bf_monitor.c bf_monitor_config_*).
 * `peer_mac` = the beamformee we steer toward (each side writes the other's MAC
 * into BFMER0_INFO). `rx_nss` = GET_RX_NSS (2 on the 2T2R 8822B/C/E). Registers
 * are family-shared, so this is generation-neutral like arm_sounder. Apply is
 * NOT enabled here — call apply_vmatrix() once a CBR has been ingested. */
inline void arm_beamformer_entry(RtlAdapter& dev, const uint8_t peer_mac[6],
                                 uint8_t rx_nss = 2, uint8_t g_id = 0) {
  for (uint16_t i = 0; i < 6; ++i)
    dev.rtw_write8(kBfmer0Info + i, peer_mac[i]);
  /* Expected CSI-report dims: nc=rx_nss-1, nr=1, grouping=0, codebook=1 (vht),
   * coeff=3 (bf_monitor_config_beamformer). */
  const uint8_t nc = rx_nss ? static_cast<uint8_t>(rx_nss - 1) : 0;
  const uint16_t csi_param =
      static_cast<uint16_t>((3u << 10) | (1u << 8) | (1u << 3) | nc);
  dev.rtw_write16(kCsiRptParam20, csi_param);
  dev.rtw_write8(kSndNdpStandby, 0x70);           /* ndp_rx_standby_timer */
  uint32_t bbpsf = dev.rtw_read<uint32_t>(kBbpsfCtrl) | (1u << 30);
  bbpsf = (g_id == 63) ? (bbpsf | (1u << 12)) : (bbpsf & ~(1u << 12));
  dev.rtw_write<uint32_t>(kBbpsfCtrl, bbpsf);
  dev.rtw_write<uint32_t>(kCsiRrsr, 0x550);
}

/* Flip the V-matrix apply toggle: TXBF_CTRL[11:9] per bandwidth (BIT9=20,
 * +BIT10=40, +BIT11=80) + DIS_NDP_BFEN (BIT15). Enable ONLY after the WMAC has
 * ingested a valid CBR (bf_monitor_enable_txbf gates on csi_matrix_len>0) —
 * enabling with no V degrades the link. `bw`: 0=20, 1=40, 2=80. */
inline void apply_vmatrix(RtlAdapter& dev, bool enable, uint8_t bw) {
  uint32_t txbf = dev.rtw_read<uint32_t>(kTxbfCtrl);
  txbf &= ~((1u << 9) | (1u << 10) | (1u << 11));
  if (enable) {
    txbf |= (1u << 9);
    if (bw >= 1) txbf |= (1u << 10);
    if (bw >= 2) txbf |= (1u << 11);
    txbf |= (1u << 15);                           /* BIT_DIS_NDP_BFEN */
  }
  dev.rtw_write<uint32_t>(kTxbfCtrl, txbf);
}

/* Jaguar-2/3 MU-beamformee registers (8822B/C/E). MU-BF group table. */
enum : uint16_t {
  kMuTxCtl    = 0x14C0, /* [10:8] STA-table index, [5:0] STA validity      */
  kMuGidTab   = 0x14C4, /* gid_valid table (reset to 0 here)               */
  kMuUserPosL = 0x14C8, /* user-position low 32                            */
  kMuUserPosH = 0x14CC, /* user-position high 32                           */
  kMuBfmee0   = 0x1684, /* MU BFee entry 0 (2 bytes): [8:0] P_AID, BIT9 en */
  kNdpaRate   = 0x045D, /* NDPA TX rate                                    */
  kNdpaOpt    = 0x045F, /* NDPA option ctrl                                */
};

/* Beamformee MU layer: on top of arm_beamformee (which sets the SU responder
 * base — sounding enable, beamformer MAC, RX-filter), add the MU group-table
 * registers so the beamformee emits an MU report (which appends the per-tone
 * delta-SNR "MU Exclusive Beamforming Report" the SU report omits). For
 * self-sounding we program the group/user-position tables directly, skipping
 * the over-the-air VHT Group ID Management handshake the vendor normally does.
 * Recipe from hal_txbf_8822b_enter() MU BFee branch (haltxbf8822b.c:484-598),
 * MU register index 0: gid_valid=0x7fe, user_position_l=0x111110, p_aid=0. */
inline void arm_beamformee_mu(RtlAdapter& dev, const uint8_t beamformer_mac[6],
                              const BfeeConfig& cfg) {
  arm_beamformee(dev, beamformer_mac, cfg);            /* SU responder base */

  /* select MU STA table index 0 (0x14C0[10:8]=0), then mark it valid ([0]=1) */
  uint32_t mtc = dev.rtw_read<uint32_t>(kMuTxCtl);
  mtc &= ~0x0700u;                                     /* index 0 */
  dev.rtw_write<uint32_t>(kMuTxCtl, mtc);
  mtc = (mtc & 0xFFFFFFC0u) | 0x01u;                   /* STA-0 valid */
  dev.rtw_write<uint32_t>(kMuTxCtl, mtc);

  dev.rtw_write<uint32_t>(kMuGidTab, 0x00000000u);     /* reset gid table */
  dev.rtw_write<uint32_t>(kMuUserPosL, 0x00111110u);   /* index-0 user pos */
  dev.rtw_write<uint32_t>(kMuUserPosH, 0x00000000u);

  uint16_t e = dev.rtw_read<uint16_t>(kMuBfmee0);
  e = (e & 0xFE00u) | 0x0200u;                         /* BIT9 enable, P_AID=0 */
  dev.rtw_write16(kMuBfmee0, e);

  uint8_t t = dev.rtw_read8(kTxbfCtrl + 3);
  dev.rtw_write8(kTxbfCtrl + 3, t | 0xD0);             /* CSI-report src bits */
  dev.rtw_write8(kNdpaRate, 0x04);                     /* NDPA 6M */
  uint8_t o = dev.rtw_read8(kNdpaOpt);
  dev.rtw_write8(kNdpaOpt, o & 0xFC);
  uint32_t sp = dev.rtw_read<uint32_t>(kSndPtclCtrl);
  dev.rtw_write<uint32_t>(kSndPtclCtrl, (sp & 0xFF0000FFu) | 0x00020200u);
}

}  // namespace bf
}  // namespace devourer

#endif  // BEAMFORMING_SOUNDER_H
