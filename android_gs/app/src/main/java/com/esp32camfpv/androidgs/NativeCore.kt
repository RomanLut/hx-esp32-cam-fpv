package com.esp32camfpv.androidgs

import android.Manifest
import android.app.Activity
import android.content.ContentValues
import android.content.pm.PackageManager
import android.content.res.AssetManager
import android.os.Build
import android.os.Environment
import android.provider.MediaStore
import android.view.Surface
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import java.io.File
import java.lang.ref.WeakReference

object NativeCore {

    @Volatile private var activityRef: WeakReference<Activity>? = null

    fun setActivity(activity: Activity?) {
        activityRef = if (activity != null) WeakReference(activity) else null
    }

    @JvmStatic
    fun createRecordingFd(filename: String): Int {
        val activity = activityRef?.get() ?: return -1

        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
            if (ContextCompat.checkSelfPermission(activity, Manifest.permission.WRITE_EXTERNAL_STORAGE)
                != PackageManager.PERMISSION_GRANTED) {
                activity.runOnUiThread {
                    ActivityCompat.requestPermissions(
                        activity, arrayOf(Manifest.permission.WRITE_EXTERNAL_STORAGE), 0)
                }
                return -1
            }
            val dir = File(
                Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_MOVIES),
                "esp32-cam-fpv"
            )
            dir.mkdirs()
            @Suppress("DEPRECATION")
            val values = ContentValues().apply {
                put(MediaStore.Video.Media.DISPLAY_NAME, filename)
                put(MediaStore.Video.Media.MIME_TYPE, "video/x-msvideo")
                put(MediaStore.Video.Media.DATA, File(dir, filename).absolutePath)
            }
            val resolver = activity.contentResolver
            val uri = resolver.insert(MediaStore.Video.Media.EXTERNAL_CONTENT_URI, values)
                ?: return -1
            return resolver.openFileDescriptor(uri, "rwt")?.detachFd() ?: run {
                resolver.delete(uri, null, null)
                -1
            }
        }

        @Suppress("InlinedApi")
        val values = ContentValues().apply {
            put(MediaStore.Video.Media.DISPLAY_NAME, filename)
            put(MediaStore.Video.Media.MIME_TYPE, "video/x-msvideo")
            put(MediaStore.Video.Media.RELATIVE_PATH, "Movies/esp32-cam-fpv")
        }
        val resolver = activity.contentResolver
        val uri = resolver.insert(MediaStore.Video.Media.EXTERNAL_CONTENT_URI, values)
            ?: return -1
        return resolver.openFileDescriptor(uri, "rwt")?.detachFd() ?: run {
            resolver.delete(uri, null, null)
            -1
        }
    }

    const val EVENT_IGNORE = 0
    const val EVENT_CONNECT_ACCEPTED = 1
    const val EVENT_CONFIG_RECEIVED = 2
    const val EVENT_VIDEO_PACKET = 3
    const val EVENT_TELEMETRY_PAYLOAD = 4
    const val EVENT_OSD_UPDATE = 5
    const val EVENT_INVALID_VIDEO_PACKET = 6
    const val EVENT_INVALID_TELEMETRY_PACKET = 7
    const val EVENT_INVALID_OSD_PACKET = 8
    const val EVENT_UNSUPPORTED_PACKET = 9

    const val TRANSPORT_RAW_BROADCAST = 0
    const val TRANSPORT_APFPV = 1
    const val TRANSPORT_TEST = 2
    const val TRANSPORT_WIFI_SCAN = 3

    init {
        System.loadLibrary("android_gs_core")
    }

    external fun getBuildInfo(): String
    external fun setAssetManager(assetManager: AssetManager)
    external fun setSettingsPath(path: String)
    external fun setRecordingsPath(path: String)
    external fun createHandle(gsDeviceId: Int = 1): Long
    external fun describeHandle(handle: Long): String
    external fun getActiveTransportKind(handle: Long): Int
    external fun getPreferredApfpvCameraId(handle: Long): Int
    external fun setPreferredApfpvCameraId(handle: Long, deviceId: Int)
    external fun isApfpvMenuSearchActive(handle: Long): Boolean
    external fun consumeApfpvReconnectRequest(handle: Long): Boolean
    external fun hasSeenApfpvUdpPackets(handle: Long): Boolean
    external fun syncApfpvCameraState(handle: Long, discoveredSsids: Array<String>, activeSsid: String?, gsRssiDbm: Int, connectingSsid: String?)
    external fun startUdpClient(
        handle: Long,
        peerHost: String = "192.168.4.1",
        peerPort: Int = 5600,
        localPort: Int = 5600
    ): Boolean
    external fun stopUdpClient(handle: Long)
    external fun isUdpClientRunning(handle: Long): Boolean
    external fun startRawBroadcastUsb(handle: Long, fd: Int): Boolean
    external fun stopRawBroadcastUsb(handle: Long)
    external fun isRawBroadcastUsbRunning(handle: Long): Boolean
    external fun startWifiScanUsb(handle: Long, fd: Int): Boolean
    external fun stopWifiScanUsb(handle: Long)
    external fun isWifiScanUsbRunning(handle: Long): Boolean
    external fun setVideoUdpOutput(handle: Long, addr: String, port: Int): Boolean
    external fun getLastEventKind(handle: Long): Int
    external fun getScreenAspectRatio(handle: Long): Int
    external fun isVrModeEnabled(handle: Long): Boolean
    external fun isScreenFlipVEnabled(handle: Long): Boolean
    external fun setRendererScreenMode(handle: Long, screenMode: Int)
    external fun setRendererVrMode(handle: Long, enabled: Boolean)
    external fun setThermalStatus(handle: Long, thermalStatus: Int)
    external fun setBatteryPercent(handle: Long, batteryPercent: Int)
    external fun syncRendererOverlay(handle: Long, buildInfo: String)
    external fun handleTap(handle: Long, x: Float, y: Float, viewWidth: Float, viewHeight: Float)
    external fun handleKey(handle: Long, keyCode: Int): Boolean
    external fun setRenderSurface(handle: Long, surface: Surface)
    external fun clearRenderSurface(handle: Long)
    external fun consumeExitRequested(handle: Long): Boolean
    external fun resetSession(handle: Long)
    external fun destroyHandle(handle: Long)
}
