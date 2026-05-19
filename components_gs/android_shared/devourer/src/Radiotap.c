/*
 * Radiotap parser
 *
 * Copyright 2007        Andy Green <andy@warmcat.com>
 * Copyright 2009        Johannes Berg <johannes@sipsolutions.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Alternatively, this software may be distributed under the terms of BSD
 * license.
 *
 * See COPYING for more details.
 */

#include "ieee80211_radiotap.h"
#include <stdio.h>
#include <stddef.h>  /* for size_t */
#include <stdint.h>  /* for uint8_t, uint16_t, etc. */

/* if not defined already */
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* Make sure your type definitions for u8, u16 etc are available.
   For example:
   typedef uint8_t u8;
   typedef uint16_t u16;
*/

enum MGN_RATE {
  MGN_1M = 0x02,
  MGN_2M = 0x04,
  MGN_5_5M = 0x0B,
  MGN_6M = 0x0C,
  MGN_9M = 0x12,
  MGN_11M = 0x16,
  MGN_12M = 0x18,
  MGN_18M = 0x24,
  MGN_24M = 0x30,
  MGN_36M = 0x48,
  MGN_48M = 0x60,
  MGN_54M = 0x6C,
  MGN_MCS32 = 0x7F,
  MGN_MCS0,
  MGN_MCS1,
  MGN_MCS2,
  MGN_MCS3,
  MGN_MCS4,
  MGN_MCS5,
  MGN_MCS6,
  MGN_MCS7,
  MGN_MCS8,
  MGN_MCS9,
  MGN_MCS10,
  MGN_MCS11,
  MGN_MCS12,
  MGN_MCS13,
  MGN_MCS14,
  MGN_MCS15,
  MGN_MCS16,
  MGN_MCS17,
  MGN_MCS18,
  MGN_MCS19,
  MGN_MCS20,
  MGN_MCS21,
  MGN_MCS22,
  MGN_MCS23,
  MGN_MCS24,
  MGN_MCS25,
  MGN_MCS26,
  MGN_MCS27,
  MGN_MCS28,
  MGN_MCS29,
  MGN_MCS30,
  MGN_MCS31,
  MGN_VHT1SS_MCS0,
  MGN_VHT1SS_MCS1,
  MGN_VHT1SS_MCS2,
  MGN_VHT1SS_MCS3,
  MGN_VHT1SS_MCS4,
  MGN_VHT1SS_MCS5,
  MGN_VHT1SS_MCS6,
  MGN_VHT1SS_MCS7,
  MGN_VHT1SS_MCS8,
  MGN_VHT1SS_MCS9,
  MGN_VHT2SS_MCS0,
  MGN_VHT2SS_MCS1,
  MGN_VHT2SS_MCS2,
  MGN_VHT2SS_MCS3,
  MGN_VHT2SS_MCS4,
  MGN_VHT2SS_MCS5,
  MGN_VHT2SS_MCS6,
  MGN_VHT2SS_MCS7,
  MGN_VHT2SS_MCS8,
  MGN_VHT2SS_MCS9,
  MGN_VHT3SS_MCS0,
  MGN_VHT3SS_MCS1,
  MGN_VHT3SS_MCS2,
  MGN_VHT3SS_MCS3,
  MGN_VHT3SS_MCS4,
  MGN_VHT3SS_MCS5,
  MGN_VHT3SS_MCS6,
  MGN_VHT3SS_MCS7,
  MGN_VHT3SS_MCS8,
  MGN_VHT3SS_MCS9,
  MGN_VHT4SS_MCS0,
  MGN_VHT4SS_MCS1,
  MGN_VHT4SS_MCS2,
  MGN_VHT4SS_MCS3,
  MGN_VHT4SS_MCS4,
  MGN_VHT4SS_MCS5,
  MGN_VHT4SS_MCS6,
  MGN_VHT4SS_MCS7,
  MGN_VHT4SS_MCS8,
  MGN_VHT4SS_MCS9,
  MGN_UNKNOWN
};

