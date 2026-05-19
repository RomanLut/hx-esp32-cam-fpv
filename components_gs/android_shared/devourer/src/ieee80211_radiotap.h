#ifndef IEEE80211_RADIOTAP_H
#define IEEE80211_RADIOTAP_H

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>


typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;
#ifdef _MSC_VER
typedef uint16_t __le16;
typedef uint32_t __le32;
#else
typedef u16 __le16;
typedef u32 __le32;
#endif

#ifndef le16_to_cpu
#define le16_to_cpu(x) (x)
#endif

#ifndef le32_to_cpu
#define le32_to_cpu(x) (x)
#endif

#define unlikely(x) (x)

/* Are two types/vars the same type (ignoring qualifiers)? */
#ifndef __same_type
#define __same_type(a, b) __builtin_types_compatible_p(typeof(a), typeof(b))
#endif

#ifndef BUILD_BUG_ON
/* Force a compilation error if condition is true */
#define BUILD_BUG_ON(condition) ((void)BUILD_BUG_ON_ZERO(condition))
/* Force a compilation error if condition is true, but also produce a
   result (of value 0 and type size_t), so the expression can be used
   e.g. in a structure initializer (or where-ever else comma expressions
   aren't permitted). */
#define BUILD_BUG_ON_ZERO(e) (sizeof(struct { int : -!!(e); }))
#define BUILD_BUG_ON_NULL(e) ((void *)sizeof(struct { int : -!!(e); }))
#endif

#define __must_be_array(a) BUILD_BUG_ON_ZERO(__same_type((a), &(a)[0]))
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]) + __must_be_array(arr))

/* Channel flags. */
#define IEEE80211_CHAN_TURBO 0x0010   /* Turbo channel */
#define IEEE80211_CHAN_CCK 0x0020     /* CCK channel */
#define IEEE80211_CHAN_OFDM 0x0040    /* OFDM channel */
#define IEEE80211_CHAN_2GHZ 0x0080    /* 2 GHz spectrum channel. */
#define IEEE80211_CHAN_5GHZ 0x0100    /* 5 GHz spectrum channel */
#define IEEE80211_CHAN_PASSIVE 0x0200 /* Only passive scan allowed */
#define IEEE80211_CHAN_DYN 0x0400     /* Dynamic CCK-OFDM channel */
#define IEEE80211_CHAN_GFSK 0x0800    /* GFSK channel (FHSS PHY) */
#define IEEE80211_CHAN_GSM 0x1000     /* GSM (900 MHz) */
#define IEEE80211_CHAN_STURBO 0x2000  /* Static Turbo */
#define IEEE80211_CHAN_HALF 0x4000    /* Half channel (10 MHz wide) */
#define IEEE80211_CHAN_QUARTER 0x8000 /* Quarter channel (5 MHz wide) */

/* For IEEE80211_RADIOTAP_FLAGS */
#define IEEE80211_RADIOTAP_F_CFP                                               \
  0x01 /* sent/received                                                        \
        * during CFP                                                           \
        */
#define IEEE80211_RADIOTAP_F_SHORTPRE                                          \
  0x02 /* sent/received                                                        \
        * with short                                                           \
        * preamble                                                             \
        */
#define IEEE80211_RADIOTAP_F_WEP                                               \
  0x04 /* sent/received                                                        \
        * with WEP encryption                                                  \
        */
#define IEEE80211_RADIOTAP_F_FRAG                                              \
  0x08                                /* sent/received                         \
                                       * with fragmentation                    \
                                       */
#define IEEE80211_RADIOTAP_F_FCS 0x10 /* frame includes FCS */
#define IEEE80211_RADIOTAP_F_DATAPAD                                           \
  0x20                                   /* frame has padding between          \
                                          * 802.11 header and payload          \
                                          * (to 32-bit boundary)               \
                                          */
#define IEEE80211_RADIOTAP_F_BADFCS 0x40 /* bad FCS */

/* For IEEE80211_RADIOTAP_RX_FLAGS */
#define IEEE80211_RADIOTAP_F_RX_BADPLCP 0x0002 /* frame has bad PLCP */

/* For IEEE80211_RADIOTAP_TX_FLAGS */
#define IEEE80211_RADIOTAP_F_TX_FAIL                                           \
  0x0001                                     /* failed due to excessive        \
                                              * retries */
