# Flashing esp32s3sense

## Flashing using online tool

* Download and uncompress prebuilt firmware files from https://github.com/RomanLut/hx-esp32-cam-fpv/releases
* Navigate to https://esp.huhn.me/
* Connect esp32s3sense to USB, click **[Connect]**, select USB UART of **esp32s3sense**
* Add firmware files as shown on screenshot:
 
![alt text](images/espwebtool.png "espwebtool_s3sense.png")

* Make sure addresses are filled corectly
* Click **[Program]**

## Flashing using Flash download tool

* Download and uncompress prebuilt firmware files from https://github.com/RomanLut/hx-esp32-cam-fpv/releases
* Download and uncommpress Flash Download tools https://www.espressif.com/en/support/download/other-tools
* Start Flash Download Tools, select esp32s3:

![alt text](images/flash_download_tool_esp32s3.png "flash_download_tool_esp32s3.png")
 
* Connect **esp32s3sense** to USB
* Add firmware files as shown on screenshot:
 
![alt text](images/flash_download_tool_files.png "flash_download_tool_files_s3sense.png")

* Make sure checkboxes are selected
* Make sure addresses are filled corectly
* Make sure files are selected in correct order
* Click **[Start]**


## Building and Flashing using PlatformIO

* Download and install PlatformIO https://platformio.org/
 
* Clone repository: ```git clone -b release --recursive https://github.com/RomanLut/esp32-cam-fpv```

* Open project: esp32-cam-fpv\air_firmware_esp32s3sense\esp32-cam-fpv-esp32s3sense.code-workspace  (or other for ov5640)

* Let PlatformIO install all components

* Connect **esp32s3sense** to USB

* Click **[PlatformIO: Upload]** on bottom toolbar.

# Over the Air update (OTA)

It is also possible to upgrade firwware over wifi.

Hold REC button while powering up to enter OTA mode. Alternatively, hold REC button and press reset button. 

OTA/Fileserver mode is indicated by LED blinking with 2 Hz frequency

* Enter OTA mode.
* Connect to **espvtx** access point
* Navigate to http://192.168.4.1/ota
* Select **firmware.bin** file
* Click **Upload**


