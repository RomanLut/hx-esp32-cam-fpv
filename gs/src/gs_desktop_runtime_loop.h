#pragma once

int runDesktopRuntimeLoop(char* argv[]);

#ifdef USE_MAVLINK
bool init_uart();
#endif
