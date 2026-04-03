#pragma once

#include <cstdint>

#include "core/transport.h"

void cleanupLinuxSingleInstancePidFile();
bool ensureLinuxSingleInstance();
void findLinuxRXInterfacesEx(gs::core::RXDescriptor& rx_descriptor);
bool findLinuxRXInterfaces(gs::core::RXDescriptor& rx_descriptor);
uint16_t generateLinuxDeviceId();
