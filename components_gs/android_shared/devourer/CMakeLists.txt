cmake_minimum_required(VERSION 3.15)

# Set the toolchain file for vcpkg on Windows.
if(WIN32)
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
endif()

project(Rtl8812auNet)

# Find pkg-config and then use it to locate libusb.
find_package(PkgConfig REQUIRED)
pkg_check_modules(libusb REQUIRED IMPORTED_TARGET libusb-1.0)

add_library(WiFiDriver
    src/logger.h
    src/Event.h

    hal/basic_types.h
    hal/hal_com_reg.h
    src/ieee80211_radiotap.h
    src/ParsedRadioPacket.cpp
    src/Radiotap.c
    src/RadiotapBuilder.cpp
    src/RadiotapBuilder.h
    src/RtlTransport.h
    src/RtlAdapter.cpp
    src/RtlAdapter.h
    src/RtlUsbAdapter.h
    src/UsbTransport.cpp
    src/UsbTransport.h
    src/UsbDeviceLock.cpp
    src/UsbDeviceLock.h
    src/UsbOpen.cpp
    src/UsbOpen.h
    src/SelectedChannel.h
    src/RateDefinitions.h
    src/RxPacket.h
    src/TxDescBits.h
    src/BeamformingSounder.h
    src/TxMode.cpp
    src/TxMode.h
    src/TxPower.cpp
    src/TxPower.h
    src/TxStats.h
    src/ThermalStatus.h
    src/TxCaps.h
    src/LinkHealth.cpp
    src/LinkHealth.h
    src/RxQuality.h
    src/IRtlDevice.h
    src/SignalStop.cpp
    src/SignalStop.h
    src/WiFiDriver.cpp
    src/WiFiDriver.h
    src/registry_priv.h
    src/PhyTableLoader.cpp
    src/PhyTableLoader.h

    # --- Jaguar1: RTL8812AU / 8811AU / 8821AU / 8814AU ---
    hal/hal8812a_fw.c hal/hal8812a_fw.h
    hal/hal8821a_fw.c hal/hal8821a_fw.h
    hal/Hal8812PwrSeq.c hal/Hal8812PwrSeq.h
    hal/Hal8821APwrSeq.c hal/Hal8821APwrSeq.h
    hal/Hal8812a_TxPwrTrack.cpp hal/Hal8812a_TxPwrTrack.h
    hal/Hal8812PhyReg.h hal/Hal8821PhyReg.h
    hal/rtl8812a_hal.h hal/rtl8812a_recv.h hal/rtl8812a_spec.h
    src/Firmware.h
    src/jaguar1/RtlJaguarDevice.cpp src/jaguar1/RtlJaguarDevice.h
    src/jaguar1/HalModule.cpp src/jaguar1/HalModule.h
    src/jaguar1/RadioManagementModule.cpp src/jaguar1/RadioManagementModule.h
    src/jaguar1/EepromManager.cpp src/jaguar1/EepromManager.h
    src/jaguar1/FirmwareManager.cpp src/jaguar1/FirmwareManager.h
    src/jaguar1/Iqk8812a.cpp src/jaguar1/Iqk8812a.h
    src/jaguar1/PhydmWatchdog.cpp src/jaguar1/PhydmWatchdog.h
    src/jaguar1/PowerTracking8812a.cpp src/jaguar1/PowerTracking8812a.h
    src/jaguar1/FrameParser.cpp src/jaguar1/FrameParser.h
    src/jaguar1/BbDbgportReader.cpp src/jaguar1/BbDbgportReader.h

    # RTL8814AU (4T4R; requires Jaguar1 core above)
    hal/hal8814a_fw.c hal/hal8814a_fw.h
    hal/Hal8814PwrSeq.c hal/Hal8814PwrSeq.h
    hal/Hal8814PhyReg.h
    hal/phydm/rtl8814a/Hal8814_PhyTables.c
    hal/phydm/rtl8814a/Hal8814_PhyTables.h
    src/jaguar1/Iqk8814a.cpp src/jaguar1/Iqk8814a.h

    # --- Jaguar2: RTL8822BU / RTL8811CU / RTL8821CU ---
    src/jaguar2/RtlJaguar2Device.cpp src/jaguar2/RtlJaguar2Device.h
    src/jaguar2/HalJaguar2.cpp src/jaguar2/HalJaguar2.h
    src/jaguar2/HalmacJaguar2Fw.cpp src/jaguar2/HalmacJaguar2Fw.h
    src/jaguar2/HalmacJaguar2MacInit.cpp src/jaguar2/HalmacJaguar2MacInit.h
    src/jaguar2/HalmacJaguar2Regs.h
    src/jaguar2/FrameParserJaguar2.h
    src/jaguar2/ChipVariant.h
    src/jaguar2/Jaguar2PhyTables.h
    src/jaguar2/Jaguar2Calibration.h
    src/jaguar2/Phy8822bTables.cpp
    src/jaguar2/Halrf8822b.cpp src/jaguar2/Halrf8822b.h
    hal/hal8822b_fw.c hal/hal8822b_fw.h
    hal/phydm/rtl8822b/Hal8822b_PhyTables.c hal/phydm/rtl8822b/Hal8822b_PhyTables.h
    src/jaguar2/Phy8821cTables.cpp
    src/jaguar2/Halrf8821c.cpp src/jaguar2/Halrf8821c.h
    hal/hal8821c_fw.c hal/hal8821c_fw.h
    hal/phydm/rtl8821c/Hal8821c_PhyTables.c hal/phydm/rtl8821c/Hal8821c_PhyTables.h

    # --- Jaguar3: RTL8812CU / RTL8822CU / RTL8812EU / RTL8822EU ---
    src/jaguar3/Jaguar3PhyTables.h
    src/jaguar3/Jaguar3Calibration.h
    src/jaguar3/ChipVariant.h
    src/jaguar3/RtlJaguar3Device.cpp src/jaguar3/RtlJaguar3Device.h
    src/jaguar3/HalJaguar3.cpp src/jaguar3/HalJaguar3.h
    src/jaguar3/HalmacJaguar3Fw.cpp src/jaguar3/HalmacJaguar3Fw.h
    src/jaguar3/HalmacJaguar3MacInit.cpp src/jaguar3/HalmacJaguar3MacInit.h
    src/jaguar3/HalmacJaguar3Regs.h
    src/jaguar3/RadioManagementJaguar3.cpp src/jaguar3/RadioManagementJaguar3.h
    src/jaguar3/FrameParserJaguar3.h
    src/jaguar3/PhyTableLoaderJaguar3.cpp src/jaguar3/PhyTableLoaderJaguar3.h
    src/jaguar3/Phy8822cTables.cpp
    src/jaguar3/Halrf8822c.cpp src/jaguar3/Halrf8822c.h
    hal/hal8822c_fw.c hal/hal8822c_fw.h
    hal/phydm/rtl8822c/Hal8822c_PhyTables.c hal/phydm/rtl8822c/Hal8822c_PhyTables.h
    hal/phydm/rtl8822c/Hal8822c_IqkNctl.c hal/phydm/rtl8822c/Hal8822c_IqkNctl.h
    src/jaguar3/Phy8822eTables.cpp
    src/jaguar3/Halrf8822e.cpp src/jaguar3/Halrf8822e.h
    hal/hal8822e_fw.c hal/hal8822e_fw.h
    hal/phydm/rtl8822e/Hal8822e_PhyTables.c hal/phydm/rtl8822e/Hal8822e_PhyTables.h
)

