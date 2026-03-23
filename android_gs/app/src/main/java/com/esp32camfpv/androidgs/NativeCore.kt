package com.esp32camfpv.androidgs

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

    init {
        System.loadLibrary("android_gs_core")
    }

    external fun getBuildInfo(): String
    external fun createHandle(gsDeviceId: Int = 1): Long
    external fun describeHandle(handle: Long): String
    external fun pushPacket(
        handle: Long,
        data: ByteArray,
        restoredByFec: Boolean = false,
        inputDbm: Int = 0
    ): Int
    external fun buildControlTransportPackets(handle: Long): Array<ByteArray>?
    external fun takeCompletedFrame(handle: Long): ByteArray?
    external fun getLastEventKind(handle: Long): Int
    external fun isMenuVisible(handle: Long): Boolean
    external fun setMenuVisible(handle: Long, visible: Boolean)
    external fun menuGoBack(handle: Long)
    external fun getMenuSelectedIndex(handle: Long): Int
    external fun getMenuTitle(handle: Long): String
    external fun getMenuItems(handle: Long): Array<String>
    external fun getMenuStatuses(handle: Long): Array<String>
    external fun getMenuStatusLines(handle: Long): Array<String>
    external fun menuSelectItem(handle: Long, itemIndex: Int)
    external fun consumeExitRequested(handle: Long): Boolean
    external fun resetSession(handle: Long)
    external fun destroyHandle(handle: Long)
}
