# Adaptive compression

With the same JPEG compression level the size of a frame can vary a lot depending on scenery. A lot means order of 5 or more. Adaptive compression is implemented to achieve best possible image quality.

For **ov2640** sensor, compression level can be set in range 1..63 (lower is better quality). However **ov2640** can return broken frames or crash with compression levels lower then 8. Also, decreasing compression level below 8 increases frame size but does not increase image quality much due to bad sensor quality itself. System uses range 8...63.

Frame data flows throught a number of queues, which can easily be overloaded due to small RAM size on ESP32, see [profiling](https://github.com/RomanLut/hx-esp32-cam-fpv/blob/master/doc/development.md#profiling).

Air unit calculates 3 coefficients which are used to adjust compression quality, where 8 is minumum compression level and each coefficient can increase it up to 63.

Theoretical maximum bandwidth of current Wifi rate is multipled by 0.5 (50%), divided by FEC redundancy **(FEC_n / FEC_k)** and divided by FPS. The result is target frame size.

Additionally, compression level is limited by maximum SD write speed when air unit DVR is enabled; it is 0.8Mb/sec **ESP32** and 1.8Mb/sec for **esp32s3sense**.  **ESP32** can write at 1.9Mb/sec but can not keep up such speed due to high overall system load.

Additionally, frame size is decreased if Wifi output queue grows (Wifi channel is shared between multiple devices. Practical bandwidth can be much lower then expected). **This is the most limiting factor**.

Adaptive compression level is a key component of **hx-esp32-cam-fpv**. Without adaptive compression, compression level have to be set to so low quality, that system became unusable.
