# Wifi

Default wifi channel is set to 7. 3…7 seems to be the best setting, because antennas are tuned for the middle range. F.e. in my experiments, channel 11 barely works with **AR9271** and **esp32s3sense** stock antenna. In the crowded wifi environment, best channel is the one not used by other devices. System may not be able to push frames on busy channel at all (high wifi queue usage will be shown on OSD).
**esp32c5** supports 5.8Ghz band (channes 44..165). Please use channnnes allow in your region (ETSI/FCC, avoid DFS channels).

## Wifi rate

24Mbps or MCS3 26Mbps seems to be the sweet spot which provides high bandwidth and range. 24 is Wifi rate; actual bandwith is ~8-14Mbps total ( including FEC 6/12). Full 24Mbps transfer rate is not achievable.

Lowering bandwidth to 12Mbps seems to not provide any range improvement; reception still drops at -83dB. 

Increasing bandwidth to 36Mbps allows to send less compressed frames, but decreases range to 600m. 36Mbps bandwidth is not fully used because practical maximum **ESP32** bandwidth seems to be 2.3Mb/sec (23Mbps) in ideal conditions ( 29Mbps or 2.9Mb/sec for **ESP32s**). On the field, practical transfer rate is ~14Mbps max.

When Air Recording is enabled, rate is also limited by SD write speed 0.8Mb/sec (8Mbps without FEC) for **ESP32** and 1.8Mb/sec (18Mbps without FEC) for **ESP32s3**. 

## Wifi interferrence 

Wifi channel is shared between multiple clients. In crowded area, bandwith can be significanly lower then expected. While tested on table at home, **hx-esp32-cam-fpv** can show ~5FPS due to low bandwidth and high packet loss; this is normal.

Note than UAV in the air will sense carrier of all Wifi routers around and share wifi channel bandwidth with them (See [Carrier-sense multiple access with collision avoidance (CSMA/CA)](https://www.geeksforgeeks.org/carrier-sense-multiple-access-csma/) )

## Wifi Band

**esp32c5** supports both **2.4GHz** and **5.8GHz** bands. Other air units support **2.4Ghz** only.

**5.8Ghz** band has much less interferrence and allows using lower FEC ratio(6/8). Higher transfer speed and lower FEC ratios allows sending beter image quality.

**2.4Ghz** band is usefull with highest FEC ratio only (6/12).

**5.8Ghz** band has slightly smaller range (about 30% smaller) compared to **2.4Ghz**.
