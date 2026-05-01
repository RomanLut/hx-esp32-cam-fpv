#include "dct_calibration.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#if defined(GS_ENABLE_DCT_CALIBRATION)

#include <fstream>
#include <cstdlib>
#include <iterator>
#include <map>
#include <mutex>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

#include "Log.h"

namespace
{

constexpr uint8_t kFirstQuality = 8;
constexpr uint8_t kLastQuality = 63;
constexpr uint8_t kMatchedFramesPerQuality = 3;
constexpr const char* kCalibrationPath = "../assets_gs/ov5640.dct.json";

struct DctTable
{
    uint8_t id = 0;
    uint8_t precision = 0;
    std::array<uint16_t, 64> values = {};
};

std::mutex s_calibration_mutex;
std::map<uint8_t, std::vector<DctTable>> s_observed_tables;
std::map<uint8_t, float> s_loaded_severity;
uint8_t s_forced_quality = kFirstQuality;
uint8_t s_matching_quality_frame_count = 0;
bool s_sweep_complete = false;
bool s_loaded_calibration = false;
bool s_logged_disabled_warning = false;
uint8_t s_logged_forced_quality = 0;
uint8_t s_last_reported_quality_log = 0;
uint8_t s_last_no_dqt_quality_log = 0;

//===================================================================================
//===================================================================================
// Returns true for JPEG markers that do not carry a length-prefixed payload.
bool isStandaloneMarker(uint8_t marker)
{
    return marker == 0x01 ||
           (marker >= 0xD0 && marker <= 0xD9);
}

//===================================================================================
//===================================================================================
// Parses JPEG DQT segments into quantization tables for the calibration JSON file.
std::vector<DctTable> parseDctTables(const uint8_t* data, size_t size)
{
    std::vector<DctTable> tables;
    size_t offset = 0;

    while (offset + 4 <= size)
    {
        if (data[offset] != 0xFF)
        {
            offset++;
            continue;
        }

        while (offset < size && data[offset] == 0xFF)
        {
            offset++;
        }

        if (offset >= size)
        {
            break;
        }

        const uint8_t marker = data[offset++];
        if (isStandaloneMarker(marker))
        {
            continue;
        }

        if (offset + 2 > size)
        {
            break;
        }

        const uint16_t segment_length =
            static_cast<uint16_t>((static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1]);
        offset += 2;

        if (segment_length < 2 || offset + segment_length - 2 > size)
        {
            break;
        }

        const size_t segment_end = offset + segment_length - 2;
        if (marker == 0xDB)
        {
            while (offset < segment_end)
            {
                const uint8_t table_info = data[offset++];
                DctTable table;
                table.precision = static_cast<uint8_t>(table_info >> 4);
                table.id = static_cast<uint8_t>(table_info & 0x0F);
                const size_t entry_size = table.precision == 0 ? 1 : 2;
                if (offset + 64 * entry_size > segment_end)
                {
                    break;
                }

                for (size_t index = 0; index < table.values.size(); index++)
                {
                    if (entry_size == 1)
                    {
                        table.values[index] = data[offset++];
                    }
                    else
                    {
                        table.values[index] =
                            static_cast<uint16_t>((static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1]);
                        offset += 2;
                    }
                }

                tables.push_back(table);
            }
        }

        offset = segment_end;
    }

    return tables;
}

//===================================================================================
//===================================================================================
// Converts the average quantization table value into the shader severity range.
float tableSeverity(const std::vector<DctTable>& tables)
{
    if (tables.empty())
    {
        return 0.0f;
    }

    double total = 0.0;
    size_t count = 0;
    for (const DctTable& table : tables)
    {
        total += std::accumulate(table.values.begin(), table.values.end(), 0.0);
        count += table.values.size();
    }

    const float average = count > 0 ? static_cast<float>(total / static_cast<double>(count)) : 0.0f;
    return std::clamp((average - 12.0f) / 90.0f, 0.0f, 1.0f);
}

//===================================================================================
//===================================================================================
// Builds postprocessing coefficients from observed quantization-table severity.
void fillParamsFromSeverity(float severity,
                            bool jpeg_deblocking,
                            bool adaptive_dithering,
                            gs::render::VideoPostprocessingParams& params)
{
    params = {};

    if (jpeg_deblocking)
    {
        params.deblocking_strength = 0.55f + 0.30f * severity;
        params.deblocking_alpha = (30.0f + 42.0f * severity) / 255.0f;
        params.deblocking_beta = (24.0f + 34.0f * severity) / 255.0f;
        params.deblocking_tc = 0.55f + 0.30f * severity;
    }

    if (adaptive_dithering)
    {
        params.dithering_strength = (6.0f + 8.0f * severity) / 255.0f;
        params.dithering_flat_threshold = 0.020f + 0.025f * severity;
    }
}

//===================================================================================
//===================================================================================
// Loads per-quality severity from the calibration JSON; shader params are derived
// from this value at runtime so generated JSON does not duplicate coefficient policy.
void loadCalibrationJsonLocked()
{
    if (s_loaded_calibration)
    {
        return;
    }
    s_loaded_calibration = true;

    std::ifstream in(kCalibrationPath);
    if (!in)
    {
        return;
    }

    const std::string json((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    size_t position = json.find("\"qualities\"");
    if (position == std::string::npos)
    {
        return;
    }

    while (true)
    {
        const size_t key_begin = json.find('"', position);
        if (key_begin == std::string::npos)
        {
            break;
        }

        const size_t key_end = json.find('"', key_begin + 1);
        if (key_end == std::string::npos)
        {
            break;
        }

        const std::string key = json.substr(key_begin + 1, key_end - key_begin - 1);
        char* parse_end = nullptr;
        const long quality = std::strtol(key.c_str(), &parse_end, 10);
        position = key_end + 1;
        if (parse_end == key.c_str() || *parse_end != '\0' || quality < kFirstQuality || quality > kLastQuality)
        {
            continue;
        }

        const size_t next_quality = json.find("\n    \"", position);
        const size_t search_end = next_quality == std::string::npos ? json.size() : next_quality;
        const size_t severity_key = json.find("\"severity\"", position);
        if (severity_key == std::string::npos || severity_key >= search_end)
        {
            continue;
        }

        const size_t colon = json.find(':', severity_key);
        if (colon == std::string::npos || colon >= search_end)
        {
            continue;
        }

        const char* severity_begin = json.c_str() + colon + 1;
        char* severity_end = nullptr;
        const float severity = std::strtof(severity_begin, &severity_end);
        if (severity_end != severity_begin)
        {
            s_loaded_severity[static_cast<uint8_t>(quality)] = std::clamp(severity, 0.0f, 1.0f);
        }
    }
}

//===================================================================================
//===================================================================================
// Writes all collected quality-level DCT tables into ov2640_dct.json for analysis.
void writeCalibrationJsonLocked()
{
    std::ofstream out(kCalibrationPath, std::ios::trunc);
    if (!out)
    {
        LOGW("Unable to open {} for OV5640 DCT calibration output", kCalibrationPath);
        return;
    }

    out << "{\n";
    out << "  \"format\": \"v1\",\n";
    out << "  \"note\": \"DCT here means JPEG DQT quantization coefficients extracted from received OV5640 JPEG frames.\",\n";
    out << "  \"forced_quality_min\": " << static_cast<int>(kFirstQuality) << ",\n";
    out << "  \"forced_quality_max\": " << static_cast<int>(kLastQuality) << ",\n";
    out << "  \"current_forced_quality\": " << static_cast<int>(s_forced_quality) << ",\n";
    out << "  \"complete\": " << (s_sweep_complete ? "true" : "false") << ",\n";
    out << "  \"qualities\": {\n";

    bool first_quality = true;
    for (const auto& quality_tables : s_observed_tables)
    {
        if (!first_quality)
        {
            out << ",\n";
        }
        first_quality = false;

        const float severity = tableSeverity(quality_tables.second);

        out << "    \"" << static_cast<int>(quality_tables.first) << "\": {\n";
        out << "      \"severity\": " << severity << ",\n";
        out << "      \"dct_coefficients\": [\n";

        for (size_t table_index = 0; table_index < quality_tables.second.size(); table_index++)
        {
            const DctTable& table = quality_tables.second[table_index];
            out << "        {\n";
            out << "          \"table_id\": " << static_cast<int>(table.id) << ",\n";
            out << "          \"precision\": " << static_cast<int>(table.precision) << ",\n";
            out << "          \"coefficients\": [";
            for (size_t value_index = 0; value_index < table.values.size(); value_index++)
            {
                if (value_index != 0)
                {
                    out << ", ";
                }
                out << table.values[value_index];
            }
            out << "]\n";
            out << "        }" << (table_index + 1 < quality_tables.second.size() ? "," : "") << "\n";
        }

        out << "      ]\n";
        out << "    }";
    }

    out << "\n  }\n";
    out << "}\n";
}

} // namespace

namespace gs::ov2640
{

//===================================================================================
//===================================================================================
// Forces one OV2640 JPEG quality level per outgoing config packet during calibration.
void applyTemporaryQualitySweep(Ground2Air_Config_Packet& config)
{
    std::lock_guard<std::mutex> lock(s_calibration_mutex);
    if (s_sweep_complete)
    {
        config.camera.quality = 0;
        return;
    }

    config.camera.quality = s_forced_quality;
    if (!s_logged_disabled_warning)
    {
        LOGW("OV5640 DCT calibration is enabled and temporarily forcing camera quality");
        s_logged_disabled_warning = true;
    }
    if (s_logged_forced_quality != s_forced_quality)
    {
        s_logged_forced_quality = s_forced_quality;
        LOGI("OV5640 DCT calibration forcing quality {}", static_cast<int>(s_forced_quality));
    }
}

//===================================================================================
//===================================================================================
// Observes JPEG DQT coefficients for the currently reported camera quality level.
void observeJpegDctTables(const uint8_t* data, size_t size, uint8_t reported_quality)
{
    // Config packets and JPEG frames are asynchronous. Label captures by the
    // quality reported by air stats so stale OV5640 frames are not written under
    // the next GS-forced quality during the sweep.
    if (data == nullptr || size == 0 || reported_quality < kFirstQuality || reported_quality > kLastQuality)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(s_calibration_mutex);
    if (s_last_reported_quality_log != reported_quality)
    {
        s_last_reported_quality_log = reported_quality;
        LOGI("OV5640 DCT calibration sees reported quality {}, forced quality {}",
             static_cast<int>(reported_quality),
             static_cast<int>(s_forced_quality));
    }
    if (s_sweep_complete || reported_quality != s_forced_quality)
    {
        s_matching_quality_frame_count = 0;
        return;
    }

    const std::vector<DctTable> tables = parseDctTables(data, size);
    if (tables.empty())
    {
        if (s_last_no_dqt_quality_log != reported_quality)
        {
            s_last_no_dqt_quality_log = reported_quality;
            LOGW("OV5640 DCT calibration found no JPEG DQT tables at quality {}, frame size {}",
                 static_cast<int>(reported_quality),
                 size);
        }
        return;
    }

    s_matching_quality_frame_count++;
    if (s_matching_quality_frame_count < kMatchedFramesPerQuality)
    {
        return;
    }

    s_observed_tables[s_forced_quality] = tables;
    if (s_forced_quality < kLastQuality)
    {
        s_forced_quality++;
        LOGI("OV5640 DCT calibration captured quality {}, next quality {}",
             static_cast<int>(reported_quality),
             static_cast<int>(s_forced_quality));
    }
    else
    {
        s_sweep_complete = true;
        LOGI("OV5640 DCT calibration captured quality {} and completed",
             static_cast<int>(reported_quality));
    }

    s_matching_quality_frame_count = 0;
    writeCalibrationJsonLocked();
}

//===================================================================================
//===================================================================================
// Returns calibrated shader coefficients for a previously observed quality level.
bool buildCalibratedPostprocessingParams(uint8_t quality,
                                         bool jpeg_deblocking,
                                         bool adaptive_dithering,
                                         gs::render::VideoPostprocessingParams& params)
{
    std::lock_guard<std::mutex> lock(s_calibration_mutex);
    loadCalibrationJsonLocked();
    const auto severity_found = s_loaded_severity.find(quality);
    if (severity_found != s_loaded_severity.end())
    {
        fillParamsFromSeverity(severity_found->second, jpeg_deblocking, adaptive_dithering, params);
        return true;
    }

    const auto found = s_observed_tables.find(quality);
    if (found == s_observed_tables.end())
    {
        params = {};
        return false;
    }

    fillParamsFromSeverity(tableSeverity(found->second), jpeg_deblocking, adaptive_dithering, params);
    return true;
}

} // namespace gs::ov2640

#else

namespace gs::ov2640
{

//===================================================================================
//===================================================================================
// Leaves camera quality unchanged when DCT calibration is disabled.
void applyTemporaryQualitySweep(Ground2Air_Config_Packet& config)
{
    (void)config;
}

//===================================================================================
//===================================================================================
// Ignores JPEG DCT tables when DCT calibration is disabled.
void observeJpegDctTables(const uint8_t* data, size_t size, uint8_t reported_quality)
{
    (void)data;
    (void)size;
    (void)reported_quality;
}

//===================================================================================
//===================================================================================
// Reports that no calibrated coefficients are available in normal builds.
bool buildCalibratedPostprocessingParams(uint8_t quality,
                                         bool jpeg_deblocking,
                                         bool adaptive_dithering,
                                         gs::render::VideoPostprocessingParams& params)
{
    (void)quality;
    (void)jpeg_deblocking;
    (void)adaptive_dithering;
    params = {};
    return false;
}

} // namespace gs::ov2640

#endif
