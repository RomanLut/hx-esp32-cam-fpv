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
// Owns the RTL raw-broadcast adapters and their serialized USB permission flow.
//
// Two platform behaviours are injected rather than branched on:
//  - requestVrFocusRecovery reclaims VR input after a system permission dialog. It is a
//    no-op on the phone build.
//  - isSystemUiFocused, when non-null, enables the focus-transition fallback used to
//    detect a permission dialog that closed without delivering its result broadcast.
//    Pass null on platforms that deliver the broadcast reliably, which keeps the
//    fallback fully inert there.
class RawBroadcastUsbController(
    private val activity: ComponentActivity,
    private val currentNativeHandle: () -> Long,
    private val requestVrFocusRecovery: (String) -> Unit = {},
    private val isSystemUiFocused: (() -> Boolean)? = null
)
{
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

    // This action must not match SerialTelemetryUsbController. Android identifies the
    // permission PendingIntent by action/request code, and sharing it lets the serial and
    // raw controllers receive or reuse each other's hot-plug permission result. On a hub
    // replug that commonly starts the first RTL adapter while the second remains unopened.
    // Scoped to the installed package so two GS apps on one device stay isolated.
    private val actionUsbPermission =
        "${activity.applicationContext.packageName}.RAW_BROADCAST_USB_PERMISSION"

    private var syncJob: Job? = null
    private var receiverRegistered = false
    private var activeDeviceNames: Set<String> = emptySet()
    private var activeConnections: List<UsbDeviceConnection> = emptyList()
    private var lastState: ControllerState? = null
    private var usbTopologyChanged = false
    private var usbDetachGeneration = 0L
    private var reconciledUsbDetachGeneration = 0L
    private var permissionRequestPendingDeviceName: String? = null
    private var permissionRequestObservedFocusLoss = false
    private val permissionDeniedDeviceNames = mutableSetOf<String>()
    private var permissionFlowNeedsFocusRecovery = false
    private val syncMutex = Mutex()

    private val receiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            when (intent?.action) {
                actionUsbPermission -> {
                    activity.lifecycleScope.launch(Dispatchers.Main) {
                        syncMutex.withLock {
                            UsbPermissionRequestCoordinator.release(PERMISSION_OWNER)
                            permissionRequestPendingDeviceName = null
                            permissionRequestObservedFocusLoss = false
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
                                permissionRequestObservedFocusLoss = false
                            }
                            detachedDeviceName?.let { permissionDeniedDeviceNames.remove(it) }
                            usbTopologyChanged = true
                            usbDetachGeneration++
                        }
                        syncNowSafely(allowPermissionRequest = false)
                    }
                }

                UsbManager.ACTION_USB_DEVICE_ATTACHED -> {
                    activity.lifecycleScope.launch(Dispatchers.Main) {
                        syncMutex.withLock {
                            // A hub enumerates its children one by one. An unrelated attach must
                            // not erase the permission request already displayed for an earlier
                            // child, or its result can no longer advance/recover the workflow.
                            intent.getUsbDevice()?.deviceName?.let {
                                permissionDeniedDeviceNames.remove(it)
                            }
                            usbTopologyChanged = true
                        }
                        syncNowSafely(allowPermissionRequest = false)
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
                syncNowSafely(allowPermissionRequest = true)
                delay(3_000L)
            }
        }
    }

    fun stop() {
        UsbPermissionRequestCoordinator.release(PERMISSION_OWNER)
        permissionRequestPendingDeviceName = null
        permissionRequestObservedFocusLoss = false
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
            // A hub reports its children as separate attach intents. Reconcile native handles
            // immediately, but let the periodic pass request permission only after that attach
            // burst has completed; opening a system dialog while the hub is still enumerating
            // can close it without delivering the requested device's permission result.
            syncNowSafely(allowPermissionRequest = false)
        }
    }

    private suspend fun syncNowSafely(allowPermissionRequest: Boolean = true) {
        try {
            syncNow(allowPermissionRequest)
            recoverVrFocusIfPermissionFlowSettled()
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (error: Throwable) {
            // USB detach races can invalidate Android UsbDevice objects between discovery and
            // openDevice(). One failed reconciliation must not cancel the lifecycle coroutine;
            // the next periodic pass must remain able to recover both adapters.
            Log.e(LOG_TAG, "USB reconciliation failed; polling will continue", error)
        }
    }

    private suspend fun syncNow(allowPermissionRequest: Boolean) {
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
                UsbPermissionRequestCoordinator.release(PERMISSION_OWNER)
                permissionRequestPendingDeviceName = null
                permissionRequestObservedFocusLoss = false
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
                UsbPermissionRequestCoordinator.release(PERMISSION_OWNER)
                updateState(ControllerState.NO_ADAPTER, "No supported RTL adapter detected")
                permissionRequestPendingDeviceName = null
                permissionRequestObservedFocusLoss = false
                permissionDeniedDeviceNames.clear()
                stopCurrentAdapterSync(handle)
                reconciledUsbDetachGeneration = usbDetachGeneration
                return
            }

            val focusProbe = isSystemUiFocused
            val pendingPermissionDevice = permissionRequestPendingDeviceName?.let { pendingName ->
                targetDevices.firstOrNull { device -> device.deviceName == pendingName }
            }
            if (pendingPermissionDevice != null && usbManager.hasPermission(pendingPermissionDevice)) {
                // UsbManager is authoritative once the grant is visible. Quest can leave and
                // restore OpenXR focus entirely between two polling passes, so requiring this
                // loop to sample the transient focus loss can strand an already granted device
                // as pending forever and block permission for every later adapter on the hub.
                permissionDeniedDeviceNames.remove(pendingPermissionDevice.deviceName)
                UsbPermissionRequestCoordinator.release(PERMISSION_OWNER)
                permissionRequestPendingDeviceName = null
                permissionRequestObservedFocusLoss = false
            } else if (pendingPermissionDevice != null && focusProbe != null && !focusProbe()) {
                permissionRequestObservedFocusLoss = true
            }
            if (pendingPermissionDevice != null &&
                permissionRequestObservedFocusLoss &&
                focusProbe != null &&
                focusProbe()
            ) {
                // Quest can close UsbPermissionActivity without delivering our PendingIntent
                // broadcast. Activity onResume is not usable because Horizon invokes it while
                // its panel is still visible. OpenXR must first leave FOCUSED and later return;
                // the pre-dialog FOCUSED transition can otherwise be mistaken for completion.
                // UsbManager then tells us whether the request was granted.
                if (usbManager.hasPermission(pendingPermissionDevice)) {
                    permissionDeniedDeviceNames.remove(pendingPermissionDevice.deviceName)
                } else {
                    permissionDeniedDeviceNames.add(pendingPermissionDevice.deviceName)
                }
                UsbPermissionRequestCoordinator.release(PERMISSION_OWNER)
                permissionRequestPendingDeviceName = null
                permissionRequestObservedFocusLoss = false
            }

            val permissionTarget = targetDevices.firstOrNull { device ->
                !usbManager.hasPermission(device) &&
                    device.deviceName !in permissionDeniedDeviceNames
            }
            if (permissionTarget != null) {
                if (reconciledUsbDetachGeneration != usbDetachGeneration &&
                    activeDeviceNames.isNotEmpty()
                ) {
                    // A fast hub replug can reuse every device name before this pass runs.
                    // Release native objects backed by the detached generation before waiting
                    // for permission on the replacement hardware.
                    stopCurrentAdapterSync(handle)
                }
                updateState(
                    ControllerState.WAITING_PERMISSION,
                    "Waiting for USB permission for ${permissionTarget.deviceName}"
                )
                if (allowPermissionRequest &&
                    permissionRequestPendingDeviceName == null &&
                    UsbPermissionRequestCoordinator.tryAcquire(
                        PERMISSION_OWNER,
                        permissionTarget.deviceName
                    )
                ) {
                    permissionRequestPendingDeviceName = permissionTarget.deviceName
                    permissionRequestObservedFocusLoss = false
                    permissionFlowNeedsFocusRecovery = true
                    requestPermission(permissionTarget)
                }
                return
            }
            val permittedTargetDevices = targetDevices.filter { device ->
                usbManager.hasPermission(device)
            }
            if (permittedTargetDevices.isEmpty()) {
                if (reconciledUsbDetachGeneration != usbDetachGeneration &&
                    activeDeviceNames.isNotEmpty()
                ) {
                    stopCurrentAdapterSync(handle)
                }
                updateState(ControllerState.WAITING_PERMISSION, "USB permission denied")
                return
            }
            permissionRequestPendingDeviceName = null
            permissionRequestObservedFocusLoss = false
            UsbPermissionRequestCoordinator.release(PERMISSION_OWNER)

            // A denied adapter must not prevent an already authorized adapter from running.
            // Keep the denial until detach so polling does not reopen redundant system dialogs.
            val targetDeviceNames = permittedTargetDevices.map { device -> device.deviceName }.toSet()
            if (activeDeviceNames == targetDeviceNames) {
                val nativeAdapterCount = withContext(Dispatchers.Default) {
                    NativeCore.getRawBroadcastUsbAdapterCount(handle)
                }
                val nativeRunning = nativeAdapterCount == targetDeviceNames.size
                val currentDetachGeneration =
                    reconciledUsbDetachGeneration == usbDetachGeneration
                if (nativeRunning && currentDetachGeneration) {
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
            for (targetDevice in permittedTargetDevices.filter {
                it.deviceName !in startedDeviceNames
            }) {
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
            reconciledUsbDetachGeneration = usbDetachGeneration
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
            Intent(actionUsbPermission),
            PendingIntent.FLAG_IMMUTABLE
        )
        usbManager.requestPermission(device, pendingIntent)
    }

    //===================================================================================
    //===================================================================================
    // Restores VR input only after every RAW adapter permission prompt has completed.
    private suspend fun recoverVrFocusIfPermissionFlowSettled()
    {
        val shouldRecover = syncMutex.withLock {
            val permissionFlowSettled =
                permissionRequestPendingDeviceName == null &&
                    findSupportedAdapters().all { device ->
                        usbManager.hasPermission(device) ||
                            device.deviceName in permissionDeniedDeviceNames
                    }
            if (permissionFlowNeedsFocusRecovery && permissionFlowSettled) {
                permissionFlowNeedsFocusRecovery = false
                true
            } else {
                false
            }
        }
        if (shouldRecover) {
            requestVrFocusRecovery("rawBroadcastUsbPermissionFlowSettled")
        }
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
        const val PERMISSION_OWNER = "raw-broadcast"
        const val MAX_RAW_BROADCAST_ADAPTERS = 2
    }
}
