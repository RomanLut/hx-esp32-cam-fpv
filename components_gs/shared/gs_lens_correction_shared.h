#pragma once

#include "gs_shared_state.h"

namespace gs::render
{
//===================================================================================
//===================================================================================
// Stores one normalized 2D coordinate.
struct Vec2
{
    float x = 0.0f;
    float y = 0.0f;
};

//===================================================================================
//===================================================================================
// Packs lens correction intrinsics and coefficients in float form for shader upload.
struct LensCorrectionParams
{
    bool enabled = false;
    bool has_camera_matrix = false;
    float fx_norm = 0.0f;
    float fy_norm = 0.0f;
    float cx_norm = 0.5f;
    float cy_norm = 0.5f;
    float k1 = 0.0f;
    float k2 = 0.0f;
    float k3 = 0.0f;
    float p1 = 0.0f;
    float p2 = 0.0f;
};

LensCorrectionParams buildLensCorrectionParams(const LensCorrectionState& state);
bool isLensCorrectionEnabled(const LensCorrectionParams& params);
Vec2 calculateLensCorrectedSampleCoord(Vec2 normalized_coord,
                                       float aspect,
                                       const LensCorrectionParams& params);
}
