#ifndef FRAMEPARSER_H
#define FRAMEPARSER_H

#include "logger.h"
#include <cstdint>
#include <span>
#include <vector>

typedef unsigned int __u32;

#ifndef __u16
typedef unsigned short __u16;
#endif

typedef signed char s8;
typedef unsigned char u8;

typedef signed short s16;
typedef unsigned short u16;

typedef signed int s32;
typedef unsigned int u32;

#define ETH_ALEN 6
#define TXDESC_SIZE 40
#define OFFSET_SZ 0
#define OFFSET_SHT 16
#define OWN BIT(31)
#define FSG BIT(27)
#define LSG BIT(26)
#define QSEL_SHT 8

#define WriteLE4Byte(_ptr, _val) ((*((u32*)(_ptr))) = cpu_to_le32(_val))
#define WriteLE2Byte(_ptr, _val) ((*((u16*)(_ptr))) = cpu_to_le16(_val))
#define WriteLE1Byte(_ptr, _val) ((*((u8*)(_ptr))) = ((u8)(_val)))

#define BIT_LEN_MASK_32(__BitLen) ((u32)(0xFFFFFFFF >> (32 - (__BitLen))))

#define LE_P4BYTE_TO_HOST_4BYTE(__pStart) (le32_to_cpu(*((u32*)(__pStart))))

#define BIT_OFFSET_LEN_MASK_32(__BitOffset, __BitLen) ((u32)(BIT_LEN_MASK_32(__BitLen) << (__BitOffset)))

#define LE_BITS_CLEARED_TO_4BYTE(__pStart, __BitOffset, __BitLen)                                                      \
    (LE_P4BYTE_TO_HOST_4BYTE(__pStart) & (~BIT_OFFSET_LEN_MASK_32(__BitOffset, __BitLen)))

#define SET_BITS_TO_LE_4BYTE(__pStart, __BitOffset, __BitLen, __Value)                                                 \
    do                                                                                                                 \
    {                                                                                                                  \
        if (__BitOffset == 0 && __BitLen == 32)                                                                        \
            WriteLE4Byte(__pStart, __Value);                                                                           \
        else                                                                                                           \
        {                                                                                                              \
            WriteLE4Byte(__pStart,                                                                                     \
                         LE_BITS_CLEARED_TO_4BYTE(__pStart, __BitOffset, __BitLen) |                                   \
                             ((((u32)__Value) & BIT_LEN_MASK_32(__BitLen)) << (__BitOffset)));                         \
        }                                                                                                              \
    } while (0)

#define SET_TX_DESC_TX_DESC_CHECKSUM_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 28, 0, 16, __Value)

#define SET_TX_DESC_RATE_ID_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 4, 16, 5, __Value)

#define SET_TX_DESC_PKT_SIZE_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc, 0, 16, __Value)
#define SET_TX_DESC_OFFSET_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc, 16, 8, __Value)
#define SET_TX_DESC_BMC_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc, 24, 1, __Value)
#define SET_TX_DESC_HTC_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc, 25, 1, __Value)
#define SET_TX_DESC_LAST_SEG_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc, 26, 1, __Value)
#define SET_TX_DESC_FIRST_SEG_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc, 27, 1, __Value)
#define SET_TX_DESC_LINIP_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc, 28, 1, __Value)
#define SET_TX_DESC_NO_ACM_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc, 29, 1, __Value)
#define SET_TX_DESC_GF_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc, 30, 1, __Value)
#define SET_TX_DESC_OWN_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc, 31, 1, __Value)

/* Dword 1 */
#define SET_TX_DESC_MACID_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 4, 0, 7, __Value)
#define SET_TX_DESC_QUEUE_SEL_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 4, 8, 5, __Value)
#define SET_TX_DESC_RDG_NAV_EXT_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 4, 13, 1, __Value)
#define SET_TX_DESC_LSIG_TXOP_EN_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 4, 14, 1, __Value)
#define SET_TX_DESC_PIFS_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 4, 15, 1, __Value)
#define SET_TX_DESC_RATE_ID_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 4, 16, 5, __Value)
#define SET_TX_DESC_EN_DESC_ID_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 4, 21, 1, __Value)
#define SET_TX_DESC_SEC_TYPE_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 4, 22, 2, __Value)
#define SET_TX_DESC_PKT_OFFSET_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 4, 24, 5, __Value)