/*
 * In order to support MSVC (which doesn’t support C99 designated
 * initializers), we replace the “[index] = { .align = …, .size = … }”
 * syntax with positional initializers. Note that the array is defined
 * to have 23 entries (0..22) corresponding to indices used in the enum.
 * Entries for undefined indices (e.g. 18) are set to {0,0}.
 */

static const struct radiotap_align_size rtap_namespace_sizes[23] = {
    /*  0: IEEE80211_RADIOTAP_TSFT */       { 8, 8 },
    /*  1: IEEE80211_RADIOTAP_FLAGS */      { 1, 1 },
    /*  2: IEEE80211_RADIOTAP_RATE */       { 1, 1 },
    /*  3: IEEE80211_RADIOTAP_CHANNEL */    { 2, 4 },
    /*  4: IEEE80211_RADIOTAP_FHSS */       { 2, 2 },
    /*  5: IEEE80211_RADIOTAP_DBM_ANTSIGNAL */ { 1, 1 },
    /*  6: IEEE80211_RADIOTAP_DBM_ANTNOISE */ { 1, 1 },
    /*  7: IEEE80211_RADIOTAP_LOCK_QUALITY */ { 2, 2 },
    /*  8: IEEE80211_RADIOTAP_TX_ATTENUATION */ { 2, 2 },
    /*  9: IEEE80211_RADIOTAP_DB_TX_ATTENUATION */ { 2, 2 },
    /* 10: IEEE80211_RADIOTAP_DBM_TX_POWER */ { 1, 1 },
    /* 11: IEEE80211_RADIOTAP_ANTENNA */      { 1, 1 },
    /* 12: IEEE80211_RADIOTAP_DB_ANTSIGNAL */ { 1, 1 },
    /* 13: IEEE80211_RADIOTAP_DB_ANTNOISE */  { 1, 1 },
    /* 14: IEEE80211_RADIOTAP_RX_FLAGS */     { 2, 2 },
    /* 15: IEEE80211_RADIOTAP_TX_FLAGS */     { 2, 2 },
    /* 16: IEEE80211_RADIOTAP_RTS_RETRIES */  { 1, 1 },
    /* 17: IEEE80211_RADIOTAP_DATA_RETRIES */ { 1, 1 },
    /* 18: (unused) */                        { 0, 0 },
    /* 19: IEEE80211_RADIOTAP_MCS */          { 1, 3 },
    /* 20: IEEE80211_RADIOTAP_AMPDU_STATUS */ { 4, 8 },
    /* 21: IEEE80211_RADIOTAP_VHT */          { 2, 12 },
    /* 22: IEEE80211_RADIOTAP_TIMESTAMP */    { 8, 12 },
};

static const struct ieee80211_radiotap_namespace radiotap_ns = {
    rtap_namespace_sizes,
    (int)sizeof(rtap_namespace_sizes) / sizeof(rtap_namespace_sizes[0]),
    0,   /* oui */
    0    /* subns */
};

/**
 * ieee80211_radiotap_iterator_init - radiotap parser iterator initialization
 * @iterator: radiotap_iterator to initialize
 * @radiotap_header: radiotap header to parse
 * @max_length: total length we can parse into (eg, whole packet length)
 *
 * Returns: 0 or a negative error code if there is a problem.
 *
 * This function initializes an opaque iterator struct which can then
 * be passed to ieee80211_radiotap_iterator_next() to visit every radiotap
 * argument which is present in the header.  It knows about extended
 * present headers and handles them.
 *
 * How to use:
 * call __ieee80211_radiotap_iterator_init() to init a semi-opaque iterator
 * struct ieee80211_radiotap_iterator (no need to init the struct beforehand)
 * checking for a good 0 return code.  Then loop calling
 * __ieee80211_radiotap_iterator_next()... it returns either 0,
 * -ENOENT if there are no more args to parse, or -EINVAL if there is a problem.
 * The iterator's @this_arg member points to the start of the argument
 * associated with the current argument index that is present, which can be
 * found in the iterator's @this_arg_index member.  This arg index corresponds
 * to the IEEE80211_RADIOTAP_... defines.
 *
 * Radiotap header length:
 * You can find the CPU-endian total radiotap header length in
 * iterator->max_length after executing ieee80211_radiotap_iterator_init()
 * successfully.
 *
 * Alignment Gotcha:
 * You must take care when dereferencing iterator.this_arg
 * for multibyte types... the pointer is not aligned.  Use
 * get_unaligned((type *)iterator.this_arg) to dereference
 * iterator.this_arg for type "type" safely on all arches.
 *
 * Example code:
 * See Documentation/networking/radiotap-headers.txt
 */

