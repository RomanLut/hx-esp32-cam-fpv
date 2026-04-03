#pragma once

#include <cstdint>
#include <string>

#include "gs_runtime_platform_services.h"

struct AirStorageStatusView
{
    bool detected = false;
    bool error = false;
    bool slow = false;
    uint16_t free_space_gb16 = 0;
    uint16_t total_space_gb16 = 0;
};

std::string formatAirStorageStatusLine(const AirStorageStatusView& status,
                                       const char* detected_label = "Ok",
                                       const char* missing_label = "?",
                                       const char* trailing_suffix = "");

std::string formatGroundStorageStatusLine(const GroundStorageStatus& status);
