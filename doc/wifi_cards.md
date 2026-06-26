# Known Wifi cards

**RTL8812AU-based** cards are recommended for the project.

*High power output on the ground station (GS) is not critical for the esp32cam-fpv project, as the range is primarily limited by the ESP32's maximum output of 20 dBm. Additionally, to the best of my knowledge, there are very few RTL8812AU-based cards on the market with a power amplifier (PA) on the 2.4 GHz band. Most RTL8812AU cards advertised as "high output power" include a PA only on the 5 GHz band. On 2.4 GHz, the output is limited to the bare RTL8812AU chip, which typically delivers around 16–17 dBm at lower data rates.*

**RTL8821AU** will work but are not recommended due to single antenna.

**AR9271** should also work but not tested. **RTL8812AU** has antena diversity and thus is recommended over **AR9271**.

**RTL8812EU** and **RTL8812EU2** support **5.8Ghz** band only and thus can be used with **esp32c5** air unit only.

### Noname RTL8812AU

Card can be powered from 5V and comes with good 5dBi dualband antenas.

Equipped with two SKY85601 amplifiers. 5GHz: 12dB LNA, no PA. 2.4GHz: No LNA and PA. Theoretical output power: 5Ghz: 11-13 dBm, 2.4Ghz: 16-17 dBm.

![alt text](/doc/images/rtl8812au.jpg "rtl8812au")


### Comfast CF-912AC (RTL8812AU)

Equipped with two SKY85703 amplifiers. 5GHz: 12dB LNA, 19dBm output PA. 2.4Ghz: No LNA and PA. Theoretical output power: 5Ghz: 19 dBm, 2.4Ghz: 16-17 dBm.

Recommended. You will have to solder IPX antena connectors. Adding metal cover is also recommended.

![alt text](/doc/images/comfast.jpg "comfast rtl8812au")

### TPLink Archer T2U Plus (RTL8821AU)

Chip has marking **RTL8811AU** but there is **RTL8821AU** silicon inside. 

5GHZ LNA/PA chip has markings **38he 1522**, possibly **RichWave RTC5633C**. 5GHz: 12dB LNA, 18dBm output PA. 2.4Ghz: No LNA and PA. Theoretical output power: 5Ghz: 18 dBm, 2.4Ghz: 16-17 dBm.

Not recommended for single adapter configuration. Might be acceptable for dual adapter configurations.

![alt text](/doc/images/tplink_t2u_plus.jpg "plink t2u plus")