#define IEEE80211_RADIOTAP_F_TX_CTS 0x0002   /* used cts 'protection' */
#define IEEE80211_RADIOTAP_F_TX_RTS 0x0004   /* used rts/cts handshake */
#define IEEE80211_RADIOTAP_F_TX_NOACK 0x0008 /* don't expect an ack */

/* For IEEE80211_RADIOTAP_MCS */
#define IEEE80211_RADIOTAP_MCS_HAVE_BW 0x01
#define IEEE80211_RADIOTAP_MCS_HAVE_MCS 0x02
#define IEEE80211_RADIOTAP_MCS_HAVE_GI 0x04
#define IEEE80211_RADIOTAP_MCS_HAVE_FMT 0x08
#define IEEE80211_RADIOTAP_MCS_HAVE_FEC 0x10
#define IEEE80211_RADIOTAP_MCS_HAVE_STBC 0x20

#define IEEE80211_RADIOTAP_MCS_BW_MASK 0x03
#define IEEE80211_RADIOTAP_MCS_BW_20 0
#define IEEE80211_RADIOTAP_MCS_BW_40 1
#define IEEE80211_RADIOTAP_MCS_BW_20L 2
#define IEEE80211_RADIOTAP_MCS_BW_20U 3
#define IEEE80211_RADIOTAP_MCS_SGI 0x04
#define IEEE80211_RADIOTAP_MCS_FMT_GF 0x08
#define IEEE80211_RADIOTAP_MCS_FEC_LDPC 0x10
#define IEEE80211_RADIOTAP_MCS_STBC_MASK 0x60
#define IEEE80211_RADIOTAP_MCS_STBC_1 1
#define IEEE80211_RADIOTAP_MCS_STBC_2 2
#define IEEE80211_RADIOTAP_MCS_STBC_3 3

#define IEEE80211_RADIOTAP_MCS_STBC_SHIFT 5

/* For IEEE80211_RADIOTAP_AMPDU_STATUS */
#define IEEE80211_RADIOTAP_AMPDU_REPORT_ZEROLEN 0x0001
#define IEEE80211_RADIOTAP_AMPDU_IS_ZEROLEN 0x0002
#define IEEE80211_RADIOTAP_AMPDU_LAST_KNOWN 0x0004
#define IEEE80211_RADIOTAP_AMPDU_IS_LAST 0x0008
#define IEEE80211_RADIOTAP_AMPDU_DELIM_CRC_ERR 0x0010
#define IEEE80211_RADIOTAP_AMPDU_DELIM_CRC_KNOWN 0x0020

/* For IEEE80211_RADIOTAP_VHT */
#define IEEE80211_RADIOTAP_VHT_KNOWN_STBC 0x0001
#define IEEE80211_RADIOTAP_VHT_KNOWN_TXOP_PS_NA 0x0002
#define IEEE80211_RADIOTAP_VHT_KNOWN_GI 0x0004
#define IEEE80211_RADIOTAP_VHT_KNOWN_SGI_NSYM_DIS 0x0008
#define IEEE80211_RADIOTAP_VHT_KNOWN_LDPC_EXTRA_OFDM_SYM 0x0010
#define IEEE80211_RADIOTAP_VHT_KNOWN_BEAMFORMED 0x0020
#define IEEE80211_RADIOTAP_VHT_KNOWN_BANDWIDTH 0x0040
#define IEEE80211_RADIOTAP_VHT_KNOWN_GROUP_ID 0x0080
#define IEEE80211_RADIOTAP_VHT_KNOWN_PARTIAL_AID 0x0100

#define IEEE80211_RADIOTAP_VHT_FLAG_STBC 0x01
#define IEEE80211_RADIOTAP_VHT_FLAG_TXOP_PS_NA 0x02
#define IEEE80211_RADIOTAP_VHT_FLAG_SGI 0x04
#define IEEE80211_RADIOTAP_VHT_FLAG_SGI_NSYM_M10_9 0x08
#define IEEE80211_RADIOTAP_VHT_FLAG_LDPC_EXTRA_OFDM_SYM 0x10
#define IEEE80211_RADIOTAP_VHT_FLAG_BEAMFORMED 0x20

