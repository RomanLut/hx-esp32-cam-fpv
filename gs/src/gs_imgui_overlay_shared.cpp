#include "gs_imgui_overlay_shared.h"

#include <algorithm>

#include "imgui.h"

namespace gs::imgui
{
void drawOverlayChipStrip(const std::vector<OverlayChipSpec>& chips,
                          float start_x,
                          float start_y,
                          float chip_height,
                          float gap)
{
    const float resolved_height = chip_height > 0.0f ? chip_height : std::max(20.0f, ImGui::GetIO().DisplaySize.y * 0.04f);
    float x = start_x;

    for (size_t i = 0; i < chips.size(); ++i)
    {
        const auto& chip = chips[i];
        if (chip.text.empty())
        {
            continue;
        }

        const ImVec2 text_size = ImGui::CalcTextSize(chip.text.c_str());
        const float chip_width = chip.width > 0.0f ? chip.width : std::max(44.0f, 16.0f + text_size.x);
        const ImVec4 bg = chip.alert ? ImVec4(0.54f, 0.29f, 0.29f, 0.80f)
                                     : ImVec4(0.42f, 0.42f, 0.42f, 0.80f);

        ImGui::SetCursorPos(ImVec2(x, start_y));
        ImGui::PushID(static_cast<int>(i));
        ImGui::PushStyleColor(ImGuiCol_Button, bg);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bg);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, bg);
        ImGui::Button(chip.text.c_str(), ImVec2(chip_width, resolved_height));
        ImGui::PopStyleColor(3);
        ImGui::PopID();

        x += chip_width + gap;
    }
}
}
