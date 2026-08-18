#include <cstdint>

#include "ieee80211_radiotap.h"

/* Radiotap header iteration
 *   implemented in net/wireless/radiotap.c
 *   docs in Documentation/networking/radiotap-headers.txt
 */
/**
 * struct ieee80211_radiotap_iterator - tracks walk thru present radiotap args
 * @rtheader: pointer to the radiotap header we are walking through
 * @max_length: length of radiotap header in cpu byte ordering
 * @this_arg_index: IEEE80211_RADIOTAP_... index of current arg
 * @this_arg: pointer to current radiotap arg
 * @arg_index: internal next argument index
 * @arg: internal next argument pointer
 * @next_bitmap: internal pointer to next present u32
 * @bitmap_shifter: internal shifter for curr u32 bitmap, b0 set == arg present
 */

//===================================================================================
//===================================================================================
// Tracks presence bitmaps, namespaces, and field data while parsing one radiotap header.
struct ieee80211_radiotap_iterator
{
	struct ieee80211_radiotap_header *rtheader;
	int max_length;
	int this_arg_index;
    uint8_t *this_arg;
	int this_arg_size;

	int arg_index;
    uint8_t *arg;
	uint8_t *next_bitmap;
    uint32_t bitmap_shifter;
	bool reset_on_ext;
	bool in_radiotap_namespace;
};

extern int ieee80211_radiotap_iterator_init(
   struct ieee80211_radiotap_iterator *iterator,
   struct ieee80211_radiotap_header *radiotap_header,
   int max_length);

extern int ieee80211_radiotap_iterator_next(
   struct ieee80211_radiotap_iterator *iterator);

