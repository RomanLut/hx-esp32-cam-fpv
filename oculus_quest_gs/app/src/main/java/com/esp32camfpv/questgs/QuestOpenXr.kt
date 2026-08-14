package com.esp32camfpv.questgs

import android.app.Activity
import android.util.Log
import com.esp32camfpv.gscommon.NativeCore

//===================================================================================
//===================================================================================
// Quest-only OpenXR entry points and loader bootstrap.
//
// These live outside the shared NativeCore because their JNI implementations exist only
// in the Quest build (openxr_quest_runtime.cpp). Keeping them in a Quest-owned class
// means the shared NativeCore declares no external function that the phone build cannot
// resolve.
object QuestOpenXr {

    private const val TAG = "QuestGs"

    // Set when libopenxr_loader.so could not be loaded at all; the native side logs and
    // reports the failure rather than silently running without a VR session.
    @Volatile
    @JvmStatic
    var openXrLoaderError: String? = null
        private set

    init {
        // Loader comes from the org.khronos.openxr:openxr_loader_for_android AAR. There is
        // deliberately no fallback loader: a second, differently-built loader masks real
        // failures and was the source of the runtime/ABI mismatch this replaced.
        //
        // This must run before android_gs_core is loaded, because android_gs_core carries a
        // DT_NEEDED on libopenxr_loader.so; loading it explicitly first turns a missing AAR
        // into a clear diagnostic instead of an opaque UnsatisfiedLinkError on the core lib.
        try {
            System.loadLibrary("openxr_loader")
            Log.i(TAG, "openxr_loader loaded")
        } catch (t: Throwable) {
            openXrLoaderError = "${t.javaClass.simpleName}: ${t.message}"
            Log.e(TAG, "FATAL: failed to load openxr_loader - VR session cannot start", t)
        }
        NativeCore.ensureLoaded()
    }

    //===================================================================================
    //===================================================================================
    // Loads the OpenXR loader and the GS core library in the required order. Call this
    // before any other NativeCore use so the loader is in place first.
    fun ensureLoaded() {
    }

    external fun startOpenXr(activity: Activity): Boolean
    external fun stopOpenXr()
    external fun isOpenXrFocused(): Boolean
}
