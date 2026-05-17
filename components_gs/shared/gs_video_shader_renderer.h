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
    float dithering_strength = 0.0f;
    float dithering_flat_threshold = 0.0f;
};

float postprocessingLevelScale(uint8_t level);
VideoPostprocessingParams buildVideoPostprocessingParams(uint8_t current_quality);

//===================================================================================
//===================================================================================
// Draws video textures with source-resolution artifact and display shader stages.
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
    static constexpr unsigned int kArtifactShaderProgramCount = 2;
    static constexpr unsigned int kDisplayShaderProgramCount = 8;

    bool ensureArtifactTarget(int frame_width, int frame_height);
    void releaseArtifactTarget();

    unsigned int m_artifact_programs[kArtifactShaderProgramCount] = {};
    unsigned int m_display_programs[kDisplayShaderProgramCount] = {};
    bool m_programs_ready = false;
    unsigned int m_artifact_framebuffer = 0;
    unsigned int m_artifact_texture = 0;
    int m_artifact_width = 0;
    int m_artifact_height = 0;
};
}