#define IEEE80211_RADIOTAP_CODING_LDPC_USER0 0x01
#define IEEE80211_RADIOTAP_CODING_LDPC_USER1 0x02
#define IEEE80211_RADIOTAP_CODING_LDPC_USER2 0x04
#define IEEE80211_RADIOTAP_CODING_LDPC_USER3 0x08

/*------------------------------ Tx Desc definition Macro
 * ------------------------*/
/* #pragma mark -- Tx Desc related definition. -- */
/* ----------------------------------------------------------------------------
 * -----------------------------------------------------------
 *	Rate
 * -----------------------------------------------------------
 * CCK Rates, TxHT = 0 */
#define DESC_RATE1M 0x00
#define DESC_RATE2M 0x01
#define DESC_RATE5_5M 0x02
#define DESC_RATE11M 0x03

/* OFDM Rates, TxHT = 0 */
#define DESC_RATE6M 0x04
#define DESC_RATE9M 0x05
#define DESC_RATE12M 0x06
#define DESC_RATE18M 0x07
#define DESC_RATE24M 0x08
#define DESC_RATE36M 0x09
#define DESC_RATE48M 0x0a
#define DESC_RATE54M 0x0b

/* MCS Rates, TxHT = 1 */
#define DESC_RATEMCS0 0x0c
#define DESC_RATEMCS1 0x0d
#define DESC_RATEMCS2 0x0e
#define DESC_RATEMCS3 0x0f
#define DESC_RATEMCS4 0x10
#define DESC_RATEMCS5 0x11
#define DESC_RATEMCS6 0x12
#define DESC_RATEMCS7 0x13
#define DESC_RATEMCS8 0x14
#define DESC_RATEMCS9 0x15
#define DESC_RATEMCS10 0x16
#define DESC_RATEMCS11 0x17
#define DESC_RATEMCS12 0x18
#define DESC_RATEMCS13 0x19
#define DESC_RATEMCS14 0x1a
#define DESC_RATEMCS15 0x1b
#define DESC_RATEMCS16 0x1C
#define DESC_RATEMCS17 0x1D
#define DESC_RATEMCS18 0x1E
#define DESC_RATEMCS19 0x1F
#define DESC_RATEMCS20 0x20
#define DESC_RATEMCS21 0x21
#define DESC_RATEMCS22 0x22
#define DESC_RATEMCS23 0x23
#define DESC_RATEMCS24 0x24
#define DESC_RATEMCS25 0x25
#define DESC_RATEMCS26 0x26
#define DESC_RATEMCS27 0x27
#define DESC_RATEMCS28 0x28
#define DESC_RATEMCS29 0x29
#define DESC_RATEMCS30 0x2A
#define DESC_RATEMCS31 0x2B
#define DESC_RATEVHTSS1MCS0 0x2C
#define DESC_RATEVHTSS1MCS1 0x2D
#define DESC_RATEVHTSS1MCS2 0x2E
#define DESC_RATEVHTSS1MCS3 0x2F
#define DESC_RATEVHTSS1MCS4 0x30
#define DESC_RATEVHTSS1MCS5 0x31
#define DESC_RATEVHTSS1MCS6 0x32
#define DESC_RATEVHTSS1MCS7 0x33
#define DESC_RATEVHTSS1MCS8 0x34
#define DESC_RATEVHTSS1MCS9 0x35
#define DESC_RATEVHTSS2MCS0 0x36
#define DESC_RATEVHTSS2MCS1 0x37
#define DESC_RATEVHTSS2MCS2 0x38
#define DESC_RATEVHTSS2MCS3 0x39
#define DESC_RATEVHTSS2MCS4 0x3A
#define DESC_RATEVHTSS2MCS5 0x3B
#define DESC_RATEVHTSS2MCS6 0x3C
#define DESC_RATEVHTSS2MCS7 0x3D
#define DESC_RATEVHTSS2MCS8 0x3E
#define DESC_RATEVHTSS2MCS9 0x3F
#define DESC_RATEVHTSS3MCS0 0x40
#define DESC_RATEVHTSS3MCS1 0x41
#define DESC_RATEVHTSS3MCS2 0x42
#define DESC_RATEVHTSS3MCS3 0x43
#define DESC_RATEVHTSS3MCS4 0x44
#define DESC_RATEVHTSS3MCS5 0x45
#define DESC_RATEVHTSS3MCS6 0x46
#define DESC_RATEVHTSS3MCS7 0x47
#define DESC_RATEVHTSS3MCS8 0x48
#define DESC_RATEVHTSS3MCS9 0x49
#define DESC_RATEVHTSS4MCS0 0x4A
#define DESC_RATEVHTSS4MCS1 0x4B
#define DESC_RATEVHTSS4MCS2 0x4C
#define DESC_RATEVHTSS4MCS3 0x4D
#define DESC_RATEVHTSS4MCS4 0x4E
#define DESC_RATEVHTSS4MCS5 0x4F
#define DESC_RATEVHTSS4MCS6 0x50
#define DESC_RATEVHTSS4MCS7 0x51
#define DESC_RATEVHTSS4MCS8 0x52
#define DESC_RATEVHTSS4MCS9 0x53

