
# Development

  Radxa Zero 3W or Raspberry Pi 4 is recommented for development.
 
  Building GS development image for Raspberry PI: [/doc/building_gs_image_rpi.md](/doc/building_gs_sd_rpi.md#building-ground-station-development-image-for-raspberry-pi)

 
# Development UI

Development UI is enabled with **d** key or **middle mouse click**.

![alt text](images/debug_menu.jpg "debugmenu")

# Statistics

 Statistic can be enabled in development menu or in OSD menu.

![alt text](images/stats.jpg "stats.jpg")

Graph               | Description
------------------- | -------------
Frames              | Big rectangle - frame received, small rectangle - frame is restored using FEC, emtpy space - frame is lost
Parts               | Number of frame blocks processed to decode the frame
Period              | Distance from previous decoded frame in ms
Quality             | Frame compression setting
Datasize            | Actual MJPEG stream bandwidth (without FEC encoding)
Wifi load           | Air unit wifi queue usage in %


# Profiling

![alt text](images/vcd_profiling.png "vcd_profiling.png")

 Air unit code contains profiler which can write frame timing in VCD format to SD card.

 VCD file can than be viewed in tools like [Wavetrace](https://www.wavetrace.io/) (available as VSCode extension),  [VCDrom](https://vc.drom.io/), https://github.com/raczben/fliplot.

 See corresponding [defines.](https://github.com/RomanLut/hx-esp32-cam-fpv/blob/master/components/common/vcd_profiler.h#L5)

 Profling is started from development UI with **[Profile]** buttons for 500ms or for 3 seconds.
  

Data source         | Description
------------------- | -------------
pf.cam_data         | Activity of camera_data_available() callback
pf.quality          | The quaity setting of frame been captured. This data source Visually defines range of a frame on graph.
pf.data_size        | Size of frame JPEG data been received in Kb
pf.fec_pool         | Number of free blocks in FEC encoder pool
pf.fec              | Activity of FEC encoder thread
pf.wifi_tx          | Activity of wifi transmitter thread, including transmission completion waiting
pf.wifi_queue       | Size of wifi transmittion queue in Kb
pf.fec_spin         | FEC encoder thread is spinning because wifi tx thread is overloaded
pf.wifi_ovf         | Toggled every time Wifi tx thread overlows
pf.wifi_spin        | Wifi stack out of memory 
pf.fec_ovf          | Toggled every time FEC input queue overflow
pf.cam_ovf          | Toggled every time camera interfacing error (frame start missing, VSYNC interrupt missing etc.)
pf.sd_fast_buf      | Size of SD card RAM queue in Kb
pf.sd_slow_buf      | Size of SD card PSRAM queue in %
ps.sd_ovf           | Toggled every time any SD card queue overflows


# Considerations

 GS application is based on SDL2. Application uses OpenGL ES API. In environment without desktop, KMSDRM should provide hardware OpenGL ES acceleration.
