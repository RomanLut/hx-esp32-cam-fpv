package com.esp32camfpv.androidgs

import android.Manifest
import android.app.Activity
import android.content.ContentValues
import android.content.pm.PackageManager
import android.content.res.AssetManager
import android.os.Build
import android.os.Environment
import android.provider.MediaStore
import android.util.Log
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

    @Volatile private var pendingRecordingUri: android.net.Uri? = null

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
            val fd = resolver.openFileDescriptor(uri, "rwt")?.detachFd() ?: run {
                resolver.delete(uri, null, null)
                return -1
            }
            pendingRecordingUri = uri
            return fd
        }

        @Suppress("InlinedApi")
        val values = ContentValues().apply {
            put(MediaStore.Video.Media.DISPLAY_NAME, filename)
            put(MediaStore.Video.Media.MIME_TYPE, "video/x-msvideo")
            put(MediaStore.Video.Media.RELATIVE_PATH, "Movies/esp32-cam-fpv")
            put(MediaStore.Video.Media.IS_PENDING, 1)
        }
        val resolver = activity.contentResolver
        val uri = resolver.insert(MediaStore.Video.Media.EXTERNAL_CONTENT_URI, values)
            ?: return -1
        val fd = resolver.openFileDescriptor(uri, "rwt")?.detachFd() ?: run {
            resolver.delete(uri, null, null)
            return -1
        }
        pendingRecordingUri = uri
        return fd
    }

    @JvmStatic
    fun finalizeRecordingFd() {
        val uri = pendingRecordingUri ?: return
        pendingRecordingUri = null
        val activity = activityRef?.get() ?: return
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
            return
        }
        try {
            @Suppress("InlinedApi")
            val values = ContentValues().apply {
                put(MediaStore.Video.Media.IS_PENDING, 0)
            }
            activity.contentResolver.update(uri, values, null, null)
        } catch (t: Throwable) {
            Log.w("QuestGs", "finalizeRecordingFd failed", t)
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
        try {
            System.loadLibrary("openxr_loader")
        } catch (t: Throwable) {
            Log.e("QuestGs", "Failed to load openxr_loader", t)
            try {
                System.loadLibrary("openxr_loader_no_khr_init")
            } catch (t2: Throwable) {
                Log.e("QuestGs", "Failed to load openxr_loader_no_khr_init", t2)
            }
        }
        System.loadLibrary("android_gs_core")
    }

    external fun getBuildInfo(): String
    external fun setAssetManager(assetManager: AssetManager)
    external fun setSettingsPath(path: String)
    external fun setRecordingsPath(path: String)
    external fun createHandle(gsDeviceId: Int = 1): Long
    external fun describeHandle(handle: Long): String
    external fun getActiveTransportKind(handle: Long): Int
    external fun isAirApfpvModeEnabled(handle: Long): Boolean
    external fun getPreferredApfpvCameraId(handle: Long): Int
    external fun setPreferredApfpvCameraId(handle: Long, deviceId: Int)
    external fun isApfpvMenuSearchActive(handle: Long): Boolean
    external fun consumeApfpvReconnectRequest(handle: Long): Boolean
    external fun consumeApfpvWifiScanPermissionPromptRequest(handle: Long): Boolean
    external fun setApfpvWifiScanPermissionError(handle: Long, enabled: Boolean)
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
    external fun getRawBroadcastUsbAdapterCount(handle: Long): Int
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
    external fun handleTouchDown(handle: Long, x: Float, y: Float, viewWidth: Float, viewHeight: Float): Boolean
    external fun handleKey(handle: Long, keyCode: Int): Boolean
    external fun setRenderSurface(handle: Long, surface: Surface?)
    external fun clearRenderSurface(handle: Long)
    external fun startOpenXr(activity: Activity): Boolean
    external fun stopOpenXr()
    external fun consumeExitRequested(handle: Long): Boolean

    @JvmStatic external fun serialTelemetryOnOpen()
    @JvmStatic external fun serialTelemetryOnClose()
    @JvmStatic external fun serialTelemetryOnBytes(data: ByteArray, length: Int)
    @JvmStatic external fun publishTelemetryUarts(uarts: Array<String>)
    @JvmStatic external fun getTelemetryUartSelection(): String

    @Volatile private var serialTelemetryWriter: ((ByteArray) -> Unit)? = null

    fun setSerialTelemetryWriter(writer: ((ByteArray) -> Unit)?) {
        serialTelemetryWriter = writer
    }

    // Called from C++ via JNI on the session thread to deliver outbound bytes
    // to the active UsbSerialPort owned by SerialTelemetryUsbController.
    @JvmStatic
    fun serialTelemetryWrite(data: ByteArray) {
        try {
            serialTelemetryWriter?.invoke(data)
        } catch (t: Throwable) {
            Log.w("QuestGs", "serialTelemetryWrite failed", t)
        }
    }
    external fun resetSession(handle: Long)
    external fun destroyHandle(handle: Long)
}
