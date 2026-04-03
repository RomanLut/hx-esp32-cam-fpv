#include "android_asset_flight_osd.h"

#include <GLES2/gl2.h>
#include <android/asset_manager.h>
#include <android/log.h>

#include "android_jni_shared.h"
#include "gs_flight_osd_render_shared.h"
#include "imgui.h"
#include <algorithm>
#include <vector>

namespace
{

constexpr const char* kLogTag = "AndroidAssetFlightOsd";

} // namespace

AndroidAssetFlightOsd::AndroidAssetFlightOsd()
{
}

AndroidAssetFlightOsd::~AndroidAssetFlightOsd() = default;

void AndroidAssetFlightOsd::draw(int surface_width,
                      int surface_height,
                      int frame_width,
                      int frame_height,
                      int screen_mode,
                      bool vr_mode)
{
    if (!ensureFont())
    {
        return;
    }
    gs::render::drawFlightOsd(*this, surface_width, surface_height, frame_width, frame_height, screen_mode, vr_mode);
}

bool AndroidAssetFlightOsd::loadFontBytes(const std::string& font_name, std::vector<unsigned char>& font_png)
{
    AAssetManager* asset_manager = androidGetAssetManager();
    if (asset_manager == nullptr)
    {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Asset manager is not set");
        return false;
    }

    const std::string asset_path = "osd_fonts/" + font_name;
    AAsset* asset = AAssetManager_open(asset_manager, asset_path.c_str(), AASSET_MODE_BUFFER);
    if (asset == nullptr)
    {
        onFontLoadError("Failed to open OSD font asset", font_name);
        return false;
    }

    const off_t asset_size = AAsset_getLength(asset);
    if (asset_size <= 0)
    {
        AAsset_close(asset);
        onFontLoadError("OSD font asset is empty", font_name);
        return false;
    }

    font_png.resize(static_cast<size_t>(asset_size));
    const int read_size = AAsset_read(asset, font_png.data(), static_cast<size_t>(asset_size));
    AAsset_close(asset);
    if (read_size != asset_size)
    {
        onFontLoadError("Failed to read OSD font asset", font_name);
        return false;
    }

    return true;
}

void AndroidAssetFlightOsd::onFontLoadError(const std::string& message, const std::string& font_name)
{
    __android_log_print(ANDROID_LOG_ERROR, kLogTag, "%s: %s", message.c_str(), font_name.c_str());
}