/* Base version of the radiotap packet header data */
#define PKTHDR_RADIOTAP_VERSION 0

static inline u16 __get_unaligned_memmove16(const void *p) {
  u16 tmp;
  memmove(&tmp, p, 2);
  return tmp;
}

static inline u32 __get_unaligned_memmove32(const void *p) {
  u32 tmp;
  memmove(&tmp, p, 4);
  return tmp;
}

static inline u16 get_unaligned_le16(const void *p) {
  u16 tmp = __get_unaligned_memmove16((const u8 *)p);
  return le16_to_cpu(tmp);
}

static inline u32 get_unaligned_le32(const void *p) {
  u32 tmp = __get_unaligned_memmove32((const u8 *)p);
  return le32_to_cpu(tmp);
}

/* A generic radio capture format is desirable. There is one for
 * Linux, but it is neither rigidly defined (there were not even
 * units given for some fields) nor easily extensible.
 *
 * I suggest the following extensible radio capture format. It is
 * based on a bitmap indicating which fields are present.
 *
 * I am trying to describe precisely what the application programmer
 * should expect in the following, and for that reason I tell the
 * units and origin of each measurement (where it applies), or else I
 * use sufficiently weaselly language ("is a monotonically nondecreasing
 * function of...") that I cannot set false expectations for lawyerly
 * readers.
 */

/*
 * The radio capture header precedes the 802.11 header.
 * All data in the header is little endian on all platforms.
 */
#ifdef _MSC_VER
#pragma pack(push, 1)
#endif
struct ieee80211_radiotap_header {
  u8 it_version; /* Version 0. Only increases
                  * for drastic changes,
                  * introduction of compatible
                  * new fields does not count.
                  */
  u8 it_pad;
  __le16 it_len;     /* length of the whole
                      * header in bytes, including
                      * it_version, it_pad,
                      * it_len, and data fields.
                      */
  __le32 it_present; /* A bitmap telling which
                      * fields are present. Set bit 31
                      * (0x80000000) to extend the
                      * bitmap by another 32 bits.
                      * Additional extensions are made
                      * by setting bit 31.
                      */
}
#ifdef _MSC_VER
;
#pragma pack(pop)
#else
__attribute__((packed));
#endif

