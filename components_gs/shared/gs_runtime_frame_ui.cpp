#include "gs_runtime_frame_ui.h"

#include "core/stats_panel_shared.h"

void drawRuntimeFrameUiContent(const RuntimeFrameUiState& state)
{
    drawRuntimeOverlayStatus(state.overlay);
    if (state.overlay.stats_visible)
    {
        gs::stats::drawFullscreenStatsPanel(state.overlay.stats_snapshot);
    }
}
