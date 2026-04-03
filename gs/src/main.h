#pragma once

#include <iostream>
#include <string>
#include <deque>
#include <mutex>
#include <algorithm>
#include <cstdio>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include "gs_gl_debug.h"
#include "gs_linux_runtime.h"
#include "gs_shared_state.h"
#include "gs_shared_runtime.h"
#include "gs_stats.h"

#define USE_MAVLINK

