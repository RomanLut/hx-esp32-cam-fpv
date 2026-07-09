#include "FrameParser.h"

#define CONFIG_USB_RX_AGGREGATION 1

#define RXDESC_SIZE 24

#include "basic_types.h"
#include "rtl8812a_recv.h"

#include <string.h>
#include <vector>

struct _phy_status_rpt_8812 {
  /*	DWORD 0*/
  u8 gain_trsw[2];    /*path-A and path-B {TRSW, gain[6:0] }*/
  u8 chl_num_LSB;     /*channel number[7:0]*/
  u8 chl_num_MSB : 2; /*channel number[9:8]*/
  u8 sub_chnl : 4;    /*sub-channel location[3:0]*/
  u8 r_RFMOD : 2;     /*RF mode[1:0]*/

  /*	DWORD 1*/
  u8 pwdb_all;  /*CCK signal quality / OFDM pwdb all*/
  s8 cfosho[2]; /*DW1 byte 1 DW1 byte2	CCK AGC report and CCK_BB_Power /
                   OFDM path-A and path-B short CFO*/
  /*this should be checked again because the definition of 8812 and 8814
   * is different*/
  /*	u8			r_cck_rx_enable_pathc:2;					cck rx enable pathc[1:0]*/
  /*	u8			cck_rx_path:4;							cck rx path[3:0]*/
  u8 resvd_0 : 6;
  u8 bt_RF_ch_MSB : 2; /*8812A:2'b0			8814A: bt rf channel keep[7:6]*/
  u8 ant_div_sw_a : 1; /*8812A: ant_div_sw_a    8814A: 1'b0*/
  u8 ant_div_sw_b : 1; /*8812A: ant_div_sw_b    8814A: 1'b0*/
  u8 bt_RF_ch_LSB : 6; /*8812A: 6'b0                   8814A: bt rf
                          channel keep[5:0]*/
  s8 cfotail[2];       /*DW2 byte 1 DW2 byte 2	path-A and path-B CFO tail*/
  u8 PCTS_MSK_RPT_0;   /*PCTS mask report[7:0]*/
  u8 PCTS_MSK_RPT_1;   /*PCTS mask report[15:8]*/

  /*	DWORD 3*/
  s8 rxevm[2]; /*DW3 byte 1 DW3 byte 2	stream 1 and stream 2 RX EVM*/
  s8 rxsnr[2]; /*DW3 byte 3 DW4 byte 0	path-A and path-B RX SNR*/

  /*	DWORD 4*/
  u8 PCTS_MSK_RPT_2;     /*PCTS mask report[23:16]*/
  u8 PCTS_MSK_RPT_3 : 6; /*PCTS mask report[29:24]*/
  u8 pcts_rpt_valid : 1; /*pcts_rpt_valid*/
  u8 resvd_1 : 1;        /*1'b0*/
  s8 rxevm_cd[2];        /*DW 4 byte 3 DW5 byte 0  8812A: 16'b0	8814A: stream 3
                            and stream 4 RX EVM*/

  /*	DWORD 5*/
  u8 csi_current[2];  /*DW5 byte 1 DW5 byte 2	8812A: stream 1 and 2 CSI
                         8814A:  path-C and path-D RX SNR*/
  u8 gain_trsw_cd[2]; /*DW5 byte 3 DW6 byte 0	path-C and path-D {TRSW,
                         gain[6:0] }*/

  /*	DWORD 6*/
  s8 sigevm;             /*signal field EVM*/
  u8 antidx_antc : 3;    /*8812A: 3'b0		8814A: antidx_antc[2:0]*/
  u8 antidx_antd : 3;    /*8812A: 3'b0		8814A: antidx_antd[2:0]*/
  u8 dpdt_ctrl_keep : 1; /*8812A: 1'b0		8814A: dpdt_ctrl_keep*/
  u8 GNT_BT_keep : 1;    /*8812A: 1'b0		8814A: GNT_BT_keep*/
  u8 antidx_anta : 3;    /*antidx_anta[2:0]*/
  u8 antidx_antb : 3;    /*antidx_antb[2:0]*/
  u8 hw_antsw_occur : 2; /*1'b0*/
};

