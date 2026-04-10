#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "imgui.h"
#include "core/osd_menu_controller.h"

namespace gs::mcp
{

/*
===================================================================================
===================================================================================
MCP design requirements for Ground Station debugging

1. The MCP endpoint must live inside the GS process.
   The GS executable or native Android GS runtime is the MCP server. There is no
   separate helper process that owns menu or transport control.

2. The write path must be input-only.
   MCP is allowed to query runtime state and inject fake key presses. It must not
   call transport methods directly, mutate APFPV camera selection directly, or use
   any privileged shortcut that bypasses normal UI behavior.

3. Transport behavior must be exercised only through the menu.
   If transport mode changes, APFPV reconnects, or camera selection occurs, it must
   happen because the existing menu logic consumed synthetic keys and executed its
   normal code path.

4. Menu introspection must come from the rendered menu itself.
   OSDMenuController captures the menu content it actually drew and exposes a debug
   buffer. The selected item is masked as "[*]" and unselected menu items are masked
   as "[ ]". MCP reads that captured buffer when it needs to understand navigation
   state.

5. Shared code must be reusable across GS platforms.
   The MCP protocol handling, socket server, request parsing, key injection queue,
   and snapshot building live in common shared code so Linux GS and Android GS reuse
   the same implementation and tool surface.

6. The endpoint is for debug automation.
   The server may bind beyond localhost when platform debugging requires remote
   access, such as reaching a Radxa GS instance over wired Ethernet. It should still
   remain narrow in scope: query state, inspect the rendered menu buffer, and inject
   synthetic keys.

7. Main-thread ImGui ownership must be preserved.
   Network I/O may run on a background thread, but injected keys are only queued from
   MCP. They are drained into ImGui from the render thread before the frame consumes
   input.
===================================================================================
===================================================================================
*/

//===================================================================================
//===================================================================================
// Runs the embedded MCP endpoint and shared key injection queue for GS.
class GsMcpServer
{
public:
    static GsMcpServer& instance();

    void start(uint16_t port = 17654);
    void stop();
    bool isRunning() const;
    uint16_t port() const;

private:
    GsMcpServer() = default;
    ~GsMcpServer() = default;
    GsMcpServer(const GsMcpServer&) = delete;
    GsMcpServer& operator=(const GsMcpServer&) = delete;
};

//===================================================================================
//===================================================================================
// Describes one queued synthetic key transition shared with platform backends.
struct InjectedKeyTransition
{
    ImGuiKey key = ImGuiKey_None;
    bool down = false;
};

//===================================================================================
//===================================================================================
// Queues one synthetic key press to be injected into ImGui on the next frame.
void queueInjectedKeyPress(ImGuiKey key);

//===================================================================================
//===================================================================================
// Queues multiple synthetic key presses to be injected into ImGui on future frames.
void queueInjectedKeyPresses(const std::vector<ImGuiKey>& keys);

//===================================================================================
//===================================================================================
// Drains queued synthetic keys into the current ImGui frame on the render thread.
void drainInjectedKeysToImGui();

//===================================================================================
//===================================================================================
// Pops the next queued synthetic key transition for platform-specific input injection.
bool popInjectedKeyTransition(InjectedKeyTransition& transition);

} // namespace gs::mcp
