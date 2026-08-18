/*
 * Radiotap parser
 *
 * Copyright 2007 Andy Green <andy@warmcat.com>
 */

#include "radiotap.h"

#include <cerrno>
#include <cstdint>
#include <cstring>

namespace
{

//===================================================================================
//===================================================================================
// Describes the alignment and encoded size of one standard radiotap field.
struct RadiotapFieldLayout
{
    uint8_t alignment;
    uint8_t size;
};

// Standard radiotap fields that GS consumes or must skip to reach repeated
// antenna namespaces emitted by the rtl8812au monitor-mode driver.
constexpr RadiotapFieldLayout kRadiotapFieldLayouts[] = {
    {8, 8},  // IEEE80211_RADIOTAP_TSFT
    {1, 1},  // IEEE80211_RADIOTAP_FLAGS
    {1, 1},  // IEEE80211_RADIOTAP_RATE
    {2, 4},  // IEEE80211_RADIOTAP_CHANNEL
    {2, 2},  // IEEE80211_RADIOTAP_FHSS
    {1, 1},  // IEEE80211_RADIOTAP_DBM_ANTSIGNAL
    {1, 1},  // IEEE80211_RADIOTAP_DBM_ANTNOISE
    {2, 2},  // IEEE80211_RADIOTAP_LOCK_QUALITY
    {2, 2},  // IEEE80211_RADIOTAP_TX_ATTENUATION
    {2, 2},  // IEEE80211_RADIOTAP_DB_TX_ATTENUATION
    {1, 1},  // IEEE80211_RADIOTAP_DBM_TX_POWER
    {1, 1},  // IEEE80211_RADIOTAP_ANTENNA
    {1, 1},  // IEEE80211_RADIOTAP_DB_ANTSIGNAL
    {1, 1},  // IEEE80211_RADIOTAP_DB_ANTNOISE
    {2, 2},  // IEEE80211_RADIOTAP_RX_FLAGS
    {2, 2},  // IEEE80211_RADIOTAP_TX_FLAGS
    {1, 1},  // IEEE80211_RADIOTAP_RTS_RETRIES
    {1, 1},  // IEEE80211_RADIOTAP_DATA_RETRIES
    {0, 0},  // Reserved
    {1, 3},  // IEEE80211_RADIOTAP_MCS
    {4, 8},  // IEEE80211_RADIOTAP_AMPDU_STATUS
    {2, 12}, // IEEE80211_RADIOTAP_VHT
};

} // namespace

//===================================================================================
//===================================================================================
// Initializes an iterator and skips every extended presence bitmap before field data.
int ieee80211_radiotap_iterator_init(
    struct ieee80211_radiotap_iterator* iterator,
    struct ieee80211_radiotap_header* radiotap_header,
    int max_length)
{
    if (iterator == nullptr || radiotap_header == nullptr ||
        max_length < static_cast<int>(sizeof(*radiotap_header)))
    {
        return -EINVAL;
    }

    if (radiotap_header->it_version != PKTHDR_RADIOTAP_VERSION)
    {
        return -EINVAL;
    }

    uint16_t header_length = 0;
    uint32_t first_present_bitmap = 0;
    std::memcpy(&header_length, &radiotap_header->it_len, sizeof(header_length));
    std::memcpy(&first_present_bitmap, &radiotap_header->it_present, sizeof(first_present_bitmap));
    if (header_length < sizeof(*radiotap_header) || header_length > max_length)
    {
        return -EINVAL;
    }

    iterator->rtheader = radiotap_header;
    iterator->max_length = static_cast<int>(header_length);
    iterator->this_arg_index = -1;
    iterator->this_arg = nullptr;
    iterator->this_arg_size = 0;
    iterator->arg_index = 0;
    iterator->bitmap_shifter = first_present_bitmap;
    iterator->arg = reinterpret_cast<uint8_t*>(radiotap_header) + sizeof(*radiotap_header);
    iterator->next_bitmap = iterator->arg;
    iterator->reset_on_ext = false;
    iterator->in_radiotap_namespace = true;

    // rtl8812au reports each RF path in another standard radiotap namespace.
    // All presence words precede field data, so find their end before iteration.
    uint32_t present_bitmap = first_present_bitmap;
    while ((present_bitmap & (1U << IEEE80211_RADIOTAP_EXT)) != 0)
    {
        const size_t bitmap_offset = static_cast<size_t>(iterator->arg -
            reinterpret_cast<uint8_t*>(radiotap_header));
        if (bitmap_offset + sizeof(present_bitmap) > header_length)
        {
            return -EINVAL;
        }

        std::memcpy(&present_bitmap, iterator->arg, sizeof(present_bitmap));
        iterator->arg += sizeof(present_bitmap);
    }

    return 0;
}