FrameParser::FrameParser(Logger_t logger) : _logger{logger} {}

static rx_pkt_attrib rtl8812_query_rx_desc_status(uint8_t *pdesc) {
  auto pattrib = rx_pkt_attrib{};

  /* Offset 0 */
  pattrib.pkt_len = GET_RX_STATUS_DESC_PKT_LEN_8812(pdesc);
  pattrib.crc_err = GET_RX_STATUS_DESC_CRC32_8812(pdesc);
  pattrib.icv_err = GET_RX_STATUS_DESC_ICV_8812(pdesc);
  pattrib.drvinfo_sz =
      (uint8_t)(GET_RX_STATUS_DESC_DRVINFO_SIZE_8812(pdesc) * 8);
  pattrib.encrypt = GET_RX_STATUS_DESC_SECURITY_8812(pdesc);
  pattrib.qos = GET_RX_STATUS_DESC_QOS_8812(pdesc);
  /* Qos data, wireless
                                                          lan header length is
                                                          26 */
  pattrib.shift_sz = GET_RX_STATUS_DESC_SHIFT_8812(
      pdesc); /* ((le32_to_cpu(pdesc.rxdw0) >> 24) & 0x3); */
  pattrib.physt = GET_RX_STATUS_DESC_PHY_STATUS_8812(
      pdesc); /* ((le32_to_cpu(pdesc.rxdw0) >> 26) & 0x1); */
  pattrib.bdecrypted = !GET_RX_STATUS_DESC_SWDEC_8812(
      pdesc); /* (le32_to_cpu(pdesc.rxdw0) & BIT(27))? 0:1; */

  /* Offset 4 */
  pattrib.priority = GET_RX_STATUS_DESC_TID_8812(pdesc);
  pattrib.mdata = GET_RX_STATUS_DESC_MORE_DATA_8812(pdesc);
  pattrib.mfrag = GET_RX_STATUS_DESC_MORE_FRAG_8812(pdesc);
  /* more fragment bit */

  /* Offset 8 */
  pattrib.seq_num = GET_RX_STATUS_DESC_SEQ_8812(pdesc);
  pattrib.frag_num = GET_RX_STATUS_DESC_FRAG_8812(pdesc);
  /* fragmentation number */

  if (GET_RX_STATUS_DESC_RPT_SEL_8812(pdesc)) {
    pattrib.pkt_rpt_type = RX_PACKET_TYPE::C2H_PACKET;
  } else {
    pattrib.pkt_rpt_type = RX_PACKET_TYPE::NORMAL_RX;
  }

  /* Offset 12 */
  pattrib.data_rate = GET_RX_STATUS_DESC_RX_RATE_8812(pdesc);
  /* Offset 16 — 8812/8821 ONLY. On 8814A this DWORD holds
   * PATTERN_IDX[7:0] / RX_EOF[8] / RX_SCRAMBLER[15:9]
   * (rtl8814a_recv.h:148-150) and the kernel's 8814 rx-desc query never
   * reads SGI/LDPC/STBC/BW from the descriptor at all (it sets
   * bw = CHANNEL_WIDTH_MAX). These four attribs are therefore garbage
   * when the RX chip is an 8814 — no current consumer reads them, but
   * don't trust them there without chip-gating first. */
  pattrib.sgi = GET_RX_STATUS_DESC_SPLCP_8812(pdesc);
  pattrib.ldpc = GET_RX_STATUS_DESC_LDPC_8812(pdesc);
  pattrib.stbc = GET_RX_STATUS_DESC_STBC_8812(pdesc);
  pattrib.bw = GET_RX_STATUS_DESC_BW_8812(pdesc);

  /* Descrambler seed recovered by the chip from the RX SERVICE field. This is
   * read at the RTL8814AU descriptor offset (DWORD4 bits 9-15, i.e.
   * GET_RX_STATUS_DESC_RX_SCRAMBLER_8814A in hal/rtl8814a_recv.h). NOTE: the
   * 8812/8821 RX status descriptor does NOT lay the scrambler out here — that
   * region holds the rate-info bits read just above (SPLCP/LDPC/STBC/BW occupy
   * bits 0-5). The field is therefore only meaningful when the RX chip is an
   * RTL8814AU; on 8812/8821 it reflects reserved/other bits and must not be
   * trusted. Used solely by the DEVOURER_DUMP_SCRAMBLER diagnostic — for the
   * precoder PoC (which targets 8812/8821 TX) the brute-force seed search in
   * tools/precoder/seed_probe.py is the reliable path. */
  pattrib.scrambler = (uint8_t)LE_BITS_TO_4BYTE(pdesc + 16, 9, 7);

  /* Offset 20 — chip-side TSF low (32 bits). Surfaced via RxAtrib.tsfl
   * for downstream latency measurement and dup-detection (see
   * examples/rx/main.cpp's rx.frame event). The macro reads bits 0..31
   * of pdesc+20 (full 4-byte u32), not a byte — the original commented
   * `(byte)` cast in master was a copy-paste from another field. */
  pattrib.tsfl = GET_RX_STATUS_DESC_TSFL_8812(pdesc);

  return pattrib;
}

