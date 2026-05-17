#include "Rtl8812aDevice.h"
#include "EepromManager.h"
#include "RadioManagementModule.h"

Rtl8812aDevice::Rtl8812aDevice(RtlUsbAdapter device, Logger_t logger)
    : _device{device},
      _eepromManager{std::make_shared<EepromManager>(device, logger)},
      _radioManagement{std::make_shared<RadioManagementModule>(
          device, _eepromManager, logger)},
      _halModule{device, _eepromManager, _radioManagement, logger},
      _logger{logger} {}

void Rtl8812aDevice::InitWrite(SelectedChannel channel) {
  StartWithMonitorMode(channel);
  SetMonitorChannel(channel);
  _logger->info("In Monitor Mode");
}

bool Rtl8812aDevice::send_packet(const uint8_t *packet, size_t length) {
  struct tx_desc *ptxdesc;
  bool resp;
  uint8_t *usb_frame;
  int real_packet_length, usb_frame_length, radiotap_length;

  bool vht = false;
  int ret = 0;
  int qos_len = 0;
  u8 fixed_rate = MGN_1M, sgi = 0, bwidth = 0, ldpc = 0, stbc = 0;
  u16 txflags = 0;
  int rate_id = 0;
  radiotap_length = int(packet[2]);
  real_packet_length = length - radiotap_length;

  if (radiotap_length != 0x0d)
    vht = true;

  usb_frame_length = real_packet_length + TXDESC_SIZE;

  _logger->debug("radiotap length is {}, 80211 length is {}, usb_frame length "
                "should be {}",
                radiotap_length, real_packet_length, usb_frame_length);

  struct ieee80211_radiotap_header *rtap_hdr;
  rtap_hdr = (struct ieee80211_radiotap_header *)packet;
  struct ieee80211_radiotap_iterator iterator;
  ret = ieee80211_radiotap_iterator_init(&iterator, rtap_hdr, radiotap_length,
                                         NULL);
  while (!ret) {
    ret = ieee80211_radiotap_iterator_next(&iterator);

    if (ret)
      continue;

    /* see if this argument is something we can use */
    switch (iterator.this_arg_index) {

    case IEEE80211_RADIOTAP_RATE:
      fixed_rate = *iterator.this_arg;
      break;

    case IEEE80211_RADIOTAP_TX_FLAGS:
      txflags = get_unaligned_le16(iterator.this_arg);
      break;

    case IEEE80211_RADIOTAP_MCS: {
      u8 mcs_flags = iterator.this_arg[1];

      uint8_t mcs_bw_field = mcs_flags & IEEE80211_RADIOTAP_MCS_BW_MASK;
      if (mcs_bw_field == IEEE80211_RADIOTAP_MCS_BW_40) {
        bwidth = CHANNEL_WIDTH_40;
      } else if (mcs_bw_field == IEEE80211_RADIOTAP_MCS_BW_20 ||
                 mcs_bw_field == IEEE80211_RADIOTAP_MCS_BW_20L ||
                 mcs_bw_field == IEEE80211_RADIOTAP_MCS_BW_20U) {
        bwidth = CHANNEL_WIDTH_20;
      }

      if (mcs_flags & 0x04) {
        sgi = 1;
      } else {
        sgi = 0;
      }
    } break;

    case IEEE80211_RADIOTAP_VHT: {
      u8 known = iterator.this_arg[0];
      u8 flags = iterator.this_arg[2];
      unsigned int mcs, nss;
      if ((known & 4) && (flags & 4))
        sgi = 1;
      if ((known & 1) && (flags & 1))
        stbc = 1;
      if (known & 0x40) {
        auto bw = iterator.this_arg[3] & 0x1f;
        if (bw >= 1 && bw <= 3)
          bwidth = 40;
        else if (bw >= 4 && bw <= 10)
          bwidth = 80;
        else
          bwidth = 20;
      }

      if (iterator.this_arg[8] & 1)
        ldpc = 1;
      mcs = (iterator.this_arg[4] >> 4) & 0x0f;
      nss = iterator.this_arg[4] & 0x0f;
      if (nss > 0) {
        if (nss > 4)
          nss = 4;
        if (mcs > 9)
          mcs = 9;
        fixed_rate = MGN_VHT1SS_MCS0 + ((nss - 1) * 10 + mcs);
      }
    } break;

    default:
      break;
    }
  }

  usb_frame = new uint8_t[usb_frame_length]();

  ptxdesc = (struct tx_desc *)usb_frame;

  _logger->debug("fixed rate:{}, sgi:{}, radiotap_bwidth:{}, ldpc:{}, stbc:{}",
                (int)fixed_rate, (int)sgi, (int)bwidth, (int)ldpc, (int)stbc);

  uint8_t BWSettingOfDesc;
  if (bwidth == CHANNEL_WIDTH_40) {
    BWSettingOfDesc = 1;
  } else if (bwidth == CHANNEL_WIDTH_80) {
    BWSettingOfDesc = 2;
  } else {
    BWSettingOfDesc = 0;
  }
  _logger->debug("TX DESC BW decision: _channel.ChannelWidth(RX)={}, radiotap_bwidth(TX)={}, BWSettingOfDesc(TX_DESC)={}",
                (int)_channel.ChannelWidth, (int)bwidth, (int)BWSettingOfDesc);

  SET_TX_DESC_DATA_BW_8812(usb_frame, BWSettingOfDesc);

  SET_TX_DESC_FIRST_SEG_8812(usb_frame, 1);
  SET_TX_DESC_LAST_SEG_8812(usb_frame, 1);
  SET_TX_DESC_OWN_8812(usb_frame, 1);

  SET_TX_DESC_PKT_SIZE_8812(usb_frame,
                            static_cast<uint32_t>(real_packet_length));

  SET_TX_DESC_OFFSET_8812(usb_frame,
                          static_cast<uint8_t>(TXDESC_SIZE + OFFSET_SZ));

  SET_TX_DESC_MACID_8812(usb_frame, static_cast<uint8_t>(0x01));

  if (!vht) {
    rate_id = 7;
  } else {
    rate_id = 9;
  }

  SET_TX_DESC_BMC_8812(usb_frame, 1);
  SET_TX_DESC_RATE_ID_8812(
      usb_frame,
      static_cast<uint8_t>(rate_id));

  SET_TX_DESC_QUEUE_SEL_8812(usb_frame, 0x12);
  SET_TX_DESC_HWSEQ_EN_8812(
      usb_frame, static_cast<uint8_t>(0));
  SET_TX_DESC_SEQ_8812(
      usb_frame,
      GetSequence(packet +
                  radiotap_length));
  SET_TX_DESC_RETRY_LIMIT_ENABLE_8812(usb_frame, static_cast<uint8_t>(1));

  SET_TX_DESC_DATA_RETRY_LIMIT_8812(usb_frame, static_cast<uint8_t>(0));
  if (sgi) {
    _logger->info("short gi enabled,set sgi");
    SET_TX_DESC_DATA_SHORT_8812(usb_frame, 1);
  }
  SET_TX_DESC_DISABLE_FB_8812(usb_frame, 1);
  SET_TX_DESC_USE_RATE_8812(usb_frame, 1);
  SET_TX_DESC_TX_RATE_8812(usb_frame,
                           static_cast<uint8_t>(MRateToHwRate(
                               fixed_rate)));

  if (ldpc) {
    SET_TX_DESC_DATA_LDPC_8812(usb_frame, ldpc);
  }

  SET_TX_DESC_DATA_STBC_8812(usb_frame, stbc & 3);

  rtl8812a_cal_txdesc_chksum(usb_frame);
  _logger->debug("tx desc formed");
#ifdef DEBUG
  for (size_t i = 0; i < usb_frame_length; ++i) {
    std::cout << "0x" << std::hex << std::setw(2) << std::setfill('0')
              << static_cast<int>(usb_frame[i]);

    if (i < usb_frame_length - 1) {
      std::cout << ",";
    }
  }
  std::cout << std::dec << std::endl;
#endif
  uint8_t *addr = usb_frame + TXDESC_SIZE;
  memcpy(addr, packet + radiotap_length, real_packet_length);
  _logger->debug("packet formed");
#ifdef DEBUG
  for (size_t i = 0; i < usb_frame_length; ++i) {
    std::cout << "0x" << std::hex << std::setw(2) << std::setfill('0')
              << static_cast<int>(usb_frame[i]);

    if (i < usb_frame_length - 1) {
      std::cout << ",";
    }
  }
  std::cout << std::dec << std::endl;
#endif
  resp = _device.send_packet(usb_frame, usb_frame_length);
  delete[] usb_frame;

  return resp;
}