target_compile_features(WiFiDriver PUBLIC cxx_std_20)

target_compile_definitions(WiFiDriver PUBLIC
    DEVOURER_HAVE_JAGUAR1=1
    DEVOURER_HAVE_8814=1
    DEVOURER_HAVE_JAGUAR2=1
    DEVOURER_HAVE_JAGUAR2_8822B=1
    DEVOURER_HAVE_JAGUAR2_8821C=1
    DEVOURER_HAVE_JAGUAR3=1
    DEVOURER_HAVE_JAGUAR3_8822C=1
    DEVOURER_HAVE_JAGUAR3_8822E=1
)

# Link WiFiDriver with libusb as found via pkg-config.
target_link_libraries(WiFiDriver PUBLIC PkgConfig::libusb)

target_include_directories(WiFiDriver PUBLIC hal)
target_include_directories(WiFiDriver PUBLIC hal/phydm/rtl8814a)
target_include_directories(WiFiDriver PUBLIC hal/phydm/rtl8821c)
target_include_directories(WiFiDriver PUBLIC hal/phydm/rtl8822b)
target_include_directories(WiFiDriver PUBLIC hal/phydm/rtl8822c)
target_include_directories(WiFiDriver PUBLIC hal/phydm/rtl8822e)
target_include_directories(WiFiDriver PUBLIC src)