/* Dword 2 */
#define SET_TX_DESC_PAID_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 8, 0, 9, __Value)
#define SET_TX_DESC_CCA_RTS_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 8, 10, 2, __Value)
#define SET_TX_DESC_AGG_ENABLE_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 8, 12, 1, __Value)
#define SET_TX_DESC_RDG_ENABLE_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 8, 13, 1, __Value)
#define SET_TX_DESC_AGG_BREAK_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 8, 16, 1, __Value)
#define SET_TX_DESC_MORE_FRAG_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 8, 17, 1, __Value)
#define SET_TX_DESC_RAW_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 8, 18, 1, __Value)
#define SET_TX_DESC_SPE_RPT_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 8, 19, 1, __Value)
#define SET_TX_DESC_AMPDU_DENSITY_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 8, 20, 3, __Value)
#define SET_TX_DESC_BT_INT_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 8, 23, 1, __Value)
#define SET_TX_DESC_GID_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 8, 24, 6, __Value)

/* Dword 3 */
#define SET_TX_DESC_WHEADER_LEN_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 12, 0, 4, __Value)
#define SET_TX_DESC_CHK_EN_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 12, 4, 1, __Value)
#define SET_TX_DESC_EARLY_MODE_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 12, 5, 1, __Value)
#define SET_TX_DESC_HWSEQ_SEL_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 12, 6, 2, __Value)
#define SET_TX_DESC_USE_RATE_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 12, 8, 1, __Value)
#define SET_TX_DESC_DISABLE_RTS_FB_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 12, 9, 1, __Value)
#define SET_TX_DESC_DISABLE_FB_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 12, 10, 1, __Value)
#define SET_TX_DESC_CTS2SELF_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 12, 11, 1, __Value)
#define SET_TX_DESC_RTS_ENABLE_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 12, 12, 1, __Value)
#define SET_TX_DESC_HW_RTS_ENABLE_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 12, 13, 1, __Value)
#define SET_TX_DESC_NAV_USE_HDR_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 12, 15, 1, __Value)
#define SET_TX_DESC_USE_MAX_LEN_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 12, 16, 1, __Value)
#define SET_TX_DESC_MAX_AGG_NUM_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 12, 17, 5, __Value)
#define SET_TX_DESC_NDPA_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 12, 22, 2, __Value)
#define SET_TX_DESC_AMPDU_MAX_TIME_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 12, 24, 8, __Value)

/* Dword 4 */
#define SET_TX_DESC_TX_RATE_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 16, 0, 7, __Value)
#define SET_TX_DESC_DATA_RATE_FB_LIMIT_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 16, 8, 5, __Value)
#define SET_TX_DESC_RTS_RATE_FB_LIMIT_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 16, 13, 4, __Value)
#define SET_TX_DESC_RETRY_LIMIT_ENABLE_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 16, 17, 1, __Value)
#define SET_TX_DESC_DATA_RETRY_LIMIT_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 16, 18, 6, __Value)
#define SET_TX_DESC_RTS_RATE_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 16, 24, 5, __Value)

/* Dword 5 */
#define SET_TX_DESC_DATA_SC_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 20, 0, 4, __Value)
#define SET_TX_DESC_DATA_SHORT_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 20, 4, 1, __Value)
#define SET_TX_DESC_DATA_BW_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 20, 5, 2, __Value)
#define SET_TX_DESC_DATA_LDPC_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 20, 7, 1, __Value)
#define SET_TX_DESC_DATA_STBC_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 20, 8, 2, __Value)
#define SET_TX_DESC_CTROL_STBC_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 20, 10, 2, __Value)
#define SET_TX_DESC_RTS_SHORT_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 20, 12, 1, __Value)
#define SET_TX_DESC_RTS_SC_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 20, 13, 4, __Value)
#define SET_TX_DESC_TX_ANT_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 20, 24, 4, __Value)

