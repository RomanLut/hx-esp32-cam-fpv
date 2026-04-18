#include "settings_storage.h"

#include "core/osd_menu_common.h"
#include "core/transport_kind.h"
#include "gs_runtime_core.h"
#include "gs_shared_state.h"
#include "wifi_channels.h"

SettingsStorage::SettingsStorage(const std::string& filename)
    : m_ini_file(filename),
      m_path(filename)
{
}

bool SettingsStorage::save(bool pretty)
{
    return m_ini_file.write(*this, pretty);
}

bool SettingsStorage::read()
{
    return m_ini_file.read(*this);
}

void SettingsStorage::setPath(const std::string& filename)
{
    m_path = filename;
    m_ini_file = mINI::INIFile(filename);
}

//===================================================================================
//===================================================================================
// Returns the active settings file path used by the shared runtime.
const std::string& SettingsStorage::path() const
{
    return m_path;
}

void SettingsStorage::loadGroundStationConfig()
{
    {
        std::string& temp = (*this)["gs"]["gs_device_id"];
        const int device_id = std::atoi(temp.c_str());
        if (device_id > 0)
        {
            s_groundstation_config.deviceId = static_cast<uint16_t>(device_id);
        }
    }

    {
        std::string& temp = (*this)["gs"]["wifi_channel"];
        const int channel = std::atoi(temp.c_str());
        if ((channel >= MIN_WIFI_CHANNEL) && (channel <= MAX_WIFI_CHANNEL))
        {
            s_groundstation_config.wifi_channel = channel;
        }
        else
        {
            s_groundstation_config.wifi_channel = DEFAULT_WIFI_CHANNEL_2_4GHZ;
        }
    }

    {
        std::string& temp = (*this)["gs"]["wifi_band"];
        const int band = std::atoi(temp.c_str());
        if ((band >= GS_WIFI_BAND_2_4_GHZ) && (band <= GS_WIFI_BAND_DUAL))
        {
            s_groundstation_config.wifiBand = static_cast<uint8_t>(band);
        }
        else
        {
            s_groundstation_config.wifiBand = DEFAULT_GS_WIFI_BAND;
        }
    }

    {
        const uint8_t default_channel = s_groundstation_config.wifiBand == GS_WIFI_BAND_5_8_GHZ
            ? DEFAULT_WIFI_CHANNEL_5_8_GHZ
            : DEFAULT_WIFI_CHANNEL_2_4GHZ;

        if (!isWifiChannelAllowedByBand(s_groundstation_config.wifi_channel, s_groundstation_config.wifiBand))
        {
            s_groundstation_config.wifi_channel = default_channel;
        }
    }

    {
        std::string& temp = (*this)["gs"]["tx_power"];
        const int tx_power = std::atoi(temp.c_str());
        if ((tx_power >= gs::menu::kMinTxPower) && (tx_power <= gs::menu::kMaxTxPower))
        {
            s_groundstation_config.txPower = tx_power;
        }
        else
        {
            s_groundstation_config.txPower = gs::menu::kDefaultTxPower;
        }
    }

    {
        std::string& temp = (*this)["gs"]["gpio_keys_layout"];
        const int layout = std::atoi(temp.c_str());
        if ((layout == 0) || (layout == 1))
        {
            s_groundstation_config.GPIOKeysLayout = static_cast<uint8_t>(layout);
        }
        else
        {
            s_groundstation_config.GPIOKeysLayout = 0;
        }
    }

    {
        std::string& temp = (*this)["gs"]["screen_aspect_ratio"];
        if (!temp.empty())
        {
            s_groundstation_config.screenAspectRatio =
                static_cast<ScreenAspectRatio>(std::clamp(std::atoi(temp.c_str()), 0, 5));
        }
    }

    {
        std::string& temp = (*this)["gs"]["vsync"];
        if (!temp.empty())
        {
            s_groundstation_config.vsync = std::atoi(temp.c_str()) != 0;
        }
    }

    {
        std::string& temp = (*this)["gs"]["vr_mode"];
        if (!temp.empty())
        {
            s_groundstation_config.vrMode = std::atoi(temp.c_str()) != 0;
        }
    }

    {
        std::string& temp = (*this)["gs"]["screen_flip_v"];
        if (!temp.empty())
        {
            s_groundstation_config.screenFlipV = std::atoi(temp.c_str()) != 0;
        }
    }

    {
        std::string& temp = (*this)["gs"]["screen_zoom"];
        if (!temp.empty())
        {
            s_groundstation_config.screenZoom = std::clamp(std::stof(temp), 0.5f, 1.5f);
        }
    }

    {
        std::string& temp = (*this)["gs"]["vr_separation"];
        if (!temp.empty())
        {
            s_groundstation_config.screenVrSeparation = std::clamp(std::stof(temp), -0.30f, 0.30f);
        }
    }

    {
        std::string& temp = (*this)["gs"]["transport_kind"];
        if (!temp.empty())
        {
            s_groundstation_config.transportKind =
                gs::core::transportKindFromInt(std::atoi(temp.c_str()), gs::core::TransportKind::RawBroadcast);
        }
    }

    {
        std::string& temp = (*this)["gs"]["apfpv_camera_id"];
        if (!temp.empty())
        {
            s_groundstation_config.apfpvPreferredCameraId =
                static_cast<uint16_t>(std::clamp(std::atoi(temp.c_str()), 0, 0xFFFF));
        }
    }

    s_groundstation_config.txInterface = (*this)["gs"]["tx_interface"];
    if (s_groundstation_config.txInterface.empty())
    {
        s_groundstation_config.txInterface = "auto";
    }

    s_groundstation_config.apfpvInterface = (*this)["gs"]["apfpv_interface"];
    if (s_groundstation_config.apfpvInterface.empty())
    {
        s_groundstation_config.apfpvInterface = "auto";
    }
}

