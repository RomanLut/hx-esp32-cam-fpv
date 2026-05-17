#pragma once

#include <cstddef>
#include <cstdint>

#include "gs_video_shader_renderer.h"

namespace gs::render
{

bool buildJpegDctPostprocessingParams(const uint8_t* jpeg_data,
                                      size_t jpeg_size,
                                      VideoPostprocessingParams& params);

} // namespace gs::render
