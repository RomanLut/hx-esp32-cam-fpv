#pragma once

#include <cstdint>

#include "gs_lens_correction_shared.h"
#include "gs_video_stabilization_shared.h"
#include "gs_video_layout_shared.h"

namespace gs::render
{
constexpr float kJpegQualityBest = 8.0f;
constexpr float kJpegQualityWorst = 63.0f;
constexpr uint8_t kJpegQualityUnknownFallback = 40;

//===================================================================================
//===================================================================================
// Packs shader postprocessing strengths derived from GS toggles and JPEG quality.
struct VideoPostprocessingParams
{
    float deblocking_strength = 0.0f;
    float deblocking_alpha = 0.0f;
    float deblocking_beta = 0.0f;
    float deblocking_tc = 0.0f; // maximum boundary smoothing weight
    float debanding_strength = 0.0f;
    float debanding_flat_threshold = 0.0f;
    float debanding_range = 0.0f;
    float dithering_strength = 0.0f;
    float dithering_flat_threshold = 0.0f;
};

float postprocessingLevelScale(uint8_t level);
VideoPostprocessingParams buildVideoPostprocessingParams(uint8_t current_quality);

//===================================================================================
//===================================================================================
// Draws video textures with startup-compiled shaders for each enabled feature set.
class VideoShaderRenderer
{
public:
    VideoShaderRenderer() = default;
    ~VideoShaderRenderer();

    void release();
    bool draw(unsigned int texture,
              const VideoQuad& quad,
              float clip_x,
              float clip_y,
              float clip_width,
              float clip_height,
              float surface_width,
              float surface_height,
              int frame_width,
              int frame_height,
              const LensCorrectionParams& lens_params,
              const gs::stabilization::StabilizationTransform& stabilization_transform,
              const VideoPostprocessingParams& postprocessing_params);

private:
    static constexpr unsigned int kShaderProgramCount = 32;

    unsigned int m_programs[kShaderProgramCount] = {};
    bool m_programs_ready = false;
};
}
