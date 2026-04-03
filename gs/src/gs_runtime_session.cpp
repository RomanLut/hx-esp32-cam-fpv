#include "gs_runtime_session.h"

SessionPulseStats consumeSessionPulseStats(gs::core::GsSessionCore& session, Clock::time_point now)
{
    SessionPulseStats stats;
    stats.link_status = session.consumeLinkStatus(now);
    stats.periodic_stats = session.consumePeriodicStats();
    return stats;
}