/* Dword 6 */
#define SET_TX_DESC_SW_DEFINE_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 24, 0, 12, __Value)
#define SET_TX_DESC_ANTSEL_A_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 24, 16, 3, __Value)
#define SET_TX_DESC_ANTSEL_B_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 24, 19, 3, __Value)
#define SET_TX_DESC_ANTSEL_C_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 24, 22, 3, __Value)
#define SET_TX_DESC_ANTSEL_D_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 24, 25, 3, __Value)
#define SET_TX_DESC_MBSSID_8821(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 24, 12, 4, __Value)

/* Dword 7 */
#define SET_TX_DESC_TX_BUFFER_SIZE_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 28, 0, 16, __Value)
#define SET_TX_DESC_TX_DESC_CHECKSUM_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 28, 0, 16, __Value)
#define SET_TX_DESC_USB_TXAGG_NUM_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 28, 24, 8, __Value)
#ifdef CONFIG_SDIO_HCI
#define SET_TX_DESC_SDIO_TXSEQ_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 28, 16, 8, __Value)
#endif

/* Dword 8 */
#define SET_TX_DESC_HWSEQ_EN_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 32, 15, 1, __Value)

/* Dword 9 */
#define SET_TX_DESC_SEQ_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 36, 12, 12, __Value)

/* Dword 10 */
#define SET_TX_DESC_TX_BUFFER_ADDRESS_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 40, 0, 32, __Value)
#define GET_TX_DESC_TX_BUFFER_ADDRESS_8812(__pTxDesc) LE_BITS_TO_4BYTE(__pTxDesc + 40, 0, 32)

/* Dword 11 */
#define SET_TX_DESC_NEXT_DESC_ADDRESS_8812(__pTxDesc, __Value) SET_BITS_TO_LE_4BYTE(__pTxDesc + 48, 0, 32, __Value)

#define SET_EARLYMODE_PKTNUM_8812(__pAddr, __Value) SET_BITS_TO_LE_4BYTE(__pAddr, 0, 4, __Value)
#define SET_EARLYMODE_LEN0_8812(__pAddr, __Value) SET_BITS_TO_LE_4BYTE(__pAddr, 4, 15, __Value)
#define SET_EARLYMODE_LEN1_1_8812(__pAddr, __Value) SET_BITS_TO_LE_4BYTE(__pAddr, 19, 13, __Value)
#define SET_EARLYMODE_LEN1_2_8812(__pAddr, __Value) SET_BITS_TO_LE_4BYTE(__pAddr + 4, 0, 2, __Value)
#define SET_EARLYMODE_LEN2_8812(__pAddr, __Value) SET_BITS_TO_LE_4BYTE(__pAddr + 4, 2, 15, __Value)
#define SET_EARLYMODE_LEN3_8812(__pAddr, __Value) SET_BITS_TO_LE_4BYTE(__pAddr + 4, 17, 15, __Value)

#define RTW_IEEE80211_FCTL_FTYPE 0x000c
#define RTW_IEEE80211_FCTL_STYPE 0x00f0
#define RTW_IEEE80211_FTYPE_MGMT 0x0000
#define RTW_IEEE80211_STYPE_ACTION 0x00d0

// radiotap
#define MAX_RX_INTERFACES 8

// offset of MCS_FLAGS and MCS index
#define MCS_FLAGS_OFF 11
#define MCS_IDX_OFF 12

// offset of VHT information
#define VHT_FLAGS_OFF 12
#define VHT_BW_OFF 13
#define VHT_MCSNSS0_OFF 14
#define VHT_CODING_OFF 18

// the last four bytes used for channel_id
#define SRC_MAC_THIRD_BYTE 12
#define DST_MAC_THIRD_BYTE 18
#define FRAME_SEQ_LB 22
#define FRAME_SEQ_HB 23

#define FRAME_TYPE_DATA 0x08
#define FRAME_TYPE_RTS 0xb4

