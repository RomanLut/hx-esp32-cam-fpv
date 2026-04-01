#pragma once

#include <string>
#include <vector>
#include <functional>

#include "packets.h"
#include "core/osd_menu_common.h"

namespace gs::menu
{

struct MenuState
{
    bool visible = false;
    OSDMenuId menu_id = OSDMenuId::Main;
    int selected_item = 0;
    std::vector<OSDMenuId> back_menu_ids;
    std::vector<int> back_menu_items;
};

struct MenuScreen
{
    std::string title;
    std::vector<std::string> items;
    std::vector<std::string> statuses;
    std::vector<std::string> status_lines;
    int selected_item = 0;
    bool can_go_back = false;
};

struct MenuViewContext
{
    const Ground2Air_Config_Packet* config = nullptr;
    bool is_ov5640 = false;
    int connected_air_device_id = 0;
    int gs_screen_aspect_ratio = 1;
    int gs_wifi_band = 0;
    int gs_tx_power = 0;
    std::string gs_tx_interface = "auto";
    int gs_gpio_keys_layout = 0;
    int gs_osd_font_index = 0;
    std::vector<std::string> transport_interfaces;
    std::string air_sd_status;
    std::string gs_sd_status;
    std::string gs_ip = "192.168.4.2";
};

struct MenuActionContext
{
    Ground2Air_Config_Packet* config = nullptr;
    bool is_ov5640 = false;
    int* gs_screen_aspect_ratio = nullptr;
    int* gs_wifi_band = nullptr;
    int* gs_tx_power = nullptr;
    std::string* gs_tx_interface = nullptr;
    int* gs_gpio_keys_layout = nullptr;
    int* gs_osd_font_index = nullptr;
    bool* show_stats = nullptr;
    int* gs_packet_debug_mode = nullptr;
    bool* exit_requested = nullptr;
    std::vector<std::string> transport_interfaces;
    std::function<void()> on_commit_config;
    std::function<void()> on_begin_search;
};

void resetMenuState(MenuState& state);
void goForward(MenuState& state, OSDMenuId new_menu_id, int new_item);
bool goBack(MenuState& state);
MenuScreen buildMenuScreen(const MenuState& state, const MenuViewContext& context);

}
