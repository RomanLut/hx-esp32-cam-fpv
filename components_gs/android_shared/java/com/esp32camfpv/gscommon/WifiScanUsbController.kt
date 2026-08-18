package com.esp32camfpv.gscommon

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
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext

//===================================================================================
//===================================================================================
// Owns the RTL scan adapter and its serialized USB permission flow.
//
// requestVrFocusRecovery is a no-op on the phone build; the Quest build uses it to
// reclaim VR focus after the system permission dialog has taken it away.
class WifiScanUsbController(
    private val activity: ComponentActivity,
    private val currentNativeHandle: () -> Long,
    private val requestVrFocusRecovery: (String) -> Unit = {}
)
{
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

    // Scoped to the installed package so two GS apps on one device cannot observe each
    // other's permission broadcasts.
    private val actionUsbPermission =
        "${activity.applicationContext.packageName}.WIFI_SCAN_USB_PERMISSION"

    private var syncJob: Job? = null
    private var receiverRegistered = false
    private var activeDeviceName: String? = null
    private var activeConnection: UsbDeviceConnection? = null
    private var lastState: ControllerState? = null
    private var usbTopologyChanged = false
    private var usbDetachGeneration = 0L
    private var reconciledUsbDetachGeneration = 0L
    private var lastUsbTopologyRestartAtMs = 0L
    private var permissionRequestPendingDeviceName: String? = null
    private var permissionDeniedDeviceName: String? = null
    private val syncMutex = Mutex()

    private val receiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            when (intent?.action) {
                actionUsbPermission -> {
                    activity.lifecycleScope.launch(Dispatchers.Main) {
                        syncMutex.withLock {
                            UsbPermissionRequestCoordinator.release(PERMISSION_OWNER)
                            permissionRequestPendingDeviceName = null
                            permissionDeniedDeviceName =
                                if (intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)) {
                                    null
                                } else {
                                    intent.getUsbDevice()?.deviceName ?: findSupportedAdapter()?.deviceName
                                }
                            usbTopologyChanged = true
                        }
                        requestVrFocusRecovery("wifiScanUsbPermissionResult")
                        syncNowSafely()
                    }
                }

                UsbManager.ACTION_USB_DEVICE_DETACHED -> {
                    activity.lifecycleScope.launch(Dispatchers.Main) {
                        syncMutex.withLock {
                            // Only the device the dialog is actually about may cancel the
                            // in-flight request. Another child detaching on the same hub must
                            // not erase a permission request the user has not answered yet.
                            val detachedDeviceName = intent.getUsbDevice()?.deviceName
                            if (permissionRequestPendingDeviceName == detachedDeviceName) {
                                UsbPermissionRequestCoordinator.release(PERMISSION_OWNER)
                                permissionRequestPendingDeviceName = null
                            }
                            if (permissionDeniedDeviceName == detachedDeviceName) {
                                permissionDeniedDeviceName = null
                            }
                            usbTopologyChanged = true
                            usbDetachGeneration++
                        }
                        syncNowSafely()
                    }
                }

                UsbManager.ACTION_USB_DEVICE_ATTACHED -> {
                    activity.lifecycleScope.launch(Dispatchers.Main) {
                        syncMutex.withLock {
                            // Hub children attach independently. Do not cancel a permission
                            // dialog already in flight for the selected RTL adapter.
                            if (permissionDeniedDeviceName == intent.getUsbDevice()?.deviceName) {
                                permissionDeniedDeviceName = null
                            }
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
                addAction(actionUsbPermission)
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
        UsbPermissionRequestCoordinator.release(PERMISSION_OWNER)
        permissionRequestPendingDeviceName = null
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

    private suspend fun syncNowSafely()
    {
        try
        {
            syncNow()
        }
        catch (cancelled: CancellationException)
        {
            throw cancelled
        }
        catch (error: Throwable)
        {
            // USB detach can invalidate the device between discovery and native startup.
            // Keep the periodic reconciler alive so scan mode recovers after replug.
            Log.e(LOG_TAG, "Wifi-scan USB reconciliation failed; polling will continue", error)
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
                UsbPermissionRequestCoordinator.release(PERMISSION_OWNER)
                permissionRequestPendingDeviceName = null
                updateState(ControllerState.NOT_SCAN_TRANSPORT, "Active transport is not WifiChannelScan")
                stopCurrentAdapterSync(handle)
                return
            }

            val targetDevice = findSupportedAdapter()
            if (targetDevice == null) {
                UsbPermissionRequestCoordinator.release(PERMISSION_OWNER)
                updateState(ControllerState.NO_ADAPTER, "No supported RTL adapter detected")
                permissionRequestPendingDeviceName = null
                permissionDeniedDeviceName = null
                stopCurrentAdapterSync(handle)
                reconciledUsbDetachGeneration = usbDetachGeneration
                return
            }

            if (!usbManager.hasPermission(targetDevice)) {
                if (reconciledUsbDetachGeneration != usbDetachGeneration &&
                    activeDeviceName != null
                ) {
                    stopCurrentAdapterSync(handle)
                }
                updateState(
                    ControllerState.WAITING_PERMISSION,
                    "Waiting for USB permission for ${targetDevice.deviceName}"
                )
                if (permissionRequestPendingDeviceName == null &&
                    permissionDeniedDeviceName != targetDevice.deviceName &&
                    UsbPermissionRequestCoordinator.tryAcquire(
                        PERMISSION_OWNER,
                        targetDevice.deviceName
                    )
                ) {
                    permissionRequestPendingDeviceName = targetDevice.deviceName
                    requestPermission(targetDevice)
                }
                return
            }
            UsbPermissionRequestCoordinator.release(PERMISSION_OWNER)
            permissionRequestPendingDeviceName = null
            permissionDeniedDeviceName = null

            if (activeDeviceName == targetDevice.deviceName) {
                val nativeRunning = withContext(Dispatchers.Default) {
                    NativeCore.isWifiScanUsbRunning(handle)
                }
                val nowMs = SystemClock.elapsedRealtime()
                val duplicateTopologyEvent =
                    usbTopologyChanged && nativeRunning &&
                        nowMs - lastUsbTopologyRestartAtMs < USB_TOPOLOGY_RESTART_DEBOUNCE_MS
                val currentDetachGeneration =
                    reconciledUsbDetachGeneration == usbDetachGeneration
                if (nativeRunning && currentDetachGeneration &&
                    (!usbTopologyChanged || duplicateTopologyEvent)
                ) {
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
            reconciledUsbDetachGeneration = usbDetachGeneration
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
            Intent(actionUsbPermission),
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
        const val PERMISSION_OWNER = "wifi-scan"
        const val USB_TOPOLOGY_RESTART_DEBOUNCE_MS = 5_000L
    }
}
