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
import android.util.Log
import androidx.activity.ComponentActivity
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.CancellationException
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
                        syncNowSafely()
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
                        syncNowSafely()
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
                syncNowSafely()
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
            syncNowSafely()
        }
    }

    private suspend fun syncNowSafely() {
        try {
            syncNow()
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (error: Throwable) {
            // USB detach races can invalidate Android UsbDevice objects between discovery and
            // openDevice(). One failed reconciliation must not cancel the lifecycle coroutine;
            // the next periodic pass must remain able to recover both adapters.
            Log.e(LOG_TAG, "USB reconciliation failed; polling will continue", error)
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
            Log.i(
                LOG_TAG,
                "Sync target=${targetDevices.map { it.deviceName }} " +
                    "active=$activeDeviceNames permission=" +
                    targetDevices.associate { it.deviceName to usbManager.hasPermission(it) } +
                    " pending=$permissionRequestPendingDeviceName topologyChanged=$usbTopologyChanged"
            )
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
                if (nativeRunning) {
                    // Attach broadcasts can arrive after the periodic pass already reconciled
                    // this exact device set. Matching Java names plus matching native count is
                    // authoritative; restarting solely because a late topology flag is set can
                    // stop an adapter while Devourer Init() is still entering its RX loop.
                    usbTopologyChanged = false
                    updateState(ControllerState.RUNNING, "Adapters already running on $targetDeviceNames")
                    return
                }
                stopCurrentAdapterSync(handle)
            } else if (!targetDeviceNames.containsAll(activeDeviceNames)) {
                // USB attach/detach broadcasts and the periodic poll can arrive back-to-back.
                // Stop and start must stay in one serialized critical section or a stale stop can
                // tear down the adapter that a newer sync just started.
                stopCurrentAdapterSync(handle)
            } else if (activeDeviceNames.isNotEmpty()) {
                val nativeAdapterCount = withContext(Dispatchers.Default) {
                    NativeCore.getRawBroadcastUsbAdapterCount(handle)
                }
                if (nativeAdapterCount != activeDeviceNames.size) {
                    // A native adapter failed after Java opened it. Rebuild the known set once;
                    // otherwise Java would keep a stale connection and never restart that device.
                    stopCurrentAdapterSync(handle)
                }
            }
            usbTopologyChanged = false

            // A bad second adapter must not tear down a healthy first adapter on every poll.
            // Preserve working connections and retry only devices missing from the native set.
            val startedConnections = activeConnections.toMutableList()
            val startedDeviceNames = activeDeviceNames.toMutableSet()
            for (targetDevice in targetDevices.filter { it.deviceName !in startedDeviceNames }) {
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
        // This action must not match SerialTelemetryUsbController. Android identifies the
        // permission PendingIntent by action/request code, and sharing it lets the serial and
        // raw controllers receive or reuse each other's hot-plug permission result. On a hub
        // replug that commonly starts the first RTL adapter while the second remains unopened.
        const val ACTION_USB_PERMISSION =
            "com.esp32camfpv.questgs.RAW_BROADCAST_USB_PERMISSION"
        const val MAX_RAW_BROADCAST_ADAPTERS = 2
    }
}