#define IEEE80211_RADIOTAP_MCS_HAVE_BW 0x01
#define IEEE80211_RADIOTAP_MCS_HAVE_MCS 0x02
#define IEEE80211_RADIOTAP_MCS_HAVE_GI 0x04
#define IEEE80211_RADIOTAP_MCS_HAVE_FMT 0x08

#define IEEE80211_RADIOTAP_MCS_BW_20 0
#define IEEE80211_RADIOTAP_MCS_BW_40 1
#define IEEE80211_RADIOTAP_MCS_BW_20L 2
#define IEEE80211_RADIOTAP_MCS_BW_20U 3
#define IEEE80211_RADIOTAP_MCS_SGI 0x04
#define IEEE80211_RADIOTAP_MCS_FMT_GF 0x08

#define IEEE80211_RADIOTAP_MCS_HAVE_FEC 0x10
#define IEEE80211_RADIOTAP_MCS_HAVE_STBC 0x20
#define IEEE80211_RADIOTAP_MCS_FEC_LDPC 0x10
#define IEEE80211_RADIOTAP_MCS_STBC_MASK 0x60
#define IEEE80211_RADIOTAP_MCS_STBC_1 1
#define IEEE80211_RADIOTAP_MCS_STBC_2 2
#define IEEE80211_RADIOTAP_MCS_STBC_3 3
#define IEEE80211_RADIOTAP_MCS_STBC_SHIFT 5

#define IEEE80211_RADIOTAP_VHT_FLAG_STBC 0x01
#define IEEE80211_RADIOTAP_VHT_FLAG_SGI 0x04
#define IEEE80211_RADIOTAP_VHT_MCS_MASK 0xF0
#define IEEE80211_RADIOTAP_VHT_NSS_MASK 0x0F
#define IEEE80211_RADIOTAP_VHT_MCS_SHIFT 4
#define IEEE80211_RADIOTAP_VHT_NSS_SHIFT 0
#define IEEE80211_RADIOTAP_VHT_BW_20M 0x00
#define IEEE80211_RADIOTAP_VHT_BW_40M 0x01
#define IEEE80211_RADIOTAP_VHT_BW_80M 0x04
#define IEEE80211_RADIOTAP_VHT_BW_160M 0x0B
#define IEEE80211_RADIOTAP_VHT_CODING_LDPC_USER0 0x01

#define IEEE80211_RADIOTAP_MCS_BW_MASK 0x03

#define MCS_KNOWN                                                                                                      \
    (IEEE80211_RADIOTAP_MCS_HAVE_MCS | IEEE80211_RADIOTAP_MCS_HAVE_BW | IEEE80211_RADIOTAP_MCS_HAVE_GI |               \
     IEEE80211_RADIOTAP_MCS_HAVE_STBC | IEEE80211_RADIOTAP_MCS_HAVE_FEC)

union Keytype
{
    u8 skey[16];
    u32 lkey[4];
};
enum _PUBLIC_ACTION
{
    ACT_PUBLIC_BSSCOEXIST = 0, /* 20/40 BSS Coexistence */
    ACT_PUBLIC_DSE_ENABLE = 1,
    ACT_PUBLIC_DSE_DEENABLE = 2,
    ACT_PUBLIC_DSE_REG_LOCATION = 3,
    ACT_PUBLIC_EXT_CHL_SWITCH = 4,
    ACT_PUBLIC_DSE_MSR_REQ = 5,
    ACT_PUBLIC_DSE_MSR_RPRT = 6,
    ACT_PUBLIC_MP = 7, /* Measurement Pilot */
    ACT_PUBLIC_DSE_PWR_CONSTRAINT = 8,
    ACT_PUBLIC_VENDOR = 9, /* for WIFI_DIRECT */
    ACT_PUBLIC_GAS_INITIAL_REQ = 10,
    ACT_PUBLIC_GAS_INITIAL_RSP = 11,
    ACT_PUBLIC_GAS_COMEBACK_REQ = 12,
    ACT_PUBLIC_GAS_COMEBACK_RSP = 13,
    ACT_PUBLIC_TDLS_DISCOVERY_RSP = 14,
    ACT_PUBLIC_LOCATION_TRACK = 15,
    ACT_PUBLIC_MAX
};

