# Web Interface

The **web interface** is available in **OTA mode**.

## Entering OTA Mode

### ESP32-CAM

To enter OTA mode on **ESP32-CAM**:

1. Hold the **Boot** button  
2. Power on the ESP32-CAM while holding the button

### ESP32-S3

To enter OTA mode on **ESP32-S3**:

1. Power on the ESP32-S3  
2. Wait **3 seconds**  
3. Press and hold the **Boot** button  
4. Keep holding until the **status LED starts blinking** (~5 seconds)  

> **Note:**  If you press **Boot** too early (immediately after power-up), the ESP32-S3 will enter **bootloader mode**, not OTA mode.


# Web Interface Usage

Connect to the **esp32vtx** access point (no password).  
Then open:

**http://192.168.4.1**

The web interface contains three tabs:

- **Recordings**
- **Settings**
- **Update**

The **Recordings** tab allows you to **Download**, **Play**, and **Delete** recordings.

![web1](images/web1.png)

![web2](images/web2.jpg)

## Settings 

![web3](images/web3.png)

## Updating firmware

Open your browser and go to: **http://192.168.4.1/ota**. Upload **firmware.bin**

![web4](images/web4.png)

## Notes

- Avoid **downloading files concurrently** — ESP32 has limitations on simultaneous connections.
- **Seeking** may not work on **Apple devices**.
