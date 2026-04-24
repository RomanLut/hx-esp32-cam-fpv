#include "gs_lens_correction_shared.h"

#include <algorithm>
#include <cmath>

namespace gs::render
{
//===================================================================================
//===================================================================================
// Converts the persisted double-precision lens state to render-time parameters.
LensCorrectionParams buildLensCorrectionParams(const LensCorrectionState& state)
{
    LensCorrectionParams params;
    params.enabled = state.enabled;
    params.k1 = static_cast<float>(state.k1);
    params.k2 = static_cast<float>(state.k2);
    params.k3 = static_cast<float>(state.k3);
    params.p1 = static_cast<float>(state.p1);
    params.p2 = static_cast<float>(state.p2);
    return params;
}

//===================================================================================
//===================================================================================
// Returns true when lens correction has visible work to do.
bool isLensCorrectionEnabled(const LensCorrectionParams& params)
{
    return params.enabled &&
           (params.k1 != 0.0f ||
            params.k2 != 0.0f ||
            params.k3 != 0.0f ||
            params.p1 != 0.0f ||
            params.p2 != 0.0f);
}

//===================================================================================
//===================================================================================
// Calculates the source sample coordinate using OpenCV normalized camera coordinates.
Vec2 calculateLensCorrectedSampleCoord(Vec2 normalized_coord,
                                       float aspect,
                                       const LensCorrectionParams& params)
{
    if (!isLensCorrectionEnabled(params))
    {
        return normalized_coord;
    }

    aspect = std::max(aspect, 0.0001f);

    // OpenCV distortion coefficients are resolution-independent when they are
    // applied to normalized camera coordinates. Without a stored camera matrix,
    // the shader assumes centered square pixels and focal length equal to half
    // the image height, so x carries the image aspect and both axes use [-1, 1].
    const float x = (normalized_coord.x - 0.5f) * 2.0f * aspect;
    const float y = (normalized_coord.y - 0.5f) * 2.0f;
    const float r2 = x * x + y * y;
    const float r4 = r2 * r2;
    const float r6 = r4 * r2;
    const float radial = 1.0f + params.k1 * r2 + params.k2 * r4 + params.k3 * r6;
    const float xy2 = 2.0f * x * y;
    const float sample_x = x * radial + params.p1 * xy2 + params.p2 * (r2 + 2.0f * x * x);
    const float sample_y = y * radial + params.p1 * (r2 + 2.0f * y * y) + params.p2 * xy2;

    return {
        sample_x / (2.0f * aspect) + 0.5f,
        sample_y * 0.5f + 0.5f
    };
}
}
