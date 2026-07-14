#ifndef RTLUSBADAPTER_H
#define RTLUSBADAPTER_H

/* Compatibility shim. The class once named RtlUsbAdapter is now the
 * bus-neutral RtlAdapter (src/RtlAdapter.h); the USB wire specifics moved to
 * devourer::UsbTransport (src/UsbTransport.h) behind the IRtlTransport seam
 * (src/RtlTransport.h). Prefer RtlAdapter in new code. */

#include "RtlAdapter.h"

using RtlUsbAdapter = RtlAdapter;

#endif /* RTLUSBADAPTER_H */
