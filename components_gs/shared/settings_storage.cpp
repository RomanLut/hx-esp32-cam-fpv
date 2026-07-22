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
        std::string& temp = (*this)["gs"]["vr_distance"];
        if (!temp.empty())
        {
            s_groundstation_config.screenVrDistance = std::clamp(std::stof(temp), 1.0f, 3.0f);
        }
    }

    {
        std::string& temp = (*this)["gs"]["vr_curved"];
        if (!temp.empty())
        {
            s_groundstation_config.screenVrCurved = std::atoi(temp.c_str()) != 0;
        }
    }

    {
        std::string& temp = (*this)["gs"]["vr_curvature_angle_deg"];
        if (!temp.empty())
        {
            s_groundstation_config.screenVrCurvatureAngleDeg = std::clamp(std::stof(temp), 30.0f, 85.0f);
        }
    }

    {
        std::string& temp = (*this)["gs"]["vr_passthrough_level"];
        if (!temp.empty())
        {
            s_groundstation_config.screenVrPassthroughLevel = static_cast<uint8_t>(std::clamp(std::atoi(temp.c_str()), 0, 7));
        }
    }

    {
        std::string& temp = (*this)["gs"]["vr_tilt_deg"];
        if (!temp.empty())
        {
            s_groundstation_config.screenVrTiltDeg = std::clamp(std::stof(temp), -20.0f, 20.0f);
        }
    }

    {
        s_postprocessingState.jpeg_deblocking_enabled = true;
        std::string& temp = (*this)["gs"]["postprocessing_jpeg_deblocking"];
        if (!temp.empty())
        {
            s_postprocessingState.jpeg_deblocking_enabled = std::atoi(temp.c_str()) != 0;
        }
    }

    {
        s_postprocessingState.adaptive_dithering_level = 2;
        std::string& temp = (*this)["gs"]["postprocessing_adaptive_dithering_level"];
        if (!temp.empty())
        {
            const int stored_level = std::atoi(temp.c_str());
            s_postprocessingState.adaptive_dithering_level = static_cast<uint8_t>(std::clamp(stored_level, 0, 3));
        }
    }
    {
        std::string& temp = (*this)["gs"]["postprocessing_pipeline_mode"];
        if (!temp.empty())
        {
            const int stored_mode = std::atoi(temp.c_str());
            s_postprocessingState.pipeline_mode = stored_mode == 0
                ? PostprocessingState::PipelineMode::RGB565
                : PostprocessingState::PipelineMode::RGB888;
        }
        else
        {
            s_postprocessingState.pipeline_mode = PostprocessingState::PipelineMode::RGB888;
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

    s_groundstation_config.telemetryUart = (*this)["gs"]["telemetry_uart"];
    if (s_groundstation_config.telemetryUart.empty())
    {
        s_groundstation_config.telemetryUart = "auto";
    }

    {
        std::string& temp = (*this)["gs"]["image_stabilization_enabled"];
        if (!temp.empty()) s_imageStabilizationState.enabled = std::atoi(temp.c_str()) != 0;
    }
    {
        std::string& temp = (*this)["gs"]["image_stabilization_debug"];
        if (!temp.empty()) s_imageStabilizationState.debug = std::atoi(temp.c_str()) != 0;
    }

    {
        std::string& temp = (*this)["gs"]["image_stabilization_channel"];
        if (!temp.empty()) s_imageStabilizationState.rc_channel = static_cast<uint8_t>(std::clamp(std::atoi(temp.c_str()), 0, 18));
    }

    {
        std::string& temp = (*this)["gs"]["image_stabilization_roi_divisor"];
        s_imageStabilizationState.roi_divisor = 6.0f;
        if (!temp.empty()) s_imageStabilizationState.roi_divisor = std::clamp(std::stof(temp), 3.0f, 10.0f);
    }
    {
        std::string& temp = (*this)["gs"]["image_stabilization_zoom"];
        if (!temp.empty()) s_imageStabilizationState.zoom = std::clamp(std::stof(temp), 1.0f, 2.0f);
    }
    {
        std::string& temp = (*this)["gs"]["image_stabilization_decay"];
        if (!temp.empty()) s_imageStabilizationState.stabilization_decay = std::clamp(std::stof(temp), 0.01f, 1.0f);
    }
    {
        std::string& temp = (*this)["gs"]["image_stabilization_limit_release_boost"];
        s_imageStabilizationState.limit_release_boost = 0.5f;
        if (!temp.empty()) s_imageStabilizationState.limit_release_boost = std::clamp(std::stof(temp), 0.0f, 10.0f);
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
        std::string& temp = (*this)["gs"]["ov3660_high_fps"];
        if (!temp.empty()) config.camera.ov3660HighFPS = std::atoi(temp.c_str()) != 0;
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
    (*this)["gs"]["vr_distance"] = std::to_string(s_groundstation_config.screenVrDistance);
    (*this)["gs"]["vr_curved"] = std::to_string(s_groundstation_config.screenVrCurved ? 1 : 0);
    (*this)["gs"]["vr_curvature_angle_deg"] = std::to_string(s_groundstation_config.screenVrCurvatureAngleDeg);
    (*this)["gs"]["vr_passthrough_level"] = std::to_string(static_cast<int>(s_groundstation_config.screenVrPassthroughLevel));
    (*this)["gs"]["vr_tilt_deg"] = std::to_string(s_groundstation_config.screenVrTiltDeg);
    (*this)["gs"]["postprocessing_jpeg_deblocking"] = std::to_string(s_postprocessingState.jpeg_deblocking_enabled ? 1 : 0);
    (*this)["gs"].remove("postprocessing_debanding_level");
    (*this)["gs"]["postprocessing_adaptive_dithering_level"] = std::to_string(std::clamp<int>(s_postprocessingState.adaptive_dithering_level, 0, 3));
    (*this)["gs"]["postprocessing_pipeline_mode"] =
        std::to_string(s_postprocessingState.pipeline_mode == PostprocessingState::PipelineMode::RGB565 ? 0 : 1);
    (*this)["gs"].remove("postprocessing_jpeg_deblocking_level");
    (*this)["gs"].remove("postprocessing_adaptive_dithering");
    (*this)["gs"]["tx_power"] = std::to_string((int)s_groundstation_config.txPower);
    (*this)["gs"]["tx_interface"] = s_groundstation_config.txInterface;
    (*this)["gs"]["apfpv_interface"] = s_groundstation_config.apfpvInterface;
    (*this)["gs"]["telemetry_uart"] = s_groundstation_config.telemetryUart;
    (*this)["gs"]["transport_kind"] = std::to_string(gs::core::transportKindToInt(s_groundstation_config.transportKind));
    (*this)["gs"]["apfpv_camera_id"] = std::to_string(s_groundstation_config.apfpvPreferredCameraId);
    (*this)["gs"]["gpio_keys_layout"] = std::to_string((int)s_groundstation_config.GPIOKeysLayout);
    (*this)["gs"]["gs_device_id"] = std::to_string(s_groundstation_config.deviceId);
    (*this)["gs"]["image_stabilization_enabled"] = std::to_string(s_imageStabilizationState.enabled ? 1 : 0);
    (*this)["gs"]["image_stabilization_debug"] = std::to_string(s_imageStabilizationState.debug ? 1 : 0);
    (*this)["gs"]["image_stabilization_channel"] = std::to_string(s_imageStabilizationState.rc_channel);
    (*this)["gs"]["image_stabilization_roi_divisor"] = std::to_string(s_imageStabilizationState.roi_divisor);
    (*this)["gs"]["image_stabilization_zoom"] = std::to_string(s_imageStabilizationState.zoom);
    (*this)["gs"]["image_stabilization_decay"] = std::to_string(s_imageStabilizationState.stabilization_decay);
    (*this)["gs"]["image_stabilization_limit_release_boost"] = std::to_string(s_imageStabilizationState.limit_release_boost);
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
    (*this)["gs"]["ov3660_high_fps"] = std::to_string((int)config.camera.ov3660HighFPS ? 1 : 0);
    (*this)["gs"]["ov5640_high_fps"] = std::to_string((int)config.camera.ov5640HighFPS ? 1 : 0);
    save();
}