/* Name                                 Data type    Units
 * ----                                 ---------    -----
 *
 * IEEE80211_RADIOTAP_TSFT              __le64       microseconds
 *
 *      Value in microseconds of the MAC's 64-bit 802.11 Time
 *      Synchronization Function timer when the first bit of the
 *      MPDU arrived at the MAC. For received frames, only.
 *
 * IEEE80211_RADIOTAP_CHANNEL           2 x __le16   MHz, bitmap
 *
 *      Tx/Rx frequency in MHz, followed by flags (see below).
 *
 * IEEE80211_RADIOTAP_FHSS              __le16       see below
 *
 *      For frequency-hopping radios, the hop set (first byte)
 *      and pattern (second byte).
 *
 * IEEE80211_RADIOTAP_RATE              u8           500kb/s
 *
 *      Tx/Rx data rate
 *
 * IEEE80211_RADIOTAP_DBM_ANTSIGNAL     s8           decibels from
 *                                                   one milliwatt (dBm)
 *
 *      RF signal power at the antenna, decibel difference from
 *      one milliwatt.
 *
 * IEEE80211_RADIOTAP_DBM_ANTNOISE      s8           decibels from
 *                                                   one milliwatt (dBm)
 *
 *      RF noise power at the antenna, decibel difference from one
 *      milliwatt.
 *
 * IEEE80211_RADIOTAP_DB_ANTSIGNAL      u8           decibel (dB)
 *
 *      RF signal power at the antenna, decibel difference from an
 *      arbitrary, fixed reference.
 *
 * IEEE80211_RADIOTAP_DB_ANTNOISE       u8           decibel (dB)
 *
 *      RF noise power at the antenna, decibel difference from an
 *      arbitrary, fixed reference point.
 *
 * IEEE80211_RADIOTAP_LOCK_QUALITY      __le16       unitless
 *
 *      Quality of Barker code lock. Unitless. Monotonically
 *      nondecreasing with "better" lock strength. Called "Signal
 *      Quality" in datasheets.  (Is there a standard way to measure
 *      this?)
 *
 * IEEE80211_RADIOTAP_TX_ATTENUATION    __le16       unitless
 *
 *      Transmit power expressed as unitless distance from max
 *      power set at factory calibration.  0 is max power.
 *      Monotonically nondecreasing with lower power levels.
 *
 * IEEE80211_RADIOTAP_DB_TX_ATTENUATION __le16       decibels (dB)
 *
 *      Transmit power expressed as decibel distance from max power
 *      set at factory calibration.  0 is max power.  Monotonically
 *      nondecreasing with lower power levels.
 *
 * IEEE80211_RADIOTAP_DBM_TX_POWER      s8           decibels from
 *                                                   one milliwatt (dBm)
 *
 *      Transmit power expressed as dBm (decibels from a 1 milliwatt
 *      reference). This is the absolute power level measured at
 *      the antenna port.
 *
 * IEEE80211_RADIOTAP_FLAGS             u8           bitmap
 *
 *      Properties of transmitted and received frames. See flags
 *      defined below.
 *
 * IEEE80211_RADIOTAP_ANTENNA           u8           antenna index
 *
 *      Unitless indication of the Rx/Tx antenna for this packet.
 *      The first antenna is antenna 0.
 *
 * IEEE80211_RADIOTAP_RX_FLAGS          __le16       bitmap
 *
 *     Properties of received frames. See flags defined below.
 *
 * IEEE80211_RADIOTAP_TX_FLAGS          __le16       bitmap
 *
 *     Properties of transmitted frames. See flags defined below.
 *
 * IEEE80211_RADIOTAP_RTS_RETRIES       u8           data
 *
 *     Number of rts retries a transmitted frame used.
 *
 * IEEE80211_RADIOTAP_DATA_RETRIES      u8           data
 *
 *     Number of unicast retries a transmitted frame used.
 *
 * IEEE80211_RADIOTAP_MCS       u8, u8, u8              unitless
 *
 *     Contains a bitmap of known fields/flags, the flags, and
 *     the MCS index.
 *
 * IEEE80211_RADIOTAP_AMPDU_STATUS      u32, u16, u8, u8        unitless
 *
 *      Contains the AMPDU information for the subframe.
 *
 * IEEE80211_RADIOTAP_VHT       u16, u8, u8, u8[4], u8, u8, u16
 *
 *      Contains VHT information about this frame.
 */
enum ieee80211_radiotap_type {
  IEEE80211_RADIOTAP_TSFT = 0,
  IEEE80211_RADIOTAP_FLAGS = 1,
  IEEE80211_RADIOTAP_RATE = 2,
  IEEE80211_RADIOTAP_CHANNEL = 3,
  IEEE80211_RADIOTAP_FHSS = 4,
  IEEE80211_RADIOTAP_DBM_ANTSIGNAL = 5,
  IEEE80211_RADIOTAP_DBM_ANTNOISE = 6,
  IEEE80211_RADIOTAP_LOCK_QUALITY = 7,
  IEEE80211_RADIOTAP_TX_ATTENUATION = 8,
  IEEE80211_RADIOTAP_DB_TX_ATTENUATION = 9,
  IEEE80211_RADIOTAP_DBM_TX_POWER = 10,
  IEEE80211_RADIOTAP_ANTENNA = 11,
  IEEE80211_RADIOTAP_DB_ANTSIGNAL = 12,
  IEEE80211_RADIOTAP_DB_ANTNOISE = 13,
  IEEE80211_RADIOTAP_RX_FLAGS = 14,
  IEEE80211_RADIOTAP_TX_FLAGS = 15,
  IEEE80211_RADIOTAP_RTS_RETRIES = 16,
  IEEE80211_RADIOTAP_DATA_RETRIES = 17,

