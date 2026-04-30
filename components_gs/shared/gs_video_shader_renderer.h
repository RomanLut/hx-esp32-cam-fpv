#pragma once

#include "gs_lens_correction_shared.h"
#include "gs_video_stabilization_shared.h"
#include "gs_video_layout_shared.h"

namespace gs::render
{
//===================================================================================
//===================================================================================
// Draws video textures with a fast path and an optional lens-correction shader.
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
              const gs::stabilization::StabilizationTransform& stabilization_transform);

private:
    unsigned int m_fast_program = 0;
    unsigned int m_lens_program = 0;
};
}
