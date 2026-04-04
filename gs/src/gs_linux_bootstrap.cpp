#include "gs_linux_bootstrap.h"

#include <cstdlib>
#include <random>
#include <string>

#include "avi.h"
#include "Comms.h"
#include "PI_HAL.h"
#include "crc.h"
#include "gpio_buttons.h"
#include "gs_recordings_storage.h"
#include "gs_linux_runtime.h"
#include "main.h"
#include "gs_linux_runtime_loop.h"
#include "gs_linux_startup.h"
#include "gs_runtime_config.h"
#include "gs_runtime_core.h"
#include "gs_runtime_state.h"
#include "gs_udp_broadcast.h"
#include "utils/utils.h"

namespace
{

int printLinuxHelp()
{
    printf("gs -option val -option val\n");
    printf("-rx <rx_interface1> <rx_interface2>, f.e. wlan1 or wlan1 wlan2 default: auto\n");
    printf("-tx <tx_interface>, f.e. wlan1 default: first rx interface\n");
    printf("-p <gd_ip>, default: disabled\n");
    printf("-ch <wifi_channel>, default: 7\n");
    printf("-w <width>, default: 1280\n");
    printf("-h <width>, default: 720\n");
    printf("-fullscreen <1/0>, default: 1\n");
    printf("-vsync <1/0>, default: 1\n");
    printf("-sm <1/0>, skip setting monitor mode with pcap, default: 1\n");
    printf("-serial <serial_port>, serial port for telemetry, default:");
    printf(isRadxaZero3() ? "/dev/ttyUSB0(if exists), /dev/ttyS3" : "/dev/ttyUSB0(if exists), /dev/serial0");
    printf("\n");
    printf("-help\n");
    return 0;
}

void parseLinuxArgs(int argc,
                      const char* argv[],
                      gs::core::RXDescriptor& rx_descriptor,
                      gs::core::TXDescriptor& tx_descriptor,
                      Ground2Air_Config_Packet& config)
{
    for (int i = 1; i < argc; ++i)
    {
        auto temp = std::string(argv[i]);
        auto next = i != argc - 1 ? std::string(argv[i + 1]) : std::string("");
        auto check_argval = [&next](std::string arg_name)
        {
            if (next.empty())
            {
                std::string er = std::string("Please provide correct argument for -") + arg_name;
                LOGE(er);
                throw er;
            }
        };

        auto check_argval_int = [&next](std::string arg_name)
        {
            if (next.empty())
            {
                std::string er = std::string("Please provide correct argument for -") + arg_name;
                LOGE(er);
                throw er;
            }
            try
            {
                std::stoi(next);
            }
            catch (std::invalid_argument&)
            {
                std::string er = std::string("Please provide correct argument for -") + arg_name;
                LOGE(er);
                throw er;
            }
        };

        if (temp == "-tx")
        {
            check_argval("tx");
            tx_descriptor.interface = next;
            i++;
        }
        else if (temp == "-serial")
        {
            check_argval("serial");
            serialPortName = next;
            i++;
        }
        else if (temp == "-p")
        {
            check_argval_int("port");
            g_gsUDPBroadcast->init(std::string("127.0.0.1"), std::stoi(next));
            i++;
        }
        else if (temp == "-rx")
        {
            rx_descriptor.interfaces.clear();
        }
        else if (temp == "-ch")
        {
            check_argval_int("ch");
            s_groundstation_config.wifi_channel = std::stoi(next);
            config.dataChannel.wifi_channel = s_groundstation_config.wifi_channel;
            i++;
        }
        else if (temp == "-w")
        {
            check_argval_int("w");
            s_hal->set_width(std::stoi(next));
            i++;
        }
        else if (temp == "-h")
        {
            check_argval_int("h");
            s_hal->set_height(std::stoi(next));
            i++;
        }
        else if (temp == "-fullscreen")
        {
            check_argval_int("fullscreen");
            s_hal->set_fullscreen(std::stoi(next) > 0);
            i++;
        }
        else if (temp == "-vsync")
        {
            check_argval_int("vsync");
            s_groundstation_config.vsync = std::stoi(next) > 0;
            i++;
        }
        else if (temp == "-sm")
        {
            check_argval_int("sm");
            rx_descriptor.skip_mon_mode_cfg = std::stoi(next) > 0;
            i++;
        }
        else if (temp == "-help")
        {
            throw printLinuxHelp();
        }
        else
        {
            rx_descriptor.interfaces.push_back(temp);
        }
    }
}

int initializeLinuxConfig(gs::core::RXDescriptor& rx_descriptor,
                            gs::core::TXDescriptor& tx_descriptor,
                            Ground2Air_Config_Packet& config)
{
    if ((rx_descriptor.interfaces.size() == 1) && (rx_descriptor.interfaces[0] == "auto"))
    {
        if (!findLinuxRXInterfaces(rx_descriptor))
        {
            printf("Unable to find RX interfaces\n");
            return 0;
        }
    }

    s_groundstation_config.txInterface = s_settingsStorage["gs"]["tx_interface"];
    if (s_groundstation_config.txInterface.empty())
    {
        s_groundstation_config.txInterface = "auto";
    }

    if ((tx_descriptor.interface == "auto") && !rx_descriptor.interfaces.empty())
    {
        bool found = false;
        for (const std::string& rx_interface : rx_descriptor.interfaces)
        {
            if (rx_interface == s_groundstation_config.txInterface)
            {
                tx_descriptor.interface = s_groundstation_config.txInterface;
                found = true;
                break;
            }
        }
        if (!found)
        {
            tx_descriptor.interface = rx_descriptor.interfaces[0];
        }
        printf("Using TX interface %s\n", tx_descriptor.interface.c_str());
    }

    s_runtimeCore.session.setConfigPacket(config);

    rx_descriptor.coding_k = config.dataChannel.fec_codec_k;
    rx_descriptor.coding_n = FEC_N;
    rx_descriptor.mtu = AIR2GROUND_MAX_MTU;

    tx_descriptor.coding_k = 2;
    tx_descriptor.coding_n = 3;
    tx_descriptor.mtu = GROUND2AIR_MAX_MTU;

    g_serialTelemetry->init(serialPortName);

#ifndef WRITE_RAW_MJPEG_STREAM
    prepAviBuffers();
#endif

    s_recordingsStorage->refreshGroundStorageStatus();
    s_hal->set_vsync(s_groundstation_config.vsync, false);

    if (!s_hal->init())
    {
        return -1;
    }

    if (!s_transport.init(rx_descriptor, tx_descriptor))
    {
        return -1;
    }

    performAirUnpair(s_groundstation_config.deviceId, s_transport);
    s_isDual = rx_descriptor.interfaces.size() > 1;
    s_transport.setChannel(s_groundstation_config.wifi_channel);
    s_transport.setTxPower(s_groundstation_config.txPower);

    return 1;
}

} // namespace