__inline static u32 _RND8(u32 sz) {
  u32 val;

  val = ((sz >> 3) + ((sz & 7) ? 1 : 0)) << 3;

  return val;
}

std::vector<Packet> FrameParser::recvbuf2recvframe(std::span<uint8_t> ptr) {
  auto pbuf = ptr;
  auto pkt_cnt = GET_RX_STATUS_DESC_USB_AGG_PKTNUM_8812(pbuf.data());
  //_logger->info("pkt_cnt == {}", pkt_cnt);

  auto ret = std::vector<Packet>{};

  do {
    /* Never parse a descriptor out of a tail fragment shorter than the
     * descriptor itself (kernel rejects transfers < RXDESC_SIZE before
     * parsing, os_dep usb_ops_linux.c). */
    if (pbuf.size() < RXDESC_SIZE) {
      break;
    }
    auto pattrib = rtl8812_query_rx_desc_status(pbuf.data());

    auto pkt_offset = RXDESC_SIZE + pattrib.drvinfo_sz + pattrib.shift_sz +
                      pattrib.pkt_len; // this is offset for next package

    /* The packet-length sanity check has to run BEFORE deciding what to do
     * about CRC/ICV errors. If pkt_len is unreadable we can't find the next
     * aggregate boundary either way, so we still have to give up. But a
     * surviving descriptor with a bad-CRC body is recoverable: we can
     * surface the (corrupted) packet to the consumer and advance to the
     * next one in the same USB aggregate. */
    if ((pattrib.pkt_len <= 0) || (pkt_offset > pbuf.size())) {
      _logger->warn(
          "RX Warning!,pkt_len <= 0 or pkt_offset > transfer_len; pkt_len: "
          "{}, pkt_offset: {}, transfer_len: {}",
          pattrib.pkt_len, pkt_offset, pbuf.size());
      break;
    }

    /* Corrupted-frame surfacing: previously this was `break`, which threw
     * away the bad frame AND every subsequent frame in the same USB
     * aggregate (typically 4-8 frames). Now we log + continue: the packet
     * still ends up in `ret` with crc_err / icv_err set on its RxAtrib so a
     * consumer can either filter (existing behaviour) or analyse the
     * corruption (corruption_analysis.py, FEC layers). The pkt_len check
     * above already guards the slice math against a corrupted descriptor. */
    if ((pattrib.crc_err) || (pattrib.icv_err)) {
      DVR_DEBUG(_logger, "RX corrupted frame surfaced: crc_err={} icv_err={} "
                     "pkt_len={}",
                     pattrib.crc_err, pattrib.icv_err, pattrib.pkt_len);
    }

    if (pattrib.mfrag) {
      // !!! We skips this packages because ohd not use fragmentation
      _logger->warn("mfrag scipping");

      // if (rtw_os_alloc_recvframe(precvframe, pbuf.Slice(pattrib.shift_sz +
      // pattrib.drvinfo_sz + RXDESC_SIZE), pskb) == false)
      //{
      //     return false;
      // }
    }

    // recvframe_put(precvframe, pattrib.pkt_len);
    /* recvframe_pull(precvframe, drvinfo_sz + RXDESC_SIZE); */

    if (pattrib.pkt_rpt_type ==
        RX_PACKET_TYPE::NORMAL_RX) /* Normal rx packet */
    {
      ret.push_back({pattrib, pbuf.subspan(pattrib.shift_sz +
                                               pattrib.drvinfo_sz + RXDESC_SIZE,
                                           pattrib.pkt_len)});

      struct _phy_status_rpt_8812 driver_data = {};
      /* Only read the PHY-status report when the descriptor says one is
       * present and it fits the remaining buffer. The kernel gates this
       * on pattrib->physt (usb_ops_linux.c:179); drvinfo_sz >= the report
       * size is the equivalent condition with the fields we carry —
       * without it, frames with drvinfo_sz==0 had payload bytes decoded
       * as RSSI/EVM/SNR, and a frame ending near the buffer tail
       * over-read the transfer buffer. */
      if (pattrib.drvinfo_sz >= sizeof(driver_data) &&
          pbuf.size() >= RXDESC_SIZE + sizeof(driver_data)) {
        memcpy(static_cast<void *>(&driver_data), pbuf.data() + RXDESC_SIZE,
               sizeof(driver_data));
      }
      ret.back().RxAtrib.rssi[0] = driver_data.gain_trsw[0];
      ret.back().RxAtrib.rssi[1] = driver_data.gain_trsw[1];
      /* 8814AU path C/D RSSI lives in gain_trsw_cd; on 8812/8811 these bytes
       * are 0. */
      ret.back().RxAtrib.rssi[2] = driver_data.gain_trsw_cd[0];
      ret.back().RxAtrib.rssi[3] = driver_data.gain_trsw_cd[1];
      ret.back().RxAtrib.snr[0] = driver_data.rxsnr[0];
      ret.back().RxAtrib.snr[1] = driver_data.rxsnr[1];
      /* 8814AU path C/D SNR is in csi_current per upstream's struct comment
       * (DWORD 5 byte 1-2); on 8812 those bytes hold stream 1/2 CSI which we
       * don't surface, so the value is meaningful only when the chip is
       * 8814AU. */
      ret.back().RxAtrib.snr[2] = static_cast<int8_t>(driver_data.csi_current[0]);
      ret.back().RxAtrib.snr[3] = static_cast<int8_t>(driver_data.csi_current[1]);
      /* Per-stream RX EVM: streams 1/2 in rxevm, 3/4 in rxevm_cd (8814 only;
       * 0 on 8812/8811). Link-quality only — see rx_pkt_attrib::evm. */
      ret.back().RxAtrib.evm[0] = driver_data.rxevm[0];
      ret.back().RxAtrib.evm[1] = driver_data.rxevm[1];
      ret.back().RxAtrib.evm[2] = driver_data.rxevm_cd[0];
      ret.back().RxAtrib.evm[3] = driver_data.rxevm_cd[1];
      /* Path-A CFO tail — the closed-loop CFO tracker input (#217). 8812
       * 11AC phy-status carries it as a named field (DW2 byte1). */
      ret.back().RxAtrib.cfo_tail = driver_data.cfotail[0];
    } else {
      /* pkt_rpt_type == TX_REPORT1-CCX, TX_REPORT2-TX RTP,HIS_REPORT-USB HISR
       * RTP, C2H_PACKET */
      if (pattrib.pkt_rpt_type == RX_PACKET_TYPE::C2H_PACKET) {
        /* Surface C2H payload bytes as a Packet so the application
         * callback can decode them. The C2H frame body lives at the same
         * offset as a normal-RX 802.11 frame (shift + drvinfo + RXDESC).
         * Layout per upstream Realtek convention: byte 0 = C2H cmd_id
         * (sub-type), byte 1 = seq, bytes 2..N = payload. Sub-type IDs
         * vary per chip and aren't enumerated in the vendored headers —
         * examples/rx/main.cpp does a best-effort decode of the 8814A TX_RPT
         * payload via the GET_8814A_C2H_TX_RPT_* macros gated by
         * DEVOURER_TX_STATUS=1, plus a raw hex dump so observers can
         * validate the sub-type ID against on-air capture. */
        ret.push_back({pattrib, pbuf.subspan(pattrib.shift_sz +
                                                 pattrib.drvinfo_sz +
                                                 RXDESC_SIZE,
                                             pattrib.pkt_len)});
      } else if (pattrib.pkt_rpt_type == RX_PACKET_TYPE::HIS_REPORT) {
        _logger->info("RX USB HIS_REPORT");
      }
    }

    /* jaguar 8-byte alignment */
    pkt_offset = (uint16_t)_RND8(pkt_offset);
    // pkt_cnt--;

    if (pkt_offset >= pbuf.size()) {
      break;
    }
    pbuf = pbuf.subspan(pkt_offset, pbuf.size() - pkt_offset);
  } while (pbuf.size() > 0);

  /* pkt_cnt (DMA_AGG_NUM from the first descriptor) is informational only:
   * neither the kernel nor devourer uses it for loop control, and devourer
   * never decremented it — so a non-zero value here is the norm for every
   * aggregated transfer, not an error. */
  if (pkt_cnt != 0) {
    DVR_DEBUG(_logger, "RX aggregate carried {} packets (DMA_AGG_NUM)", pkt_cnt);
  }
  //_logger->info("{} received in frame", ret.size());

  return ret;
}

