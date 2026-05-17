#pragma once

#include <cstddef>
#include <cstdint>

#include "gs_video_shader_renderer.h"
#include "packets.h"

namespace gs::ov2640
{

void applyTemporaryQualitySweep(Ground2Air_Config_Packet& config);
void observeJpegDctTables(const uint8_t* data, size_t size, uint8_t reported_quality);
bool buildCalibratedPostprocessingParams(uint8_t quality,
                                         bool jpeg_deblocking,
                                         bool adaptive_dithering,
                                         gs::render::VideoPostprocessingParams& params);

}
