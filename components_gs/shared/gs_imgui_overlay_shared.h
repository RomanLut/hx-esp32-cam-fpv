#pragma once

#include <string>
#include <vector>

namespace gs::imgui
{
struct OverlayChipSpec
{
    std::string text;
    bool alert = false;
    float width = 0.0f;
};

void drawOverlayChipStrip(const std::vector<OverlayChipSpec>& chips,
                          float start_x = 0.0f,
                          float start_y = 0.0f,
                          float chip_height = 0.0f,
                          float gap = 6.0f);
}
