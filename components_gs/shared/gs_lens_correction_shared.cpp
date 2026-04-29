#include "gs_lens_correction_shared.h"

#include <algorithm>
#include <cmath>

#include "gs_camera_calibration_shared.h"

namespace gs::render
{
//===================================================================================
//===================================================================================
// Converts the persisted double-precision lens state to render-time parameters.
LensCorrectionParams buildLensCorrectionParams(const LensCorrectionState& state)
{
    LensCorrectionParams params;
    params.enabled = state.enabled && !gs::calibration::wantsLensCorrectionDisabled();
    params.has_camera_matrix = state.image_width > 0 &&
                               state.image_height > 0 &&
                               state.fx > 0.0 &&
                               state.fy > 0.0;
    if (params.has_camera_matrix)
    {
        params.fx_norm = static_cast<float>(state.fx / static_cast<double>(state.image_width));
        params.fy_norm = static_cast<float>(state.fy / static_cast<double>(state.image_height));
        params.cx_norm = static_cast<float>(state.cx / static_cast<double>(state.image_width));
        params.cy_norm = static_cast<float>(state.cy / static_cast<double>(state.image_height));
    }
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

    // OpenCV coefficients must be applied through the same normalized camera
    // matrix used during calibration. The fallback preserves older manually
    // entered coefficients that assumed a centered camera and square pixels.
    const float fx_norm = params.has_camera_matrix ? std::max(params.fx_norm, 0.0001f) : 0.5f / aspect;
    const float fy_norm = params.has_camera_matrix ? std::max(params.fy_norm, 0.0001f) : 0.5f;
    const float cx_norm = params.has_camera_matrix ? params.cx_norm : 0.5f;
    const float cy_norm = params.has_camera_matrix ? params.cy_norm : 0.5f;
    const float x = (normalized_coord.x - cx_norm) / fx_norm;
    const float y = (normalized_coord.y - cy_norm) / fy_norm;
    const float r2 = x * x + y * y;
    const float r4 = r2 * r2;
    const float r6 = r4 * r2;
    const float radial = 1.0f + params.k1 * r2 + params.k2 * r4 + params.k3 * r6;
    const float xy2 = 2.0f * x * y;
    const float sample_x = x * radial + params.p1 * xy2 + params.p2 * (r2 + 2.0f * x * x);
    const float sample_y = y * radial + params.p1 * (r2 + 2.0f * y * y) + params.p2 * xy2;

    return {
        sample_x * fx_norm + cx_norm,
        sample_y * fy_norm + cy_norm
    };
}
}
