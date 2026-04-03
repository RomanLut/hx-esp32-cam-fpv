#pragma once

int runLinuxRuntimeLoop(char* argv[]);

#ifdef USE_MAVLINK
bool init_uart();
#endif
