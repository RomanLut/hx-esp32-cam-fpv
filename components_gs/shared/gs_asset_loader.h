#pragma once

#include <cstdint>
#include <vector>

// Loads a named GS asset JPEG (e.g. "nosignal.jpg") into out.
// Android: reads from the APK asset bundle via AAssetManager.
// Linux:   reads ../assets_gs/<name> (relative to the gs/ launch directory).
bool loadAssetJpeg(const char* name, std::vector<uint8_t>& out);