int ieee80211_radiotap_iterator_init(
    struct ieee80211_radiotap_iterator *iterator,
    struct ieee80211_radiotap_header *radiotap_header, int max_length,
    const struct ieee80211_radiotap_vendor_namespaces *vns) {
  /* check the radiotap header can actually be present */
  if (max_length < (int)sizeof(struct ieee80211_radiotap_header))
    return -EINVAL;

  /* Linux only supports version 0 radiotap format */
  if (radiotap_header->it_version)
    return -EINVAL;

  /* sanity check for allowed length and radiotap length field */
  if (max_length < get_unaligned_le16(&radiotap_header->it_len))
    return -EINVAL;

  iterator->_rtheader = radiotap_header;
  iterator->_max_length = get_unaligned_le16(&radiotap_header->it_len);
  iterator->_arg_index = 0;
  iterator->_bitmap_shifter = get_unaligned_le32(&radiotap_header->it_present);
  iterator->_arg = (uint8_t *)radiotap_header + sizeof(*radiotap_header);
  iterator->_reset_on_ext = 0;
  iterator->_next_bitmap = &radiotap_header->it_present; // NOLINT(clang-diagnostic-address-of-packed-member)
  iterator->_next_bitmap++;
  iterator->_vns = vns;
  iterator->current_namespace = &radiotap_ns;
  iterator->is_radiotap_ns = 1;

  /* find payload start allowing for extended bitmap(s) */

  if (iterator->_bitmap_shifter & (1u << IEEE80211_RADIOTAP_EXT)) {
    if (((size_t)iterator->_arg - (size_t)iterator->_rtheader) + sizeof(uint32_t) >
        (size_t)iterator->_max_length)
      return -EINVAL;
    while (get_unaligned_le32(iterator->_arg) &
           (1u << IEEE80211_RADIOTAP_EXT)) {
      iterator->_arg += sizeof(uint32_t);
      if (((size_t)iterator->_arg - (size_t)iterator->_rtheader) + sizeof(uint32_t) >
          (size_t)iterator->_max_length)
        return -EINVAL;
    }

    iterator->_arg += sizeof(uint32_t);

    /*
     * no need to check again for blowing past stated radiotap
     * header length, because ieee80211_radiotap_iterator_next
     * checks it before it is dereferenced
     */
  }

  iterator->this_arg = iterator->_arg;

  /* we are all initialized happily */

  return 0;
}

static void find_ns(struct ieee80211_radiotap_iterator *iterator, uint32_t oui,
                    uint8_t subns) {
  int i;

  iterator->current_namespace = NULL;

  if (!iterator->_vns)
    return;

  for (i = 0; i < iterator->_vns->n_ns; i++) {
    if (iterator->_vns->ns[i].oui != oui)
      continue;
    if (iterator->_vns->ns[i].subns != subns)
      continue;

    iterator->current_namespace = &iterator->_vns->ns[i];
    break;
  }
}

/**
 * ieee80211_radiotap_iterator_next - return next radiotap parser iterator arg
 * @iterator: radiotap_iterator to move to next arg (if any)
 *
 * Returns: 0 if there is an argument to handle,
 * -ENOENT if there are no more args or -EINVAL
 * if there is something else wrong.
 *
 * This function provides the next radiotap arg index (IEEE80211_RADIOTAP_*)
 * in @this_arg_index and sets @this_arg to point to the
 * payload for the field.  It takes care of alignment handling and extended
 * present fields.  @this_arg can be changed by the caller (eg,
 * incremented to move inside a compound argument like
 * IEEE80211_RADIOTAP_CHANNEL).  The args pointed to are in
 * little-endian format whatever the endianess of your CPU.
 *
 * Alignment Gotcha:
 * You must take care when dereferencing iterator.this_arg
 * for multibyte types... the pointer is not aligned.  Use
 * get_unaligned((type *)iterator.this_arg) to dereference
 * iterator.this_arg for type "type" safely on all arches.
 */

