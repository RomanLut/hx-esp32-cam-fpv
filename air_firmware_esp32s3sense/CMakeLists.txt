cmake_minimum_required(VERSION 3.5)

set(EXTRA_COMPONENT_DIRS "../components" "../components2")
set(COMPONENT_REQUIRES "common")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(air_firmware)

# GCC 13.2 can crash in the IRA pass when the ESP-IDF 5.3.1 LCD component is
# built with -Og on Windows. The air firmware does not use this component.
idf_component_get_property(esp_lcd_component esp_lcd COMPONENT_LIB)
target_compile_options(${esp_lcd_component} PRIVATE -O0)
