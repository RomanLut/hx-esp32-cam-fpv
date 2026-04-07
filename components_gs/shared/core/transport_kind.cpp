#include "core/transport_kind.h"

namespace gs::core
{

//===================================================================================
//===================================================================================
// Parses a persisted integer transport identifier and falls back to the provided default.
TransportKind transportKindFromInt(int value, TransportKind fallback)
{
    switch (value)
    {
    case 0:
        return TransportKind::RawBroadcast;

    case 1:
        return TransportKind::APFPV;

    case 2:
        return TransportKind::TestTransport;

    default:
        return fallback;
    }
}

//===================================================================================
//===================================================================================
// Converts a transport kind to a stable persisted integer identifier.
int transportKindToInt(TransportKind kind)
{
    switch (kind)
    {
    case TransportKind::RawBroadcast:
        return 0;

    case TransportKind::APFPV:
        return 1;

    case TransportKind::TestTransport:
        return 2;
    }

    return transportKindToInt(TransportKind::RawBroadcast);
}

}
