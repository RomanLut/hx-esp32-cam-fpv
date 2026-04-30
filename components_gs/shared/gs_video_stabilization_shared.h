#pragma once

#include <cstdint>
#include <vector>

namespace gs::stabilization
{

//===================================================================================
//===================================================================================
// Aggregates stabilization timings consumed once per stats window.
struct StabilizationStats
{
    uint32_t count = 0;
    uint32_t min_ms = 0;
    uint32_t max_ms = 0;
};

//===================================================================================
//===================================================================================
// Holds the render-time affine stabilization transform in source pixel coordinates.
struct StabilizationTransform
{
    bool enabled = false;
    int width = 0;
    int height = 0;
    float m00 = 1.0f;
    float m01 = 0.0f;
    float m02 = 0.0f;
    float m10 = 0.0f;
    float m11 = 1.0f;
    float m12 = 0.0f;
};

//===================================================================================
//===================================================================================
// Returns true when video stabilization is enabled for this process.
bool isEnabled();

//===================================================================================
//===================================================================================
// Resets temporal stabilization state so the next frame becomes a new reference.
void reset();

//===================================================================================
//===================================================================================
// Estimates stabilization transform for an RGB565 frame without modifying pixels.
bool estimateRgb565Frame(const std::vector<uint8_t>& pixels,
                         int width,
                         int height,
                         int stride);

//===================================================================================
//===================================================================================
// Estimates stabilization transform for a caller-owned RGB565 frame without copying.
bool estimateRgb565Frame(const uint8_t* pixels,
                         size_t size,
                         int width,
                         int height,
                         int stride);

//===================================================================================
//===================================================================================
// Estimates stabilization transform for a caller-owned RGB24 frame without copying.
bool estimateRgbFrame(const uint8_t* pixels,
                      size_t size,
                      int width,
                      int height,
                      int stride);

//===================================================================================
//===================================================================================
// Returns the latest transform estimated for render-time stabilization.
StabilizationTransform getLatestTransform();

//===================================================================================
//===================================================================================
// Returns and resets stabilization timings accumulated since the previous call.
StabilizationStats consumeStats();

} // namespace gs::stabilization
