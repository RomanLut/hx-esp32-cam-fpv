#ifndef JAGUAR2_CHIP_VARIANT_H
#define JAGUAR2_CHIP_VARIANT_H

namespace jaguar2 {

/* Which Jaguar2-generation chip a device is. Both variants share the HalMAC
 * firmware-download / MAC-init / power-sequence path and the older phydm
 * `check_positive` BB/AGC/RF table format (walked by the shared
 * src/PhyTableLoader); they differ in the firmware blob, the PHY/RF table DATA,
 * the txpwr_lmt table, the IQK algorithm, and the RF-path count.
 *
 *   C8822B — RTL8822BU: 2T2R, WiFi+BT combo  (SYS_CFG2 chip-id 0x0a; cold 0x50)
 *   C8821C — RTL8811CU / RTL8821CU: 1T1R AC600 + BT combo (SYS_CFG2 chip-id 0x09,
 *            hardware-verified on a CF-811AC; RF_TYPE bit27=0 => 1T1R)
 *
 * Resolved at construction in WiFiDriver::CreateRtlDevice from the SYS_CFG2
 * (0x00FC) chip-id byte and threaded into HalJaguar2 (table/RF-path select),
 * HalmacJaguar2Fw (blob select) and the calibration factory. */
enum class ChipVariant { C8822B, C8821C };

} /* namespace jaguar2 */

#endif /* JAGUAR2_CHIP_VARIANT_H */
