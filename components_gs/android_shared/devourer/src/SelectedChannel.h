#ifndef SELECTEDCHANNEL_H
#define SELECTEDCHANNEL_H

#include <cstdint>

enum ChannelWidth_t {
  CHANNEL_WIDTH_20 = 0,
  CHANNEL_WIDTH_40 = 1,
  CHANNEL_WIDTH_80 = 2,
  CHANNEL_WIDTH_160 = 3,
  CHANNEL_WIDTH_80_80 = 4,
  CHANNEL_WIDTH_5 = 5,
  CHANNEL_WIDTH_10 = 6,
  CHANNEL_WIDTH_MAX = 7,
};

struct SelectedChannel {
  uint8_t Channel;
  uint8_t ChannelOffset;
  ChannelWidth_t ChannelWidth;
};

#endif /* SELECTEDCHANNEL_H */