void SettingsStorage::loadGround2AirConfig()
{
    Ground2Air_Config_Packet config = s_runtimeCore.session.copyConfigPacket();
    config.dataChannel.wifi_channel = s_groundstation_config.wifi_channel;

    {
        std::string& temp = (*this)["gs"]["brightness"];
        if (!temp.empty()) config.camera.brightness = std::clamp(std::atoi(temp.c_str()), -2, 2);
    }

    {
        std::string& temp = (*this)["gs"]["contrast"];
        if (!temp.empty()) config.camera.contrast = std::clamp(std::atoi(temp.c_str()), -2, 2);
    }

    {
        std::string& temp = (*this)["gs"]["saturation"];
        if (!temp.empty()) config.camera.saturation = std::clamp(std::atoi(temp.c_str()), -2, 2);
    }

    {
        std::string& temp = (*this)["gs"]["ae_level"];
        if (!temp.empty()) config.camera.ae_level = std::clamp(std::atoi(temp.c_str()), -2, 2);
    }

    {
        std::string& temp = (*this)["gs"]["sharpness"];
        if (!temp.empty()) config.camera.sharpness = std::clamp(std::atoi(temp.c_str()), -3, 3);
    }

    {
        std::string& temp = (*this)["gs"]["vflip"];
        if (!temp.empty()) config.camera.vflip = std::atoi(temp.c_str()) != 0;
    }

    {
        std::string& temp = (*this)["gs"]["resolution"];
        if (!temp.empty()) config.camera.resolution = static_cast<Resolution>(
            std::clamp(std::atoi(temp.c_str()), (int)Resolution::VGA, (int)Resolution::HD));
    }

    {
        std::string& temp = (*this)["gs"]["wifi_rate"];
        if (!temp.empty()) config.dataChannel.wifi_rate = static_cast<WIFI_Rate>(
            std::clamp(std::atoi(temp.c_str()), (int)WIFI_Rate::RATE_G_12M_ODFM, (int)WIFI_Rate::RATE_N_72M_MCS7_S));
    }

    {
        std::string& temp = (*this)["gs"]["fec_n"];
        if (!temp.empty()) config.dataChannel.fec_codec_n = static_cast<uint8_t>(
            std::clamp(std::atoi(temp.c_str()), FEC_K + 1, FEC_N));
    }

    {
        std::string& temp = (*this)["gs"]["ov2640_high_fps"];
        if (!temp.empty()) config.camera.ov2640HighFPS = std::atoi(temp.c_str()) != 0;
    }

    {
        std::string& temp = (*this)["gs"]["ov5640_high_fps"];
        if (!temp.empty()) config.camera.ov5640HighFPS = std::atoi(temp.c_str()) != 0;
    }

    s_runtimeCore.session.setConfigPacket(config);
}

