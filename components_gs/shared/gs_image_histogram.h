#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace gs::image_histogram
{

constexpr size_t kBinCount = 256;
using Bins = std::array<uint32_t, kBinCount>;

namespace detail
{

//===================================================================================
//===================================================================================
// Stores the decoder-to-menu histogram exchange and its collection gate.
struct State
{
    std::atomic<bool> collection_enabled = false;
    std::mutex bins_mutex;
    Bins bins = {};
};

//===================================================================================
//===================================================================================
// Returns the process-wide histogram state shared by renderer and decoder threads.
inline State& state()
{
    static State value;
    return value;
}

//===================================================================================
//===================================================================================
// Converts one RGB color to its integer Rec. 601 luma histogram bin.
inline uint8_t luma(uint8_t red, uint8_t green, uint8_t blue)
{
    return static_cast<uint8_t>((77u * red + 150u * green + 29u * blue) >> 8u);
}

//===================================================================================
//===================================================================================
// Publishes a completed local histogram without exposing a partial decoder update.
inline void publish(const Bins& bins)
{
    State& shared = state();
    std::lock_guard<std::mutex> lock(shared.bins_mutex);
    shared.bins = bins;
}

} // namespace detail

//===================================================================================
//===================================================================================
// Enables decoded-pixel counting only while an image-settings histogram is drawn.
inline void setCollectionEnabled(bool enabled)
{
    detail::state().collection_enabled.store(enabled, std::memory_order_relaxed);
}

//===================================================================================
//===================================================================================
// Returns whether a decoder should spend time counting decoded pixel colors.
inline bool isCollectionEnabled()
{
    return detail::state().collection_enabled.load(std::memory_order_relaxed);
}

//===================================================================================
//===================================================================================
// Copies the latest complete histogram for lock-safe rendering.
inline Bins copyLatest()
{
    detail::State& shared = detail::state();
    std::lock_guard<std::mutex> lock(shared.bins_mutex);
    return shared.bins;
}

//===================================================================================
//===================================================================================
// Counts luma values from a decoded packed RGB888 image when collection is enabled.
inline void updateFromRgb888(const uint8_t* pixels, int width, int height, int stride)
{
    if (!isCollectionEnabled() || pixels == nullptr || width <= 0 || height <= 0 || stride < width * 3)
    {
        return;
    }

    Bins bins = {};
    for (int y = 0; y < height; ++y)
    {
        const uint8_t* row = pixels + static_cast<size_t>(y) * static_cast<size_t>(stride);
        for (int x = 0; x < width; ++x)
        {
            const uint8_t* color = row + static_cast<size_t>(x) * 3u;
            ++bins[detail::luma(color[0], color[1], color[2])];
        }
    }
    detail::publish(bins);
}

//===================================================================================
//===================================================================================
// Counts luma values from a decoded packed RGBA8888 image when collection is enabled.
inline void updateFromRgba8888(const uint8_t* pixels, int width, int height, int stride)
{
    if (!isCollectionEnabled() || pixels == nullptr || width <= 0 || height <= 0 || stride < width * 4)
    {
        return;
    }

    Bins bins = {};
    for (int y = 0; y < height; ++y)
    {
        const uint8_t* row = pixels + static_cast<size_t>(y) * static_cast<size_t>(stride);
        for (int x = 0; x < width; ++x)
        {
            const uint8_t* color = row + static_cast<size_t>(x) * 4u;
            ++bins[detail::luma(color[0], color[1], color[2])];
        }
    }
    detail::publish(bins);
}

//===================================================================================
//===================================================================================
// Counts luma values from a decoded little-endian RGB565 image when collection is enabled.
inline void updateFromRgb565(const uint8_t* pixels, int width, int height, int stride)
{
    if (!isCollectionEnabled() || pixels == nullptr || width <= 0 || height <= 0 || stride < width * 2)
    {
        return;
    }

    Bins bins = {};
    for (int y = 0; y < height; ++y)
    {
        const uint8_t* row = pixels + static_cast<size_t>(y) * static_cast<size_t>(stride);
        for (int x = 0; x < width; ++x)
        {
            const uint8_t* color = row + static_cast<size_t>(x) * 2u;
            const uint16_t packed = static_cast<uint16_t>(color[0]) |
                                    (static_cast<uint16_t>(color[1]) << 8u);
            const uint8_t red = static_cast<uint8_t>(((packed >> 11u) & 0x1fu) * 255u / 31u);
            const uint8_t green = static_cast<uint8_t>(((packed >> 5u) & 0x3fu) * 255u / 63u);
            const uint8_t blue = static_cast<uint8_t>((packed & 0x1fu) * 255u / 31u);
            ++bins[detail::luma(red, green, blue)];
        }
    }
    detail::publish(bins);
}

} // namespace gs::image_histogram
