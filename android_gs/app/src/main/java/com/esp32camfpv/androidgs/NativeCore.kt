package com.esp32camfpv.androidgs

import android.content.res.AssetManager
import android.view.Surface

object NativeCore {

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
    external fun setRendererScreenMode(handle: Long, screenMode: Int)
    external fun setRendererVrMode(handle: Long, enabled: Boolean)
    external fun setThermalStatus(handle: Long, thermalStatus: Int)
    external fun syncRendererOverlay(handle: Long, buildInfo: String)
    external fun handleTap(handle: Long, x: Float, y: Float, viewWidth: Float, viewHeight: Float)
    external fun handleKey(handle: Long, keyCode: Int): Boolean
    external fun setRenderSurface(handle: Long, surface: Surface)
    external fun clearRenderSurface(handle: Long)
    external fun consumeExitRequested(handle: Long): Boolean
    external fun resetSession(handle: Long)
    external fun destroyHandle(handle: Long)
}
