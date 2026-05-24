package com.esp32camfpv.androidgs

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

class RawBroadcastUsbController(
    private val activity: ComponentActivity,
    private val currentNativeHandle: () -> Long
) {
    private enum class ControllerState {
        IDLE,
        NO_HANDLE,
        NOT_RAW_TRANSPORT,
        NO_ADAPTER,
        WAITING_PERMISSION,
        RUNNING
    }

    private val usbManager =
        activity.applicationContext.getSystemService(Context.USB_SERVICE) as UsbManager

    private var syncJob: Job? = null
    private var receiverRegistered = false
    private var activeDeviceNames: Set<String> = emptySet()
    private var activeConnections: List<UsbDeviceConnection> = emptyList()
    private var lastState: ControllerState? = null
    private var usbTopologyChanged = false
    private var lastUsbTopologyRestartAtMs = 0L
    private var permissionRequestPendingDeviceName: String? = null
    private val permissionDeniedDeviceNames = mutableSetOf<String>()
    private val syncMutex = Mutex()

    private val receiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            when (intent?.action) {
                ACTION_USB_PERMISSION -> {
                    activity.lifecycleScope.launch(Dispatchers.Main) {
                        syncMutex.withLock {
                            permissionRequestPendingDeviceName = null
                            val permissionDevice =
                                intent.getUsbDevice() ?: findSupportedAdapters().firstOrNull()
                            if (intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)) {
                                permissionDevice?.deviceName?.let { permissionDeniedDeviceNames.remove(it) }
                            } else {
                                permissionDevice?.deviceName?.let { permissionDeniedDeviceNames.add(it) }
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
                            permissionRequestPendingDeviceName = null
                            permissionDeniedDeviceNames.clear()
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
            if (activeTransportKind != NativeCore.TRANSPORT_RAW_BROADCAST) {
                updateState(ControllerState.NOT_RAW_TRANSPORT, "Active transport is not RawBroadcast")
                stopCurrentAdapterSync(handle)
                return
            }

            val targetDevices = findSupportedAdapters()
            if (targetDevices.isEmpty()) {
                updateState(ControllerState.NO_ADAPTER, "No supported RTL adapter detected")
                permissionRequestPendingDeviceName = null
                permissionDeniedDeviceNames.clear()
                stopCurrentAdapterSync(handle)
                return
            }

            val permissionTarget = targetDevices.firstOrNull { device ->
                !usbManager.hasPermission(device) &&
                    device.deviceName !in permissionDeniedDeviceNames
            }
            if (permissionTarget != null) {
                updateState(
                    ControllerState.WAITING_PERMISSION,
                    "Waiting for USB permission for ${permissionTarget.deviceName}"
                )
                if (permissionRequestPendingDeviceName == null) {
                    permissionRequestPendingDeviceName = permissionTarget.deviceName
                    requestPermission(permissionTarget)
                }
                return
            }
            if (targetDevices.any { device -> !usbManager.hasPermission(device) }) {
                updateState(ControllerState.WAITING_PERMISSION, "USB permission denied")
                return
            }
            permissionRequestPendingDeviceName = null
            permissionDeniedDeviceNames.clear()

            val targetDeviceNames = targetDevices.map { device -> device.deviceName }.toSet()
            if (activeDeviceNames == targetDeviceNames) {
                val nativeAdapterCount = withContext(Dispatchers.Default) {
                    NativeCore.getRawBroadcastUsbAdapterCount(handle)
                }
                val nativeRunning = nativeAdapterCount == targetDeviceNames.size
                val nowMs = SystemClock.elapsedRealtime()
                val duplicateTopologyEvent =
                    usbTopologyChanged && nativeRunning &&
                        nowMs - lastUsbTopologyRestartAtMs < USB_TOPOLOGY_RESTART_DEBOUNCE_MS
                if (nativeRunning && (!usbTopologyChanged || duplicateTopologyEvent)) {
                    usbTopologyChanged = false
                    updateState(ControllerState.RUNNING, "Adapters already running on $targetDeviceNames")
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

            val startedConnections = mutableListOf<UsbDeviceConnection>()
            val startedDeviceNames = mutableSetOf<String>()
            for (targetDevice in targetDevices) {
                val connection = usbManager.openDevice(targetDevice)
                if (connection == null) {
                    Log.w(LOG_TAG, "Failed to open USB adapter ${targetDevice.deviceName}")
                    continue
                }

                val started = withContext(Dispatchers.Default) {
                    NativeCore.startRawBroadcastUsb(handle, connection.fileDescriptor)
                }
                if (!started) {
                    connection.close()
                    Log.w(LOG_TAG, "Native raw-broadcast start failed for ${targetDevice.deviceName}")
                    continue
                }
                startedConnections += connection
                startedDeviceNames += targetDevice.deviceName
            }

            if (startedConnections.isEmpty()) {
                return
            }

            activeConnections = startedConnections
            activeDeviceNames = startedDeviceNames
            updateState(ControllerState.RUNNING, "Started raw-broadcast adapters $startedDeviceNames")
        }
    }

    private fun findSupportedAdapters(): List<UsbDevice> {
        return usbManager.deviceList.values.filter { device ->
            RtlUsbDeviceAllowlist.isSupported(device)
        }.sortedBy { device -> device.deviceName }.take(MAX_RAW_BROADCAST_ADAPTERS)
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
        val oldConnections = activeConnections
        activeConnections = emptyList()
        activeDeviceNames = emptySet()
        if (handle != 0L) {
            withContext(Dispatchers.Default) {
                NativeCore.stopRawBroadcastUsb(handle)
            }
        }
        oldConnections.forEach { connection -> connection.close() }
    }

    private fun updateState(state: ControllerState, message: String) {
        if (lastState == state) {
            return
        }

        lastState = state
        Log.i(LOG_TAG, message)
    }

    private companion object {
        const val LOG_TAG = "RawBroadcastUsb"
        const val ACTION_USB_PERMISSION = "com.esp32camfpv.androidgs.USB_PERMISSION"
        const val MAX_RAW_BROADCAST_ADAPTERS = 2
        const val USB_TOPOLOGY_RESTART_DEBOUNCE_MS = 5_000L
    }
}
