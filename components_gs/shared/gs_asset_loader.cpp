#include "gs_asset_loader.h"

#include <fstream>
#include <iterator>
#include <string>

#ifdef __ANDROID__
#include <android/asset_manager.h>
#include "android_jni_shared.h"
#endif

bool loadAssetJpeg(const char* name, std::vector<uint8_t>& out)
{
#ifdef __ANDROID__
    AAssetManager* mgr = androidGetAssetManager();
    if (mgr == nullptr) { return false; }
    AAsset* asset = AAssetManager_open(mgr, name, AASSET_MODE_BUFFER);
    if (asset == nullptr) { return false; }
    const off_t sz = AAsset_getLength(asset);
    out.resize(static_cast<size_t>(sz));
    const int n = AAsset_read(asset, out.data(), out.size());
    AAsset_close(asset);
    return n == static_cast<int>(sz);
#else
    const std::string path = std::string("../assets_gs/") + name;
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) { return false; }
    out.assign(std::istreambuf_iterator<char>(f), {});
    return !out.empty();
#endif
}