enum class RX_PACKET_TYPE
{
    NORMAL_RX,  /* Normal rx packet */
    TX_REPORT1, /* CCX */
    TX_REPORT2, /* TX RPT */
    HIS_REPORT, /* USB HISR RPT */
    C2H_PACKET
};

struct rx_pkt_attrib
{
    uint16_t pkt_len;
    bool physt;
    uint8_t drvinfo_sz;
    uint8_t shift_sz;
    bool qos;
    uint8_t priority;
    bool mdata;
    uint16_t seq_num;
    uint8_t frag_num;
    bool mfrag;
    bool bdecrypted;
    uint8_t encrypt; /* when 0 indicate no encrypt. when non-zero, indicate the
                        encrypt algorith */
    bool crc_err;
    bool icv_err;
    uint8_t data_rate;
    uint8_t bw;
    uint8_t stbc;
    uint8_t ldpc;
    uint8_t sgi;
    uint8_t rssi[2];
    int8_t snr[2];
    RX_PACKET_TYPE pkt_rpt_type;
};

struct Packet
{
    rx_pkt_attrib RxAtrib;
    std::span<uint8_t> Data;
};

class FrameParser
{
    Logger_t _logger;

  public:
    FrameParser(Logger_t logger);
    std::vector<Packet> recvbuf2recvframe(std::span<uint8_t> ptr);
};

struct pkt_attrib
{
    u8 type;
    u8 subtype;
    u8 bswenc;
    u8 dhcp_pkt;
    u16 ether_type;
    u16 seqnum;
    u8 hw_ssn_sel;  /* for HW_SEQ0,1,2,3 */
    u16 pkt_hdrlen; /* the original 802.3 pkt header len */
    u16 hdrlen;     /* the WLAN Header Len */
    u32 pktlen;     /* the original 802.3 pkt raw_data len (not include ether_hdr data) */
    u32 last_txcmdsz;
    u8 nr_frags;
    u8 encrypt; /* when 0 indicate no encrypt. when non-zero, indicate the encrypt algorith */
                // #if defined(CONFIG_CONCURRENT_MODE)
    u8 bmc_camid;
    // #endif
    u8 iv_len;
    u8 icv_len;
    u8 iv[18];
    u8 icv[16];
    u8 priority;
    u8 ack_policy;
    u8 mac_id;
    u8 vcs_mode; /* virtual carrier sense method */
    u8 dst[ETH_ALEN];
    u8 src[ETH_ALEN];
    u8 ta[ETH_ALEN];
    u8 ra[ETH_ALEN];
    u8 key_idx;
    u8 qos_en;
    u8 ht_en;
    u8 raid; /* rate adpative id */
    u8 bwmode;
    u8 ch_offset;     /* PRIME_CHNL_OFFSET */
    u8 sgi;           /* short GI */
    u8 ampdu_en;      /* tx ampdu enable */
    u8 ampdu_spacing; /* ampdu_min_spacing for peer sta's rx */
    u8 amsdu;
    u8 amsdu_ampdu_en; /* tx amsdu in ampdu enable */
    u8 mdata;          /* more data bit */
    u8 pctrl;          /* per packet txdesc control enable */
    u8 triggered;      /* for ap mode handling Power Saving sta */
    u8 qsel;
    u8 order; /* order bit */
    u8 eosp;
    u8 rate;
    u8 intel_proxim;
    u8 retry_ctrl;
    u8 mbssid;
    u8 ldpc;
    u8 stbc;
    // #ifdef CONFIG_WMMPS_STA
    u8 trigger_frame;
    // #endif /* CONFIG_WMMPS_STA */

    struct sta_info* psta;
    // #ifdef CONFIG_TCP_CSUM_OFFLOAD_TX
    u8 hw_tcp_csum;
    // #endif

    u8 rtsen;
    u8 cts2self;
    union Keytype dot11tkiptxmickey;
    /* union Keytype  dot11tkiprxmickey; */
    union Keytype dot118021x_UncstKey;