void rtl8812a_cal_txdesc_chksum(uint8_t *ptxdesc) {
  u16 *usPtr;
  u32 count;
  u32 index;
  u16 checksum = 0;

  usPtr = (u16 *)ptxdesc;

  /* checksum is always calculated by first 32 bytes, */
  /* and it doesn't depend on TX DESC length. */
  /* Thomas,Lucas@SD4,20130515 */
  count = 16;

  /* Clear first */
  SET_TX_DESC_TX_DESC_CHECKSUM_8812(ptxdesc, 0);

  for (index = 0; index < count; index++)
    checksum = checksum ^ le16_to_cpu(*(usPtr + index));

  SET_TX_DESC_TX_DESC_CHECKSUM_8812(ptxdesc, checksum);
}

int rtw_action_frame_parse(const u8 *frame, u32 frame_len, u8 *category,
                           u8 *action) {
  /*const u8 *frame_body = frame + sizeof(struct rtw_ieee80211_hdr_3addr);
  u16 fc;
  u8 c;
  u8 a = ACT_PUBLIC_MAX;

  fc = le16_to_cpu(((struct rtw_ieee80211_hdr_3addr *)frame)->frame_ctl);

  if ((fc & (RTW_IEEE80211_FCTL_FTYPE | RTW_IEEE80211_FCTL_STYPE))
      != (RTW_IEEE80211_FTYPE_MGMT | RTW_IEEE80211_STYPE_ACTION)
     )
          return _FALSE;

  c = frame_body[0];

  switch (c) {
  case RTW_WLAN_CATEGORY_P2P: // vendor-specific
          break;
  default:
          a = frame_body[1];
  }

  if (category)
          *category = c;
  if (action)
          *action = a;
*/
  return _TRUE;
}