int ieee80211_radiotap_iterator_next(
    struct ieee80211_radiotap_iterator *iterator) {
  while (1) {
    int hit = 0;
    int pad, align = 0, size = 0, subns;
    uint32_t oui;

    if ((iterator->_arg_index % 32) == IEEE80211_RADIOTAP_EXT &&
        !(iterator->_bitmap_shifter & 1))
      return -ENOENT;

    if (!(iterator->_bitmap_shifter & 1))
      goto next_entry;

    switch (iterator->_arg_index % 32) {
    case IEEE80211_RADIOTAP_RADIOTAP_NAMESPACE:
    case IEEE80211_RADIOTAP_EXT:
      align = 1;
      size = 0;
      break;
    case IEEE80211_RADIOTAP_VENDOR_NAMESPACE:
      align = 2;
      size = 6;
      break;
    default:
      if (!iterator->current_namespace ||
          iterator->_arg_index >= iterator->current_namespace->n_bits) {
        if (iterator->current_namespace == &radiotap_ns)
          return -ENOENT;
        align = 0;
      } else {
        align =
            iterator->current_namespace->align_size[iterator->_arg_index].align;
        size =
            iterator->current_namespace->align_size[iterator->_arg_index].size;
      }
      if (!align) {
        iterator->_arg = iterator->_next_ns_data;
        iterator->current_namespace = NULL;
        goto next_entry;
      }
      break;
    }

    pad = (((size_t)iterator->_arg - (size_t)iterator->_rtheader) & (align - 1));
    if (pad)
      iterator->_arg += align - pad;

    if (iterator->_arg_index % 32 == IEEE80211_RADIOTAP_VENDOR_NAMESPACE) {
      int vnslen;
      if (((size_t)iterator->_arg + size - (size_t)iterator->_rtheader) >
          (size_t)iterator->_max_length)
        return -EINVAL;

      oui = (iterator->_arg[0] << 16) | (iterator->_arg[1] << 8) |
            iterator->_arg[2];
      subns = iterator->_arg[3];

      find_ns(iterator, oui, subns);
      vnslen = get_unaligned_le16(iterator->_arg + 4);
      iterator->_next_ns_data = iterator->_arg + size + vnslen;
      if (!iterator->current_namespace)
        size += vnslen;
    }

    /*
     * this is what we will return to user, but we need to
     * move on first so next call has something fresh to test
     */
    iterator->this_arg_index = iterator->_arg_index;
    iterator->this_arg = iterator->_arg;
    iterator->this_arg_size = size;

    /* internally move on the size of this arg */
    iterator->_arg += size;

    /*
     * check for insanity where we are given a bitmap that
     * claims to have more arg content than the length of the
     * radiotap section.  We will normally end up equalling this
     * max_length on the last arg, never exceeding it.
     */

    if (((size_t)iterator->_arg - (size_t)iterator->_rtheader) >
        (size_t)iterator->_max_length)
      return -EINVAL;

    switch (iterator->_arg_index % 32) {
    case IEEE80211_RADIOTAP_VENDOR_NAMESPACE:
      iterator->_reset_on_ext = 1;

      iterator->is_radiotap_ns = 0;
      /*
       * If parser didn't register this vendor
       * namespace with us, allow it to show it
       * as 'raw. Do do that, set argument index
       * to vendor namespace.
       */
      iterator->this_arg_index = IEEE80211_RADIOTAP_VENDOR_NAMESPACE;
      if (!iterator->current_namespace)
        hit = 1;
      goto next_entry;
    case IEEE80211_RADIOTAP_RADIOTAP_NAMESPACE:
      iterator->_reset_on_ext = 1;
      iterator->current_namespace = &radiotap_ns;
      iterator->is_radiotap_ns = 1;
      goto next_entry;
    case IEEE80211_RADIOTAP_EXT:
      /*
       * bit 31 was set, there is more
       * -- move to next u32 bitmap
       */
      iterator->_bitmap_shifter = get_unaligned_le32(iterator->_next_bitmap);
      iterator->_next_bitmap++;
      if (iterator->_reset_on_ext)
        iterator->_arg_index = 0;
      else
        iterator->_arg_index++;
      iterator->_reset_on_ext = 0;
      break;
    default:
      /* we've got a hit! */
      hit = 1;
    next_entry:
      iterator->_bitmap_shifter >>= 1;
      iterator->_arg_index++;
    }

    /* if we found a valid arg earlier, return it now */
    if (hit)
      return 0;
  }
}

