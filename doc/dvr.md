# DVR

Class 10 SD Card is required for the air unit. Maximum supported size is 32GB. Should be formatted to FAT32. The quality of recording is the same on air and ground; no recompression is done (obviously, GS recording does not contain lost frames).

**ESP32** can work with SD card in 4bit and 1bit mode. 1bit mode is chosen to free few pins. 4bit mode seems to provide little benefit (write speed is only 30% faster in 4 bit mode).

**esp32c5** does not have sdmmc hardware. SD Card is mounted in SPI mode which is slow. Currently , every 4 frame is saved on **esp32c5** air unit only.