  IEEE80211_RADIOTAP_MCS = 19,
  IEEE80211_RADIOTAP_AMPDU_STATUS = 20,
  IEEE80211_RADIOTAP_VHT = 21,
  IEEE80211_RADIOTAP_TIMESTAMP = 22,

  /* valid in every it_present bitmap, even vendor namespaces */
  IEEE80211_RADIOTAP_RADIOTAP_NAMESPACE = 29,
  IEEE80211_RADIOTAP_VENDOR_NAMESPACE = 30,
  IEEE80211_RADIOTAP_EXT = 31
};

struct radiotap_align_size {
  uint8_t align : 4, size : 4;
};

struct ieee80211_radiotap_namespace {
  const struct radiotap_align_size *align_size;
  int n_bits;
  uint32_t oui;
  uint8_t subns;
};

struct ieee80211_radiotap_vendor_namespaces {
  const struct ieee80211_radiotap_namespace *ns;
  int n_ns;
};

/* helpers */
static inline int ieee80211_get_radiotap_len(unsigned char *data) {
  struct ieee80211_radiotap_header *hdr =
      (struct ieee80211_radiotap_header *)data;

  return get_unaligned_le16(&hdr->it_len);
}

/**
 * struct ieee80211_radiotap_iterator - tracks walk thru present radiotap args
 * @this_arg_index: index of current arg, valid after each successful call
 *      to ieee80211_radiotap_iterator_next()
 * @this_arg: pointer to current radiotap arg; it is valid after each
 *      call to ieee80211_radiotap_iterator_next() but also after
 *      ieee80211_radiotap_iterator_init() where it will point to
 *      the beginning of the actual data portion
 * @this_arg_size: length of the current arg, for convenience
 * @current_namespace: pointer to the current namespace definition
 *      (or internally %NULL if the current namespace is unknown)
 * @is_radiotap_ns: indicates whether the current namespace is the default
 *      radiotap namespace or not
 *
 * @_rtheader: pointer to the radiotap header we are walking through
 * @_max_length: length of radiotap header in cpu byte ordering
 * @_arg_index: next argument index
 * @_arg: next argument pointer
 * @_next_bitmap: internal pointer to next present u32
 * @_bitmap_shifter: internal shifter for curr u32 bitmap, b0 set == arg present
 * @_vns: vendor namespace definitions
 * @_next_ns_data: beginning of the next namespace's data
 * @_reset_on_ext: internal; reset the arg index to 0 when going to the
 *      next bitmap word
 *
 * Describes the radiotap parser state. Fields prefixed with an underscore
 * must not be used by users of the parser, only by the parser internally.
 */

struct ieee80211_radiotap_iterator {
  struct ieee80211_radiotap_header *_rtheader;
  const struct ieee80211_radiotap_vendor_namespaces *_vns;
  const struct ieee80211_radiotap_namespace *current_namespace;

  unsigned char *_arg, *_next_ns_data;
  __le32 *_next_bitmap;

  unsigned char *this_arg;
  int this_arg_index;
  int this_arg_size;

  int is_radiotap_ns;

  int _max_length;
  int _arg_index;
  uint32_t _bitmap_shifter;
  int _reset_on_ext;
};

int ieee80211_radiotap_iterator_init(
    struct ieee80211_radiotap_iterator *iterator,
    struct ieee80211_radiotap_header *radiotap_header, int max_length,
    const struct ieee80211_radiotap_vendor_namespaces *vns);

int ieee80211_radiotap_iterator_next(
    struct ieee80211_radiotap_iterator *iterator);

u16 GetSequence(const u8 *packet);
u8 MRateToHwRate(u8 rate);

#endif
