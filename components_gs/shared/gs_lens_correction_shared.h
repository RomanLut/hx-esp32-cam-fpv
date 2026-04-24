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
// Packs lens correction coefficients in float form for shader upload.
struct LensCorrectionParams
{
    bool enabled = false;
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
