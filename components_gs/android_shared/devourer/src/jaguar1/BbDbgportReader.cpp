#include "BbDbgportReader.h"

namespace devourer {

namespace {
constexpr uint16_t kRegSysCfg = 0x00F0;  // hal/hal_com_reg.h:101
}  // namespace

BbDbgportReader::BbDbgportReader(RtlAdapter& device, Logger_t logger)
    : _device{device}, _logger{logger} {}

bool BbDbgportReader::is_chip_alive() {
  uint32_t v = _device.rtw_read32(kRegSysCfg);
  /* All-zeros or all-ones reads typically indicate libusb returned an
   * error and zeroed/poisoned the buffer; a healthy SYS_CFG read always
   * has *some* bit set in the upper half (chip-id / cut-version). */
  if (v == 0 || v == 0xFFFFFFFFu) {
    _logger->warn("BbDbgportReader: SYS_CFG read returned 0x{:08x} — chip "
                  "appears wedged", v);
    return false;
  }
  return true;
}

uint32_t BbDbgportReader::read_dbgport(uint32_t selector) {
  if (_wedged) {
    /* Refuse further writes once we've seen a dead-chip — fail loud, not
     * silently corrupt subsequent samples. */
    return 0;
  }
  uint32_t saved = _device.rtw_read32(kDbgPortSelectorReg);
  _device.rtw_write32(kDbgPortSelectorReg, selector);
  uint32_t value = _device.rtw_read32(kDbgPortReadbackReg);
  _device.rtw_write32(kDbgPortSelectorReg, saved);

  if (!is_chip_alive()) {
    _wedged = true;
    _logger->error("BbDbgportReader: chip wedged after selector=0x{:08x} "
                   "(value=0x{:08x}). Recovery: libusb_reset_device, "
                   "usbreset, power-cycle. Reader will refuse further "
                   "writes until reconstructed.", selector, value);
  }
  return value;
}

}  // namespace devourer