void Rtl8812aDevice::Init(Action_ParsedRadioPacket packetProcessor,
                          SelectedChannel channel) {
  _packetProcessor = packetProcessor;

  StartWithMonitorMode(channel);
  SetMonitorChannel(channel);

  _logger->info("Listening air...");
  while (!should_stop) {
    auto packets = _device.infinite_read();
    for (auto &p : packets) {
      _packetProcessor(p);
    }
  }

#if 0
  _device.UsbDevice.SetBulkDataHandler(BulkDataHandler);
  _readTask = Task.Run(() = > _device.UsbDevice.InfinityRead());
#endif
}

void Rtl8812aDevice::SetMonitorChannel(SelectedChannel channel) {
  _radioManagement->set_channel_bwmode(channel.Channel, channel.ChannelOffset,
                                       channel.ChannelWidth);
}

void Rtl8812aDevice::StartWithMonitorMode(SelectedChannel selectedChannel) {
  if (NetDevOpen(selectedChannel) == false) {
    throw std::ios_base::failure("StartWithMonitorMode failed NetDevOpen");
  }

  _radioManagement->SetMonitorMode();
}

void Rtl8812aDevice::SetTxPower(uint8_t power) {
  _radioManagement->SetTxPower(power);
}

bool Rtl8812aDevice::NetDevOpen(SelectedChannel selectedChannel) {
  auto status = _halModule.rtw_hal_init(selectedChannel);
  if (status == false) {
    return false;
  }

  return true;
}
