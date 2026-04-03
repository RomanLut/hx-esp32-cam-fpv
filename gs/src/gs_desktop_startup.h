#pragma once

#include <cstdint>

#include "core/transport.h"

void cleanupDesktopSingleInstancePidFile();
bool ensureDesktopSingleInstance();
void findDesktopRXInterfacesEx(gs::core::RXDescriptor& rx_descriptor);
bool findDesktopRXInterfaces(gs::core::RXDescriptor& rx_descriptor);
uint16_t generateDesktopDeviceId();