int runLinuxBootstrap(int argc, const char* argv[])
{
    if (!ensureLinuxSingleInstance())
    {
        return EXIT_FAILURE;
    }

    std::atexit(cleanupLinuxSingleInstancePidFile);
    init_crc8_table();
    std::srand(static_cast<unsigned int>(std::time(0)));

    gs::core::RXDescriptor rx_descriptor;
    rx_descriptor.interfaces = {"auto"};

    gs::core::TXDescriptor tx_descriptor;
    tx_descriptor.interface = "auto";

    s_hal.reset(new PI_HAL());
    memset(&s_last_airStats, 0, sizeof(AirStats));

    loadSharedSettings(0);
    if (s_groundstation_config.deviceId == 0)
    {
        s_groundstation_config.deviceId = generateLinuxDeviceId();
        s_settingsStorage.saveGroundStationConfig();
    }
    printf("gs_device_id: 0x%04x\n", s_groundstation_config.deviceId);

    s_settingsStorage.loadGround2AirConfig();
    Ground2Air_Config_Packet config = s_runtimeCore.session.copyConfigPacket();

    try
    {
        parseLinuxArgs(argc, argv, rx_descriptor, tx_descriptor, config);
    }
    catch (int code)
    {
        return code;
    }

    const int init_result = initializeLinuxConfig(rx_descriptor, tx_descriptor, config);
    if (init_result <= 0)
    {
        return init_result;
    }

    gpio_buttons_start();
    const int result = runLinuxRuntimeLoop((char**)argv);
    gpio_buttons_stop();
    s_hal->shutdown();
    return result;
}
