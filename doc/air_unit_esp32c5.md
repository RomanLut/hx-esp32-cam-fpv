# ESP32-C5 Air Unit

The **esp32c5** supports both **2.4GHz** and **5.8GHz** bands. Thanks to lower interference on **5.8GHz**, the air unit can transmit a 14 Mb/s stream with a lower FEC ratio, resulting in better image quality. It is capable of broadcasting a 1280×720 stream at 25 FPS. It allows using the **RunCam WiFiLink VRX** as a ground station and allows using small **5.8GHz** lollipop antennas with circular polarization.

Unfortunately, there are currently no **esp32c5** boards on the market with a good form factor and a proper camera connector. Using an **esp32c5** as an air unit requires advanced hardware skills and custom assembly. For this reason, the **esp32s3sense** remains the best choice for an air unit for now.

![alt text](/doc/images/esp32c5_air_unit.jpg "esp32c5_air_unit")

See [Building esp32c5 air unit](/doc/esp32c5_air_unit.md) for the detailed hardware build.

## Current consumption

**esp32c5** consumes about 540mA.
