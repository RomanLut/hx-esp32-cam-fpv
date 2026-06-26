# OSD Menu

## OSD Elements

![alt text](/doc/images/osd_elements.png "osd_elements")

From left to right:

* ```AIR:-10``` Air Unit RSSI in Dbm
* ```GS:-14:-13``` GS RSSI in Dbm on each wifi card
* Average wifi queue usage. Should be below 50%. Look for free wifi channel if it turns red frequently
* actual MJPEG stream bandwidth in Mbps (without FEC encoding). Wifi stream bandwwith = MJPEG stream bandwidth * FEC_n / FEC_k (2x for FEC 6/12)
* resolution
* FPS at GS
* ```!NO PING!``` Indicates that air unit does not receive GS packets (configuration packets, uplink Mavlink)
* ```AIR``` Air unit is recording video to SD card
* ```GS``` GS is recording video to SD card
* ```HQ DVR``` HQ DVR mode enabled
* ```!SD SLOW!``` SD card on AIR unit is too slow to record video, frames are skipped
* ```OFF``` Camera is stopped by RC channel
* ```Air: 112°``` Air unit temperature exceeded 110° (overheat)
* ```GS: 90°``` GS CPU temperature exceeded 80° (overheat). Throttling and degraded performance may occur.

## OSD Menu navigation

![alt text](/doc/images/osd_menu.jpg "osd_menu")

OSD Menu can be navigated with **GPIO Joystick**, keyboard or mouse.

| Key                                                    | Function |
|--------------------------------------------------------|----------|
| Joystick Center, Enter, Right Click                    | Open OSD menu |
| Joystick Right, Air REC, GS REC, Esc, Right Click, R, G | Close OSD Menu |
| Joystick Center, Joystick Right, Enter, Left Click     | Select menu item |
| Joystick Up, Arrow Up                                  | Select previous menu item |
| Joystick Down, Arrow Down                              | Select next menu item |
| Joystick Left, Arrow Left, Esc                         | Exit to previous menu |

## GPIO Joystick button mapping

GPIO Joystick and buttons are mapped to keys.

| Key             | Function |
|-----------------|----------|
| Joystick Center | Enter |
| Joystick Left   | Arrow Left |
| Joystick Right  | Arrow Right |
| Joystick Up     | Arrow Up |
| Joystick Down   | Arrow Down |
| AIR REC         | r |
| GROUND REC      | g |

## Other keys

| Key   | Function |
|-------|----------|
| Space | Exit application |
| Esc   | Close OSD menu or exit application |
| d     | Open Development UI |
