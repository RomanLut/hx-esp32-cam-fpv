#pragma once

namespace gs::core
{

//===================================================================================
//===================================================================================
// Lists the runtime-selectable transport backends shared by Linux and Android GS.
enum class TransportKind
{
    RawBroadcast = 0,
    APFPV = 1,
    TestTransport = 2
};

//===================================================================================
//===================================================================================
// Parses a persisted integer transport identifier and falls back to the provided default.
TransportKind transportKindFromInt(int value, TransportKind fallback);

//===================================================================================
//===================================================================================
// Converts a transport kind to a stable persisted integer identifier.
int transportKindToInt(TransportKind kind);

}