    // #ifdef CONFIG_TDLS
    u8 direct_link;
    struct sta_info* ptdls_sta;
    // #endif /* CONFIG_TDLS */
    u8 key_type;

    u8 icmp_pkt;

    // #ifdef CONFIG_BEAMFORMING
    u16 txbf_p_aid; /*beamforming Partial_AID*/
    u16 txbf_g_id;  /*beamforming Group ID*/

    /*
     * 2'b00: Unicast NDPA
     * 2'b01: Broadcast NDPA
     * 2'b10: Beamforming Report Poll
     * 2'b11: Final Beamforming Report Poll
     */
    u8 bf_pkt_type;
    // #endif
    u8 inject; /* == a5 if injected */
};

#if defined(_MSC_VER)
#pragma pack(push, 1)
#endif
struct rtw_ieee80211_hdr
{
    u16 frame_ctl;
    u16 duration_id;
    u8 addr1[ETH_ALEN];
    u8 addr2[ETH_ALEN];
    u8 addr3[ETH_ALEN];
    u16 seq_ctl;
}
#if !defined(_MSC_VER)
 __attribute__((packed))
#endif
;
#if defined(_MSC_VER)
#pragma pack(pop)
#endif

#if defined(_MSC_VER)
#pragma pack(push, 1)
#endif
struct rtw_ieee80211_hdr_3addr
{
    u16 frame_ctl;
    u16 duration_id;
    u8 addr1[ETH_ALEN];
    u8 addr2[ETH_ALEN];
    u8 addr3[ETH_ALEN];
    u16 seq_ctl;
}
#if !defined(_MSC_VER)
 __attribute__((packed))
#endif
;
#if defined(_MSC_VER)
#pragma pack(pop)
#endif

#if defined(_MSC_VER)
#pragma pack(push, 1)
#endif
struct rtw_ieee80211_hdr_qos
{
    u16 frame_ctl;
    u16 duration_id;
    u8 addr1[ETH_ALEN];
    u8 addr2[ETH_ALEN];
    u8 addr3[ETH_ALEN];
    u16 seq_ctl;
    u8 addr4[ETH_ALEN];
    u16 qc;
}
#if !defined(_MSC_VER)
 __attribute__((packed))
#endif
;
#if defined(_MSC_VER)
#pragma pack(pop)
#endif

#if defined(_MSC_VER)
#pragma pack(push, 1)
#endif
struct rtw_ieee80211_hdr_3addr_qos
{
    u16 frame_ctl;
    u16 duration_id;
    u8 addr1[ETH_ALEN];
    u8 addr2[ETH_ALEN];
    u8 addr3[ETH_ALEN];
    u16 seq_ctl;
    u16 qc;
}
#if !defined(_MSC_VER)
 __attribute__((packed))
#endif
;
#if defined(_MSC_VER)
#pragma pack(pop)
#endif

#if defined(_MSC_VER)
#pragma pack(push, 1)
#endif
struct eapol
{
    u8 snap[6];
    u16 ethertype;
    u8 version;
    u8 type;
    u16 length;
}
#if !defined(_MSC_VER)
 __attribute__((packed))
#endif
;
#if defined(_MSC_VER)
#pragma pack(pop)
#endif

struct tx_desc
{
    unsigned int txdw0; // First descriptor field, typically contains packet length and other control information
    unsigned int txdw1; // Second descriptor field, possibly includes MAC ID, queue selection, etc.
    unsigned int txdw2; // Third descriptor field, reserved or for specific purposes
    unsigned int txdw3; // Fourth descriptor field, sequence number, and frame control flags
    unsigned int txdw4; // Fifth descriptor field, more control flags such as transmission rate, status, etc.
    unsigned int txdw5; // Sixth descriptor field, reserved
    unsigned int txdw6; // Seventh descriptor field, possibly used for additional status information
    unsigned int txdw7; // Eighth descriptor field, specifies how the packet should be processed

    unsigned int txdw8; // Additional field in a 40-byte descriptor
    unsigned int txdw9; // Another additional field
};

void rtl8812a_cal_txdesc_chksum(uint8_t* ptxdesc);
#endif /* FRAMEPARSER_H */
