# Image resolution selection

**OV2640**

**esp32cam** and **esp32s3sence** boards often come with the **OV2640** sensor by default. 

The sweet spot settings for this camera seems to be 800x600 resolution with JPEG compression level in range 8…63 (lower is better quality). 30 fps is achieved. Additionaly, custom 16:9 modes 640x360 and 800x456 are implemented. Personally I like 800x456 because 16:9 looks more "digital" :)

Another options are 640x480 and 640x360, which can have better JPEG compression level set per frame, but luck pixel details and thus do not benefit over 800x456.

Any resolution lower then 640x360, despite high frame rate (60fps can be achieved at 320x240), is useless in my opinion due to luck of details.

**ov2640** can capture 1280x720 at 13 FPS. Image looks Ok on 7" screen, but FPS is definitely lower then acceptable level for FPV glasses.

It is possible to overclock **ov2640** sensor in **Camera Settings** to enable 40Fps in 640x360, 640x480 and 800x456 modes, however it is not garantied to work. If it does not work - try with another sensor.

**ov2640** is Ok for day but has much worse light sensitivity and dynamic range compared to **ov5640** in the evening. This and the next video are made in almost the same light conditions:

800x456 30fps MCS3 26Mbps (actual transfer rate ~10Mbps totalwith FEC 6/12) with ov2640 camera 120° M8 lens:

https://github.com/RomanLut/hx-esp32-cam-fpv/assets/11955117/9e3b3920-04c3-46fd-9e62-9f3c5c584a0d

**OV3660**

The **OV3660** has a larger image sensor than the OV2640 and supports higher resolutions and frame rates. However, there do not appear to be any **OV3660** modules on the market with large-lens optics.

In general, upgrading to the **OV5640** is the better choice, as it offers superior overall image quality and performance.

One exception is low-light performance. The **OV3660** image may appear slightly better because the **OV5640** tends to exhibit a fixed vertical line pattern in dark scenes, whereas the **OV3660** produces more uniform noise, which is generally less distracting to the eye.

**OV5640**

**OV5640** supports the same resolutions and offers the same FPS thanks to binning support, but also have much better light sensivity, brightness and contrast. It also has higher pixel rate and supports 1280x720 30fps (which can be received by **esp32s3** thanks to 2x maximum DMA speed).

800x456 image looks much better on **ov5640** compared to **ov2640** thanks to highger sensor quality.

It is possible to enable **50FPS** 640x360 and 800x456 modes is **Camera Settings**. Unfortunatelly, camera seems to distort colors in low light conditions in these modes (flying in the evening).

While **ov5640** can do **50FPS** in higher resolution modes, it does not make a sense to use them because higher FPS requires too high bandwidth for MJPEG stream. 

**Note: ov5640** does not support **vertical image flip**.

800x456 30fps MCS3 26Mbps (~10Mbps actual transfer rate total with FEC 6/12) with ov5640 camera 160° M8 lens:

https://github.com/RomanLut/hx-esp32-cam-fpv/assets/11955117/cbc4af6c-e31f-45cf-9bb4-2e1dd850a5d8

## HQ DVR Mode

While **ov5640** can capture 1280x720 30fps,  this mode requires too high bandwidth, so system has to set high compression levels which elliminate detais. There is no sense to use this mode for FPV on 2.4Ghz band.

Since release 0.2.1, 1280x720 mode works in "HQ DVR" mode on **esp32/esp32s3**: video is saved with best possible quality limited by SD card performance only on Air unit, while frames are sent as fast as link allows (usually 5-10 FPS).

Mode is usefull for recording video which can be watched on big screen.

An example of DVR recording on **esp32s3**:

https://github.com/user-attachments/assets/b0c2f0b5-2106-4702-b434-837e8ce5914b

**esp32c5** is capable sending 1280x720 25 fps on **5.8Ghz** band. HG DVR mode is not present on this board.
