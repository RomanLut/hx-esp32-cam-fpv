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
  /* Offset 16 */
  pattrib.sgi = GET_RX_STATUS_DESC_SPLCP_8812(pdesc);
  pattrib.ldpc = GET_RX_STATUS_DESC_LDPC_8812(pdesc);
  pattrib.stbc = GET_RX_STATUS_DESC_STBC_8812(pdesc);
  pattrib.bw = GET_RX_STATUS_DESC_BW_8812(pdesc);

  /* Offset 20 */
  /* pattrib.tsfl=(byte)GET_RX_STATUS_DESC_TSFL_8812(pdesc); */

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
    auto pattrib = rtl8812_query_rx_desc_status(pbuf.data());

    if ((pattrib.crc_err) || (pattrib.icv_err)) {
      _logger->info("RX Warning! crc_err={} "
                    "icv_err={}, skip!",
                    pattrib.crc_err, pattrib.icv_err);
      break;
    }

    auto pkt_offset = RXDESC_SIZE + pattrib.drvinfo_sz + pattrib.shift_sz +
                      pattrib.pkt_len; // this is offset for next package

    if ((pattrib.pkt_len <= 0) || (pkt_offset > pbuf.size())) {
      _logger->warn(
          "RX Warning!,pkt_len <= 0 or pkt_offset > transfer_len; pkt_len: "
          "{}, pkt_offset: {}, transfer_len: {}",
          pattrib.pkt_len, pkt_offset, pbuf.size());
      break;
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

      struct _phy_status_rpt_8812 driver_data;
      memcpy(static_cast<void*>(&driver_data), pbuf.data() + RXDESC_SIZE, sizeof(driver_data));
      ret.back().RxAtrib.rssi[0] = driver_data.gain_trsw[0];
      ret.back().RxAtrib.rssi[1] = driver_data.gain_trsw[1];
      ret.back().RxAtrib.snr[0] = driver_data.rxsnr[0];
      ret.back().RxAtrib.snr[1] = driver_data.rxsnr[1];
    } else {
      /* pkt_rpt_type == TX_REPORT1-CCX, TX_REPORT2-TX RTP,HIS_REPORT-USB HISR
       * RTP */
      if (pattrib.pkt_rpt_type == RX_PACKET_TYPE::C2H_PACKET) {
        _logger->info("RX USB C2H_PACKET");
        // rtw_hal_c2h_pkt_pre_hdl(padapter, precvframe.u.hdr.rx_data,
        // pattrib.pkt_len);
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

  if (pkt_cnt != 0) {
    _logger->info("Unprocessed packets: {}", pkt_cnt);
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
