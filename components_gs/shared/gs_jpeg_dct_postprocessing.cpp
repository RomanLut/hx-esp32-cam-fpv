#include "gs_jpeg_dct_postprocessing.h"

#include <algorithm>
#include <array>
#include <numeric>
#include <vector>

#include "gs_shared_state.h"

namespace
{

//===================================================================================
//===================================================================================
// Holds one JPEG DQT quantization table extracted from the encoded frame.
struct DctTable
{
    uint8_t id = 0;
    uint8_t precision = 0;
    std::array<uint16_t, 64> values = {};
};

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
// Parses JPEG DQT segments from one encoded frame.
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
        if (marker == 0xDA)
        {
            // DQT tables belong in the header area before scan entropy. After SOS,
            // byte stuffing can contain marker-like values that are not segments.
            break;
        }

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
// Converts average quantization-table strength into the shader severity range.
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
// Builds postprocessing coefficients from per-frame quantization-table severity.
void fillParamsFromSeverity(float severity, gs::render::VideoPostprocessingParams& params)
{
    params = {};

    if (s_postprocessingState.jpeg_deblocking_enabled)
    {
        params.deblocking_strength = 0.55f + 0.30f * severity;
        params.deblocking_alpha = (30.0f + 42.0f * severity) / 255.0f;
        params.deblocking_beta = (24.0f + 34.0f * severity) / 255.0f;
        params.deblocking_tc = 0.55f + 0.30f * severity;
    }

    const float dithering_scale =
        gs::render::postprocessingLevelScale(s_postprocessingState.adaptive_dithering_level);
    if (dithering_scale > 0.0f)
    {
        params.dithering_strength = ((6.0f + 8.0f * severity) * dithering_scale) / 255.0f;
        params.dithering_flat_threshold = 0.020f + 0.025f * severity;
    }
}

} // namespace

namespace gs::render
{

//===================================================================================
//===================================================================================
// Builds shader postprocessing params from JPEG DQT tables embedded in one frame.
bool buildJpegDctPostprocessingParams(const uint8_t* jpeg_data,
                                      size_t jpeg_size,
                                      VideoPostprocessingParams& params)
{
    params = {};
    if (jpeg_data == nullptr || jpeg_size == 0)
    {
        return false;
    }

    const std::vector<DctTable> tables = parseDctTables(jpeg_data, jpeg_size);
    if (tables.empty())
    {
        // Some malformed or abbreviated JPEG streams may omit DQT. Keep
        // postprocessing off for that frame rather than reusing stale metadata.
        return false;
    }

    fillParamsFromSeverity(tableSeverity(tables), params);
    return true;
}

} // namespace gs::render
