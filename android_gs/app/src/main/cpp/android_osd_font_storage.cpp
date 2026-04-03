#include "android_osd_font_storage.h"

#include <android/asset_manager.h>

#include "android_jni_shared.h"
#include "flight_osd.h"

namespace
{

//===================================================================================
//===================================================================================
// Owns the Android OSD font storage implementation for explicit runtime binding.
AndroidOSDFontStorage s_android_osd_font_storage;

//===================================================================================
//===================================================================================
// Lists the built-in Android OSD fonts packaged with the application assets.
const std::vector<std::string> kAndroidBuiltinOsdFontNames = {
    s_flightOSD.defaultFontName(),
};
} // namespace

//===================================================================================
//===================================================================================
// Returns the shared Android OSD font storage instance for explicit runtime binding.
IOSDFontStorage& getAndroidOsdFontStorage()
{
    return s_android_osd_font_storage;
}

//===================================================================================
//===================================================================================
// Returns the Android OSD font list exposed by the packaged APK assets.
const std::vector<std::string>& AndroidOSDFontStorage::osdFontsList() const
{
    return kAndroidBuiltinOsdFontNames;
}

//===================================================================================
//===================================================================================
// Loads raw PNG bytes for an Android OSD font from APK assets via AAssetManager.
bool AndroidOSDFontStorage::loadOSDFontBytes(const std::string& font_name, std::vector<unsigned char>& font_png) const
{
    AAssetManager* asset_manager = androidGetAssetManager();
    if (asset_manager == nullptr)
    {
        return false;
    }

    const std::string asset_path = "osd_fonts/" + font_name;
    AAsset* asset = AAssetManager_open(asset_manager, asset_path.c_str(), AASSET_MODE_BUFFER);
    if (asset == nullptr)
    {
        return false;
    }

    const off_t asset_size = AAsset_getLength(asset);
    if (asset_size <= 0)
    {
        AAsset_close(asset);
        return false;
    }

    font_png.resize(static_cast<size_t>(asset_size));
    const int read_size = AAsset_read(asset, font_png.data(), static_cast<size_t>(asset_size));
    AAsset_close(asset);
    return read_size == asset_size;
}
