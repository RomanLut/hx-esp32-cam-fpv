
# Building esp32c5 air unit

Unfortunately, there are currently no **esp32c5** boards on the market with a good form factor and a proper camera connector. Using an **esp32c5** as an air unit requires advanced hardware skills and custom assembly. For this reason, the **esp32s3sense** remains the best choice for an air unit for now.

## esp32c5 proof-of-concept air unit

Air unit can be built using **esp32-c5-devkit-c1-N8R4** board, **BY-OV5640** camera adapter and microSD card adapter.

![alt text](/doc/images/esp32-c5-devkit-c1-prototype_sch.png "esp32c5_air_unit")

Camera and SD card wires should be as short as possible (2-3cm).

**esp32-c5-devkit-c1** board should have at least 4Mb RAM (N8R**4** or more).

![alt text](/doc/images/esp32-c5-prototype.jpg "esp32c5_air_unit")

## esp32c5 DIY air unit

A better air unit can be made using **esp32-c5-wroom-n16-r8** module,  **BY-OV5640** camera adapter and microSD card adapter.
PCB is not designed yet. Module, linear regulator, resistors and capacitors are soldered to piece of copper-clad laminate.

![alt text](/doc/images/esp32c5_air_unit.jpg "esp32c5_air_unit")

![alt text](/doc/images/esp32c5-prototype-3p.jpg "esp32c5_air_unit")

(on the photo above pcb is marked BY-OV2640 but parts (oscillator, linear regulator) are soldered for BY-OV5640)

![alt text](/doc/images/esp32c5-sch.png "esp32c5_air_unit")

[Schematics PDF](/doc/datasheets/esp32c5-prototype-eda.pdf)

[Project EasyEDA](/doc/EasyEDA/hx_esp32c5_cam_proto.eprj)