u16 GetSequence(const u8 *packet) {
  u16 seqCtrl = (u16)(packet[22]) | (u16)(packet[23] << 8);
  u16 seqNum = (seqCtrl >> 4) & 0x0FFF;
  return seqNum;
}

u8 MRateToHwRate(u8 rate) {
  u8 ret = DESC_RATE1M;
  switch (rate) {
  case MGN_1M: ret = DESC_RATE1M; break;
  case MGN_2M: ret = DESC_RATE2M; break;
  case MGN_5_5M: ret = DESC_RATE5_5M; break;
  case MGN_11M: ret = DESC_RATE11M; break;
  case MGN_6M: ret = DESC_RATE6M; break;
  case MGN_9M: ret = DESC_RATE9M; break;
  case MGN_12M: ret = DESC_RATE12M; break;
  case MGN_18M: ret = DESC_RATE18M; break;
  case MGN_24M: ret = DESC_RATE24M; break;
  case MGN_36M: ret = DESC_RATE36M; break;
  case MGN_48M: ret = DESC_RATE48M; break;
  case MGN_54M: ret = DESC_RATE54M; break;
  case MGN_MCS0: ret = DESC_RATEMCS0; break;
  case MGN_MCS1: ret = DESC_RATEMCS1; break;
  case MGN_MCS2: ret = DESC_RATEMCS2; break;
  case MGN_MCS3: ret = DESC_RATEMCS3; break;
  case MGN_MCS4: ret = DESC_RATEMCS4; break;
  case MGN_MCS5: ret = DESC_RATEMCS5; break;
  case MGN_MCS6: ret = DESC_RATEMCS6; break;
  case MGN_MCS7: ret = DESC_RATEMCS7; break;
  case MGN_MCS8: ret = DESC_RATEMCS8; break;
  case MGN_MCS9: ret = DESC_RATEMCS9; break;
  case MGN_MCS10: ret = DESC_RATEMCS10; break;
  case MGN_MCS11: ret = DESC_RATEMCS11; break;
  case MGN_MCS12: ret = DESC_RATEMCS12; break;
  case MGN_MCS13: ret = DESC_RATEMCS13; break;
  case MGN_MCS14: ret = DESC_RATEMCS14; break;
  case MGN_MCS15: ret = DESC_RATEMCS15; break;
  case MGN_MCS16: ret = DESC_RATEMCS16; break;
  case MGN_MCS17: ret = DESC_RATEMCS17; break;
  case MGN_MCS18: ret = DESC_RATEMCS18; break;
  case MGN_MCS19: ret = DESC_RATEMCS19; break;
  case MGN_MCS20: ret = DESC_RATEMCS20; break;
  case MGN_MCS21: ret = DESC_RATEMCS21; break;
  case MGN_MCS22: ret = DESC_RATEMCS22; break;
  case MGN_MCS23: ret = DESC_RATEMCS23; break;
  case MGN_MCS24: ret = DESC_RATEMCS24; break;
  case MGN_MCS25: ret = DESC_RATEMCS25; break;
  case MGN_MCS26: ret = DESC_RATEMCS26; break;
  case MGN_MCS27: ret = DESC_RATEMCS27; break;
  case MGN_MCS28: ret = DESC_RATEMCS28; break;
  case MGN_MCS29: ret = DESC_RATEMCS29; break;
  case MGN_MCS30: ret = DESC_RATEMCS30; break;
  case MGN_MCS31: ret = DESC_RATEMCS31; break;
  case MGN_VHT1SS_MCS0: ret = DESC_RATEVHTSS1MCS0; break;
  case MGN_VHT1SS_MCS1: ret = DESC_RATEVHTSS1MCS1; break;
  case MGN_VHT1SS_MCS2: ret = DESC_RATEVHTSS1MCS2; break;
  case MGN_VHT1SS_MCS3: ret = DESC_RATEVHTSS1MCS3; break;
  case MGN_VHT1SS_MCS4: ret = DESC_RATEVHTSS1MCS4; break;
  case MGN_VHT1SS_MCS5: ret = DESC_RATEVHTSS1MCS5; break;
  case MGN_VHT1SS_MCS6: ret = DESC_RATEVHTSS1MCS6; break;
  case MGN_VHT1SS_MCS7: ret = DESC_RATEVHTSS1MCS7; break;
  case MGN_VHT1SS_MCS8: ret = DESC_RATEVHTSS1MCS8; break;
  case MGN_VHT1SS_MCS9: ret = DESC_RATEVHTSS1MCS9; break;
  case MGN_VHT2SS_MCS0: ret = DESC_RATEVHTSS2MCS0; break;
  case MGN_VHT2SS_MCS1: ret = DESC_RATEVHTSS2MCS1; break;
  case MGN_VHT2SS_MCS2: ret = DESC_RATEVHTSS2MCS2; break;
  case MGN_VHT2SS_MCS3: ret = DESC_RATEVHTSS2MCS3; break;
  case MGN_VHT2SS_MCS4: ret = DESC_RATEVHTSS2MCS4; break;
  case MGN_VHT2SS_MCS5: ret = DESC_RATEVHTSS2MCS5; break;
  case MGN_VHT2SS_MCS6: ret = DESC_RATEVHTSS2MCS6; break;
  case MGN_VHT2SS_MCS7: ret = DESC_RATEVHTSS2MCS7; break;
  case MGN_VHT2SS_MCS8: ret = DESC_RATEVHTSS2MCS8; break;
  case MGN_VHT2SS_MCS9: ret = DESC_RATEVHTSS2MCS9; break;
  case MGN_VHT3SS_MCS0: ret = DESC_RATEVHTSS3MCS0; break;
  case MGN_VHT3SS_MCS1: ret = DESC_RATEVHTSS3MCS1; break;
  case MGN_VHT3SS_MCS2: ret = DESC_RATEVHTSS3MCS2; break;
  case MGN_VHT3SS_MCS3: ret = DESC_RATEVHTSS3MCS3; break;
  case MGN_VHT3SS_MCS4: ret = DESC_RATEVHTSS3MCS4; break;
  case MGN_VHT3SS_MCS5: ret = DESC_RATEVHTSS3MCS5; break;
  case MGN_VHT3SS_MCS6: ret = DESC_RATEVHTSS3MCS6; break;
  case MGN_VHT3SS_MCS7: ret = DESC_RATEVHTSS3MCS7; break;
  case MGN_VHT3SS_MCS8: ret = DESC_RATEVHTSS3MCS8; break;
  case MGN_VHT3SS_MCS9: ret = DESC_RATEVHTSS3MCS9; break;
  case MGN_VHT4SS_MCS0: ret = DESC_RATEVHTSS4MCS0; break;
  case MGN_VHT4SS_MCS1: ret = DESC_RATEVHTSS4MCS1; break;
  case MGN_VHT4SS_MCS2: ret = DESC_RATEVHTSS4MCS2; break;
  case MGN_VHT4SS_MCS3: ret = DESC_RATEVHTSS4MCS3; break;
  case MGN_VHT4SS_MCS4: ret = DESC_RATEVHTSS4MCS4; break;
  case MGN_VHT4SS_MCS5: ret = DESC_RATEVHTSS4MCS5; break;
  case MGN_VHT4SS_MCS6: ret = DESC_RATEVHTSS4MCS6; break;
  case MGN_VHT4SS_MCS7: ret = DESC_RATEVHTSS4MCS7; break;
  case MGN_VHT4SS_MCS8: ret = DESC_RATEVHTSS4MCS8; break;
  case MGN_VHT4SS_MCS9: ret = DESC_RATEVHTSS4MCS9; break;
  default: break;
  }
  return ret;
}

