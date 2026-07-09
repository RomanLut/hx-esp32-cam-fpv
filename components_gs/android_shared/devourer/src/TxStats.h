#ifndef DEVOURER_TX_STATS_H
#define DEVOURER_TX_STATS_H

#include <cstdint>

namespace devourer {

/* TX submission health — the driver-side drop signal an adaptive-link
 * controller uses to detect congestion (the "xtx" cut-bitrate trigger). Counted
 * at the USB bulk-OUT layer, so it is family-agnostic (all three HALs push TX
 * through the same RtlAdapter).
 *
 * `submitted` counts every frame handed to the USB stack; `failed` counts those
 * that did not complete OK — either a synchronous submit/transfer error or an
 * async URB completion with a non-OK status. The distinction that matters for
 * congestion vs. a dead path is `last_was_timeout`: a bulk-OUT TIMEOUT
 * (LIBUSB_ERROR_TIMEOUT / LIBUSB_TRANSFER_TIMED_OUT) is the chip NAKing because
 * its TX FIFO is full — recoverable back-pressure, the xtx case — whereas a
 * hard error (NO_DEVICE, pipe stall, ...) is a broken link. `last_error_rc` is
 * the raw libusb code of the most recent failure (0 = none yet). */
struct TxStats {
  uint64_t submitted = 0;
  uint64_t failed = 0;
  int last_error_rc = 0;      /* raw libusb rc / negated transfer status */
  bool last_was_timeout = false;
};

} // namespace devourer

#endif /* DEVOURER_TX_STATS_H */
