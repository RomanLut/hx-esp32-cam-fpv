#pragma once

#include <android/asset_manager.h>
#include <jni.h>

JavaVM* androidGetJavaVm();
AAssetManager* androidGetAssetManager();
void androidSetAssetManager(AAssetManager* asset_manager);