int parse_radiotap() {
  int ret = 0;
  int rtap_len;
  int qos_len = 0;
  int dot11_hdr_len = 24;
  int snap_len = 6;
  unsigned char *pdata;
  u16 frame_ctl;
  unsigned char src_mac_addr[6];
  unsigned char dst_mac_addr[6];
  u8 fixed_rate = MGN_1M, sgi = 0, bwidth = 0, ldpc = 0, stbc = 0;
  u16 txflags = 0;

  /* This packet is only 13 bytes long.
     We add an explicit cast to silence the warning. */
  uint8_t pkt[] = {
      0x00, 0x00, 0x0d, 0x00, 0x00, 0x80, 0x08,
      0x00, 0x08, 0x00, 0x37, 0x00, 0x01,
  };
  struct ieee80211_radiotap_header *rtap_hdr;
  rtap_hdr = (struct ieee80211_radiotap_header *)(void *)pkt;
  struct ieee80211_radiotap_iterator iterator;
  ret = ieee80211_radiotap_iterator_init(&iterator, rtap_hdr, 0x0d, NULL);
  while (!ret) {
    ret = ieee80211_radiotap_iterator_next(&iterator);

    if (ret)
      continue;

    /* see if this argument is something we can use */
    switch (iterator.this_arg_index) {

    case IEEE80211_RADIOTAP_RATE: /* u8 */
      fixed_rate = *iterator.this_arg;
      break;

    case IEEE80211_RADIOTAP_TX_FLAGS:
      txflags = get_unaligned_le16(iterator.this_arg);
      break;

    case IEEE80211_RADIOTAP_MCS: { /* u8,u8,u8 */
      u8 mcs_have = iterator.this_arg[0];
//      printf("MCS value:%d %d %d\n", iterator.this_arg[0], iterator.this_arg[1],
//             iterator.this_arg[2]);
//      printf("mcs parse:%d\n", mcs_have);
      if (mcs_have & IEEE80211_RADIOTAP_MCS_HAVE_MCS) {
        fixed_rate = iterator.this_arg[2] & 0x7f;
        if (fixed_rate > 31)
          fixed_rate = 0;
        fixed_rate += MGN_MCS0;
      }
//      printf("mcs_have & 4 = %d,%d \n", (mcs_have & 4),
//             (iterator.this_arg[1] & 1));
      if ((mcs_have & 4) && (iterator.this_arg[1] & 4))
        sgi = 1;
      if ((mcs_have & 1) && (iterator.this_arg[1] & 1))
        bwidth = 1;
      if ((mcs_have & 0x10) && (iterator.this_arg[1] & 0x10))
        ldpc = 1;
      if ((mcs_have & 0x20))
        stbc = (iterator.this_arg[1] >> 5) & 3;
    } break;

    case IEEE80211_RADIOTAP_VHT: {
      /* u16 known, u8 flags, u8 bandwidth, u8 mcs_nss[4], u8 coding, u8
       * group_id, u16 partial_aid */
      u8 known = iterator.this_arg[0];
      u8 flags = iterator.this_arg[2];
      unsigned int mcs, nss;
      if ((known & 4) && (flags & 4))
        sgi = 1;
      if ((known & 1) && (flags & 1))
        stbc = 1;
      if (known & 0x40) {
        bwidth = iterator.this_arg[3] & 0x1f;
        if (bwidth >= 1 && bwidth <= 3)
          bwidth = 1; // 40 MHz
        else if (bwidth >= 4 && bwidth <= 10)
          bwidth = 2; // 80 MHz
        else
          bwidth = 0; // 20 MHz
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

  printf("fixed rate:%d\n", fixed_rate);
  printf("sgi =%d, bandwdith=%d, ldpc=%d, stbc=%d\n", sgi, bwidth, ldpc, stbc);
  return 1;
}
