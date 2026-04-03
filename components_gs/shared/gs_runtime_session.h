#pragma once

#include "Clock.h"
#include "core/gs_session_core.h"

struct SessionPulseStats
{
    gs::core::LinkStatusSnapshot link_status = {};
    gs::core::PeriodicStatsSnapshot periodic_stats = {};
};

SessionPulseStats consumeSessionPulseStats(gs::core::GsSessionCore& session, Clock::time_point now);
