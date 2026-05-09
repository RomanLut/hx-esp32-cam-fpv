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
    private var activeDeviceName: String? = null
    private var activeConnection: UsbDeviceConnection? = null
    private var lastState: ControllerState? = null
    private val syncMutex = Mutex()

    private val receiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            when (intent?.action) {
                ACTION_USB_PERMISSION,
                UsbManager.ACTION_USB_DEVICE_DETACHED,
                UsbManager.ACTION_USB_DEVICE_ATTACHED -> {
                    activity.lifecycleScope.launch(Dispatchers.Main) {
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

            val targetDevice = findSupportedAdapter()
            if (targetDevice == null) {
                updateState(ControllerState.NO_ADAPTER, "No supported RTL adapter detected")
                stopCurrentAdapterSync(handle)
                return
            }

            if (!usbManager.hasPermission(targetDevice)) {
                updateState(
                    ControllerState.WAITING_PERMISSION,
                    "Waiting for USB permission for ${targetDevice.deviceName}"
                )
                requestPermission(targetDevice)
                return
            }

            if (activeDeviceName == targetDevice.deviceName) {
                updateState(ControllerState.RUNNING, "Adapter already running on ${targetDevice.deviceName}")
                return
            }

            // USB attach/detach broadcasts and the periodic poll can arrive back-to-back.
            // Stop and start must stay in one serialized critical section or a stale stop can
            // tear down the adapter that a newer sync just started.
            stopCurrentAdapterSync(handle)

            val connection = usbManager.openDevice(targetDevice)
            if (connection == null) {
                Log.w(LOG_TAG, "Failed to open USB adapter ${targetDevice.deviceName}")
                return
            }

            val started = withContext(Dispatchers.Default) {
                NativeCore.startRawBroadcastUsb(handle, connection.fileDescriptor)
            }
            if (!started) {
                connection.close()
                Log.w(LOG_TAG, "Native raw-broadcast start failed for ${targetDevice.deviceName}")
                return
            }

            activeConnection = connection
            activeDeviceName = targetDevice.deviceName
            updateState(ControllerState.RUNNING, "Started raw-broadcast adapter ${targetDevice.deviceName}")
            Log.i(LOG_TAG, "Started raw-broadcast USB adapter ${targetDevice.deviceName}")
        }
    }

    private fun findSupportedAdapter(): UsbDevice? {
        return usbManager.deviceList.values.firstOrNull { device ->
            device.vendorId == RTL_VENDOR_ID
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

    private suspend fun stopCurrentAdapterSync(handle: Long = currentNativeHandle()) {
        val oldConnection = activeConnection
        activeConnection = null
        activeDeviceName = null
        if (handle != 0L) {
            withContext(Dispatchers.Default) {
                NativeCore.stopRawBroadcastUsb(handle)
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
        const val LOG_TAG = "RawBroadcastUsb"
        const val ACTION_USB_PERMISSION = "com.esp32camfpv.androidgs.USB_PERMISSION"
        const val RTL_VENDOR_ID = 0x0BDA
    }
}