//===================================================================================
//===================================================================================
// Returns the next standard or raw vendor radiotap field, including repeated namespaces.
int ieee80211_radiotap_iterator_next(struct ieee80211_radiotap_iterator* iterator)
{
    if (iterator == nullptr || iterator->rtheader == nullptr)
    {
        return -EINVAL;
    }

    while (true)
    {
        const int namespace_index = iterator->arg_index % 32;
        if (namespace_index == IEEE80211_RADIOTAP_EXT &&
            (iterator->bitmap_shifter & 1U) == 0)
        {
            return -ENOENT;
        }

        if ((iterator->bitmap_shifter & 1U) == 0)
        {
            iterator->bitmap_shifter >>= 1;
            ++iterator->arg_index;
            continue;
        }

        if (namespace_index == IEEE80211_RADIOTAP_RADIOTAP_NAMESPACE)
        {
            iterator->reset_on_ext = true;
            iterator->in_radiotap_namespace = true;
            iterator->bitmap_shifter >>= 1;
            ++iterator->arg_index;
            continue;
        }

        if (namespace_index == IEEE80211_RADIOTAP_EXT)
        {
            uint32_t next_present_bitmap = 0;
            std::memcpy(&next_present_bitmap, iterator->next_bitmap, sizeof(next_present_bitmap));
            iterator->next_bitmap += sizeof(next_present_bitmap);
            iterator->bitmap_shifter = next_present_bitmap;
            iterator->arg_index = iterator->reset_on_ext ? 0 : iterator->arg_index + 1;
            iterator->reset_on_ext = false;
            continue;
        }

        size_t alignment = 0;
        size_t field_size = 0;
        if (namespace_index == IEEE80211_RADIOTAP_VENDOR_NAMESPACE)
        {
            alignment = 2;
            field_size = 6;
        }
        else
        {
            if (!iterator->in_radiotap_namespace || iterator->arg_index < 0 ||
                iterator->arg_index >= static_cast<int>(sizeof(kRadiotapFieldLayouts) /
                                                        sizeof(kRadiotapFieldLayouts[0])))
            {
                return -ENOENT;
            }

            const RadiotapFieldLayout layout = kRadiotapFieldLayouts[iterator->arg_index];
            alignment = layout.alignment;
            field_size = layout.size;
            if (alignment == 0)
            {
                return -ENOENT;
            }
        }

        const size_t current_offset = static_cast<size_t>(iterator->arg -
            reinterpret_cast<uint8_t*>(iterator->rtheader));
        const size_t padding = current_offset & (alignment - 1);
        if (padding != 0)
        {
            iterator->arg += alignment - padding;
        }

        if (namespace_index == IEEE80211_RADIOTAP_VENDOR_NAMESPACE)
        {
            const size_t vendor_header_offset = static_cast<size_t>(iterator->arg -
                reinterpret_cast<uint8_t*>(iterator->rtheader));
            if (vendor_header_offset + field_size > static_cast<size_t>(iterator->max_length))
            {
                return -EINVAL;
            }

            uint16_t vendor_payload_size = 0;
            std::memcpy(&vendor_payload_size, iterator->arg + 4, sizeof(vendor_payload_size));
            field_size += vendor_payload_size;
            iterator->reset_on_ext = true;
            iterator->in_radiotap_namespace = false;
        }

        const size_t field_offset = static_cast<size_t>(iterator->arg -
            reinterpret_cast<uint8_t*>(iterator->rtheader));
        if (field_offset + field_size > static_cast<size_t>(iterator->max_length))
        {
            return -EINVAL;
        }

        iterator->this_arg_index = namespace_index;
        iterator->this_arg = iterator->arg;
        iterator->this_arg_size = static_cast<int>(field_size);
        iterator->arg += field_size;
        iterator->bitmap_shifter >>= 1;
        ++iterator->arg_index;

        return 0;
    }
}
