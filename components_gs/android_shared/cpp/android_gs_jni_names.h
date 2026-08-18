#pragma once

//===================================================================================
//===================================================================================
// Single source of truth for the JNI names of the shared Android-family GS classes.
//
// android_gs and oculus_quest_gs are separate applications with different
// applicationIds, but they compile the same native bridge. JNI export symbols and
// FindClass paths encode the *Java package*, not the applicationId, so the shared
// Kotlin classes deliberately live in the neutral com.esp32camfpv.gscommon package
// (see NativeCore.kt). That is what lets one native_bridge implementation serve both
// apps instead of one per-app copy.
//
// Do not reintroduce per-app #ifdef ladders here.

#define ANDROID_GS_JAVA_PACKAGE_PATH "com/esp32camfpv/gscommon"

#define ANDROID_GS_NATIVE_CORE_CLASS ANDROID_GS_JAVA_PACKAGE_PATH "/NativeCore"

#define ANDROID_GS_BITMAP_DECODE_BRIDGE_CLASS \
    ANDROID_GS_JAVA_PACKAGE_PATH "/BitmapDecodeBridge"
#define ANDROID_GS_BITMAP_DECODE_RESULT_CLASS \
    ANDROID_GS_BITMAP_DECODE_BRIDGE_CLASS "$Result"
#define ANDROID_GS_BITMAP_DECODE_RESULT_SIGNATURE \
    "([B)L" ANDROID_GS_BITMAP_DECODE_RESULT_CLASS ";"

// Expands to the exported JNI symbol for one NativeCore method.
#define ANDROID_GS_JNI(name) Java_com_esp32camfpv_gscommon_NativeCore_##name
