package com.esp32camfpv.questgs

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbManager
import android.os.Build
import android.os.SystemClock
import android.util.Log
import androidx.activity.ComponentActivity
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext

class WifiScanUsbController(
    private val activity: ComponentActivity,
    private val currentNativeHandle: () -> Long
) {
    private enum class ControllerState {
        IDLE,
        NO_HANDLE,
        NOT_SCAN_TRANSPORT,
        NO_ADAPTER,
        WAITING_PERMISSION,
        RUNNING
    }

    private val usbManager =
        activity.applicationContext.getSystemService(Context.USB_SERVICE) as UsbManager

    private var syncJob: Job? = null
    private var receiverRegistered = false
    private var activeDeviceName: String? = null
    private var activeConnection: UsbDeviceConnection? = null
    private var lastState: ControllerState? = null
    private var usbTopologyChanged = false
    private var lastUsbTopologyRestartAtMs = 0L
    private var permissionRequestPending = false
    private var permissionDeniedDeviceName: String? = null
    private val syncMutex = Mutex()

    private val receiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            when (intent?.action) {
                ACTION_USB_PERMISSION -> {
                    activity.lifecycleScope.launch(Dispatchers.Main) {
                        syncMutex.withLock {
                            permissionRequestPending = false
                            permissionDeniedDeviceName =
                                if (intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)) {
                                    null
                                } else {
                                    intent.getUsbDevice()?.deviceName ?: findSupportedAdapter()?.deviceName
                                }
                            usbTopologyChanged = true
                        }
                        syncNow()
                    }
                }

                UsbManager.ACTION_USB_DEVICE_DETACHED,
                UsbManager.ACTION_USB_DEVICE_ATTACHED -> {
                    activity.lifecycleScope.launch(Dispatchers.Main) {
                        syncMutex.withLock {
                            permissionRequestPending = false
                            permissionDeniedDeviceName = null
                            usbTopologyChanged = true
                        }
                        syncNow()
                    }
                }
            }
        }
    }

    fun start() {
        if (!receiverRegistered) {
            val filter = IntentFilter().apply {
                addAction(ACTION_USB_PERMISSION)
                addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED)
                addAction(UsbManager.ACTION_USB_DEVICE_DETACHED)
            }
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                activity.registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED)
            } else {
                @Suppress("DEPRECATION")
                activity.registerReceiver(receiver, filter)
            }
            receiverRegistered = true
        }

        if (syncJob != null) {
            return
        }

        syncJob = activity.lifecycleScope.launch(Dispatchers.Main) {
            while (true) {
                syncNow()
                delay(3_000L)
            }
        }
    }

    fun stop() {
        syncJob?.cancel()
        syncJob = null
        activity.lifecycleScope.launch(Dispatchers.Main) {
            syncMutex.withLock {
                stopCurrentAdapterSync()
            }
        }
        if (receiverRegistered) {
            activity.unregisterReceiver(receiver)
            receiverRegistered = false
        }
    }

    fun handleUsbTopologyChanged() {
        activity.lifecycleScope.launch(Dispatchers.Main) {
            syncMutex.withLock {
                usbTopologyChanged = true
            }
            syncNow()
        }
    }

    private suspend fun syncNow() {
        syncMutex.withLock {
            val handle = currentNativeHandle()
            if (handle == 0L) {
                updateState(ControllerState.NO_HANDLE, "No native handle yet")
                stopCurrentAdapterSync()
                return
            }

            val activeTransportKind = withContext(Dispatchers.Default) {
                NativeCore.getActiveTransportKind(handle)
            }
            if (activeTransportKind != NativeCore.TRANSPORT_WIFI_SCAN) {
                updateState(ControllerState.NOT_SCAN_TRANSPORT, "Active transport is not WifiChannelScan")
                stopCurrentAdapterSync(handle)
                return
            }

            val targetDevice = findSupportedAdapter()
            if (targetDevice == null) {
                updateState(ControllerState.NO_ADAPTER, "No supported RTL adapter detected")
                permissionRequestPending = false
                permissionDeniedDeviceName = null
                stopCurrentAdapterSync(handle)
                return
            }

            if (!usbManager.hasPermission(targetDevice)) {
                updateState(
                    ControllerState.WAITING_PERMISSION,
                    "Waiting for USB permission for ${targetDevice.deviceName}"
                )
                if (!permissionRequestPending &&
                    permissionDeniedDeviceName != targetDevice.deviceName
                ) {
                    permissionRequestPending = true
                    requestPermission(targetDevice)
                }
                return
            }
            permissionRequestPending = false
            permissionDeniedDeviceName = null

            if (activeDeviceName == targetDevice.deviceName) {
                val nativeRunning = withContext(Dispatchers.Default) {
                    NativeCore.isWifiScanUsbRunning(handle)
                }
                val nowMs = SystemClock.elapsedRealtime()
                val duplicateTopologyEvent =
                    usbTopologyChanged && nativeRunning &&
                        nowMs - lastUsbTopologyRestartAtMs < USB_TOPOLOGY_RESTART_DEBOUNCE_MS
                if (nativeRunning && (!usbTopologyChanged || duplicateTopologyEvent)) {
                    usbTopologyChanged = false
                    updateState(ControllerState.RUNNING, "Adapter already running on ${targetDevice.deviceName}")
                    return
                }

                if (usbTopologyChanged) {
                    lastUsbTopologyRestartAtMs = nowMs
                }
                stopCurrentAdapterSync(handle)
            } else {
                // USB attach/detach broadcasts and the periodic poll can arrive back-to-back.
                // Stop and start must stay in one serialized critical section or a stale stop can
                // tear down the adapter that a newer sync just started.
                if (usbTopologyChanged) {
                    lastUsbTopologyRestartAtMs = SystemClock.elapsedRealtime()
                }
                stopCurrentAdapterSync(handle)
            }
            usbTopologyChanged = false

            val connection = usbManager.openDevice(targetDevice)
            if (connection == null) {
                Log.w(LOG_TAG, "Failed to open USB adapter ${targetDevice.deviceName}")
                return
            }

            val started = withContext(Dispatchers.Default) {
                NativeCore.startWifiScanUsb(handle, connection.fileDescriptor)
            }
            if (!started) {
                connection.close()
                Log.w(LOG_TAG, "Native wifi-scan start failed for ${targetDevice.deviceName}")
                return
            }

            activeConnection = connection
            activeDeviceName = targetDevice.deviceName
            updateState(ControllerState.RUNNING, "Started wifi-scan adapter ${targetDevice.deviceName}")
        }
    }

    private fun findSupportedAdapter(): UsbDevice? {
        return usbManager.deviceList.values.firstOrNull { device ->
            RtlUsbDeviceAllowlist.isSupported(device)
        }
    }

    private fun requestPermission(device: UsbDevice) {
        val pendingIntent = PendingIntent.getBroadcast(
            activity,
            0,
            Intent(ACTION_USB_PERMISSION),
            PendingIntent.FLAG_IMMUTABLE
        )
        usbManager.requestPermission(device, pendingIntent)
    }

    @Suppress("DEPRECATION")
    private fun Intent.getUsbDevice(): UsbDevice? {
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            getParcelableExtra(UsbManager.EXTRA_DEVICE, UsbDevice::class.java)
        } else {
            getParcelableExtra(UsbManager.EXTRA_DEVICE)
        }
    }

    private suspend fun stopCurrentAdapterSync(handle: Long = currentNativeHandle()) {
        val oldConnection = activeConnection
        activeConnection = null
        activeDeviceName = null
        if (handle != 0L) {
            withContext(Dispatchers.Default) {
                NativeCore.stopWifiScanUsb(handle)
            }
        }
        oldConnection?.close()
    }

    private fun updateState(state: ControllerState, message: String) {
        if (lastState == state) {
            return
        }

        lastState = state
        Log.i(LOG_TAG, message)
    }

    private companion object {
        const val LOG_TAG = "WifiScanUsb"
        const val ACTION_USB_PERMISSION = "com.esp32camfpv.questgs.WIFI_SCAN_USB_PERMISSION"
        const val USB_TOPOLOGY_RESTART_DEBOUNCE_MS = 5_000L
    }
}