void SettingsStorage::saveGroundStationConfig()
{
    (*this)["gs"]["wifi_channel"] = std::to_string(s_groundstation_config.wifi_channel);
    (*this)["gs"]["wifi_band"] = std::to_string((int)s_groundstation_config.wifiBand);
    (*this)["gs"]["screen_aspect_ratio"] = std::to_string((int)s_groundstation_config.screenAspectRatio);
    (*this)["gs"]["vr_mode"] = std::to_string(s_groundstation_config.vrMode ? 1 : 0);
    (*this)["gs"]["vsync"] = std::to_string(s_groundstation_config.vsync ? 1 : 0);
    (*this)["gs"]["screen_flip_v"] = std::to_string(s_groundstation_config.screenFlipV ? 1 : 0);
    (*this)["gs"]["screen_zoom"] = std::to_string(s_groundstation_config.screenZoom);
    (*this)["gs"]["vr_separation"] = std::to_string(s_groundstation_config.screenVrSeparation);
    (*this)["gs"]["tx_power"] = std::to_string((int)s_groundstation_config.txPower);
    (*this)["gs"]["tx_interface"] = s_groundstation_config.txInterface;
    (*this)["gs"]["apfpv_interface"] = s_groundstation_config.apfpvInterface;
    (*this)["gs"]["transport_kind"] = std::to_string(gs::core::transportKindToInt(s_groundstation_config.transportKind));
    (*this)["gs"]["apfpv_camera_id"] = std::to_string(s_groundstation_config.apfpvPreferredCameraId);
    (*this)["gs"]["gpio_keys_layout"] = std::to_string((int)s_groundstation_config.GPIOKeysLayout);
    (*this)["gs"]["gs_device_id"] = std::to_string(s_groundstation_config.deviceId);
    save();
}

void SettingsStorage::saveGround2AirConfig()
{
    const Ground2Air_Config_Packet config = s_runtimeCore.session.copyConfigPacket();
    (*this)["gs"]["brightness"] = std::to_string(config.camera.brightness);
    (*this)["gs"]["contrast"] = std::to_string(config.camera.contrast);
    (*this)["gs"]["saturation"] = std::to_string(config.camera.saturation);
    (*this)["gs"]["ae_level"] = std::to_string(config.camera.ae_level);
    (*this)["gs"]["sharpness"] = std::to_string(config.camera.sharpness);
    (*this)["gs"]["vflip"] = std::to_string(config.camera.vflip ? 1 : 0);
    (*this)["gs"]["resolution"] = std::to_string((int)config.camera.resolution);
    (*this)["gs"]["wifi_rate"] = std::to_string((int)config.dataChannel.wifi_rate);
    (*this)["gs"]["fec_n"] = std::to_string((int)config.dataChannel.fec_codec_n);
    (*this)["gs"]["ov2640_high_fps"] = std::to_string((int)config.camera.ov2640HighFPS ? 1 : 0);
    (*this)["gs"]["ov5640_high_fps"] = std::to_string((int)config.camera.ov5640HighFPS ? 1 : 0);
    save();
}
