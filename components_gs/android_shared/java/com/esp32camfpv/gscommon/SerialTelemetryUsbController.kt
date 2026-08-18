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
import com.hoho.android.usbserial.driver.UsbSerialPort
import com.hoho.android.usbserial.driver.UsbSerialProber
import com.hoho.android.usbserial.util.SerialInputOutputManager
import java.util.concurrent.Executors
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock

//===================================================================================
//===================================================================================
// Bridges one USB-UART device to native telemetry with single-flight permission handling.
//
// requestVrFocusRecovery is a no-op on the phone build; the Quest build uses it to
// reclaim VR input after the system permission dialog has taken focus away.
class SerialTelemetryUsbController(
    private val activity: ComponentActivity,
    private val requestVrFocusRecovery: (String) -> Unit = {}
)
{
    private val usbManager =
        activity.applicationContext.getSystemService(Context.USB_SERVICE) as UsbManager

    // Scoped to the installed package so two GS apps on one device cannot observe each
    // other's permission broadcasts.
    private val actionUsbPermission =
        "${activity.applicationContext.packageName}.USB_PERMISSION"

    private var syncJob: Job? = null
    private var receiverRegistered = false
    private var permissionRequestPendingDeviceName: String? = null
    private val permissionDeniedDeviceNames = mutableSetOf<String>()
    private var lastPermissionSelection: String? = null
    private var usbDetachGeneration = 0L
    private var reconciledUsbDetachGeneration = 0L
    private val syncMutex = Mutex()

    private var activeDeviceName: String? = null
    private var activeConnection: UsbDeviceConnection? = null
    private var activeSerialPort: UsbSerialPort? = null
    private var activeIoManager: SerialInputOutputManager? = null
    private val ioExecutor = Executors.newSingleThreadExecutor()

    private val receiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            when (intent?.action) {
                actionUsbPermission -> {
                    val permissionDevice = intent.getUsbDevice() ?: findSupportedDevice("auto")
                    UsbPermissionRequestCoordinator.release(PERMISSION_OWNER)
                    permissionRequestPendingDeviceName = null
                    if (intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)) {
                        permissionDevice?.deviceName?.let { permissionDeniedDeviceNames.remove(it) }
                    } else {
                        permissionDevice?.deviceName?.let { permissionDeniedDeviceNames.add(it) }
                    }
                    requestVrFocusRecovery("serialUsbPermissionResult")
                    activity.lifecycleScope.launch(Dispatchers.Main) { syncNowSafely() }
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
                            detachedDeviceName?.let { permissionDeniedDeviceNames.remove(it) }
                            usbDetachGeneration++
                        }
                        syncNowSafely()
                    }
                }

                UsbManager.ACTION_USB_DEVICE_ATTACHED -> {
                    activity.lifecycleScope.launch(Dispatchers.Main) {
                        syncMutex.withLock {
                            // Do not let another child on the same hub erase an in-flight UART
                            // permission request before the system delivers its result.
                            intent.getUsbDevice()?.deviceName?.let {
                                permissionDeniedDeviceNames.remove(it)
                            }
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

        NativeCore.setSerialTelemetryWriter(::writeBytesFromNative)

        if (syncJob != null) return
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
            syncMutex.withLock { teardownActive() }
        }
        if (receiverRegistered) {
            activity.unregisterReceiver(receiver)
            receiverRegistered = false
        }
        NativeCore.setSerialTelemetryWriter(null)
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
            // A detach can invalidate UsbDevice/UsbSerialPort between discovery and open.
            // Keep periodic reconciliation alive so reappearing hardware recovers automatically.
            Log.e(LOG_TAG, "Serial USB reconciliation failed; polling will continue", error)
        }
    }

    private suspend fun syncNow() {
        syncMutex.withLock {
            // Publish the current set of attached UART devices first so the OSD
            // menu list reflects the latest state even when "None" is selected.
            publishUartList()

            val selection = try { NativeCore.getTelemetryUartSelection() } catch (_: Throwable) { "auto" }
            if (selection != lastPermissionSelection) {
                UsbPermissionRequestCoordinator.release(PERMISSION_OWNER)
                permissionRequestPendingDeviceName = null
                permissionDeniedDeviceNames.clear()
                lastPermissionSelection = selection
            }
            if (selection == "none") {
                UsbPermissionRequestCoordinator.release(PERMISSION_OWNER)
                permissionRequestPendingDeviceName = null
                permissionDeniedDeviceNames.clear()
                if (activeDeviceName != null) {
                    Log.i(LOG_TAG, "Adapter closed (telemetry UART set to None)")
                    teardownActive()
                }
                return
            }

            val target = findSupportedDevice(selection)
            if (target == null) {
                UsbPermissionRequestCoordinator.release(PERMISSION_OWNER)
                permissionRequestPendingDeviceName = null
                permissionDeniedDeviceNames.clear()
                if (activeDeviceName != null) {
                    Log.i(LOG_TAG, "Adapter detached or no longer matches selection")
                    teardownActive()
                }
                reconciledUsbDetachGeneration = usbDetachGeneration
                return
            }

            if (activeDeviceName == target.deviceName &&
                reconciledUsbDetachGeneration == usbDetachGeneration
            ) {
                return
            }

            if (activeDeviceName != null &&
                reconciledUsbDetachGeneration != usbDetachGeneration
            ) {
                // Android can reuse the same /dev/bus/usb path after a fast hub replug.
                // A detach generation proves the old file descriptor is stale even when
                // the replacement has the same name, so rebuild it without restarting GS.
                teardownActive()
            }

            if (!usbManager.hasPermission(target)) {
                // The platform does not coalesce repeated UsbManager requests. Keep one dialog
                // in flight and remember denial until the topology or selection changes.
                if (permissionRequestPendingDeviceName == null &&
                    target.deviceName !in permissionDeniedDeviceNames &&
                    UsbPermissionRequestCoordinator.tryAcquire(PERMISSION_OWNER, target.deviceName)
                ) {
                    permissionRequestPendingDeviceName = target.deviceName
                    requestPermission(target)
                }
                return
            }
            UsbPermissionRequestCoordinator.release(PERMISSION_OWNER)
            permissionRequestPendingDeviceName = null
            permissionDeniedDeviceNames.remove(target.deviceName)

            // Different device than the one we had open — tear down first.
            teardownActive()

            val driver = UsbSerialProber.getDefaultProber().probeDevice(target)
            if (driver == null || driver.ports.isEmpty()) {
                Log.w(LOG_TAG, "No serial driver for ${target.deviceName}")
                return
            }
            val port = driver.ports[0]

            val connection = usbManager.openDevice(target)
            if (connection == null) {
                Log.w(LOG_TAG, "Failed to open USB device ${target.deviceName}")
                return
            }
            try {
                port.open(connection)
                port.setParameters(
                    115200,
                    8,
                    UsbSerialPort.STOPBITS_1,
                    UsbSerialPort.PARITY_NONE
                )
                // Arduino USBCDC suppresses TX while TinyUSB sees DTR deasserted. A failed
                // control-line request therefore means the port is not usable and must not
                // be published as open; the surrounding failure path closes it and retries.
                port.dtr = true
                port.rts = true
            } catch (t: Throwable) {
                Log.w(LOG_TAG, "Open/setParameters failed for ${target.deviceName}", t)
                try { port.close() } catch (_: Throwable) {}
                connection.close()
                return
            }

            // onRunError can fire after a replug has already opened a new port, so
            // it must scope its teardown to its own SerialInputOutputManager. Without
            // this self-check, a stale error from the prior session would close the
            // freshly reopened port.
            var managerSelf: SerialInputOutputManager? = null
            val ioManager = SerialInputOutputManager(port, object : SerialInputOutputManager.Listener {
                override fun onNewData(data: ByteArray?) {
                    if (data != null && data.isNotEmpty()) {
                        NativeCore.serialTelemetryOnBytes(data, data.size)
                    }
                }
                override fun onRunError(e: Exception?) {
                    val self = managerSelf ?: return
                    activity.lifecycleScope.launch(Dispatchers.Main) {
                        syncMutex.withLock {
                            if (activeIoManager === self) {
                                Log.w(LOG_TAG, "Serial IO error", e)
                                teardownActive()
                            }
                        }
                    }
                }
            })
            managerSelf = ioManager
            ioExecutor.submit(ioManager)

            activeConnection = connection
            activeDeviceName = target.deviceName
            activeSerialPort = port
            activeIoManager = ioManager
            reconciledUsbDetachGeneration = usbDetachGeneration
            NativeCore.serialTelemetryOnOpen()
            Log.i(LOG_TAG, "Opened UART ${target.deviceName} VID=0x${target.vendorId.toString(16)} @ 115200")
        }
    }

    private fun teardownActive() {
        if (activeDeviceName == null) return
        NativeCore.serialTelemetryOnClose()
        try { activeIoManager?.stop() } catch (_: Throwable) {}
        try { activeSerialPort?.close() } catch (_: Throwable) {}
        try { activeConnection?.close() } catch (_: Throwable) {}
        activeIoManager = null
        activeSerialPort = null
        activeConnection = null
        activeDeviceName = null
    }

    // Stable identifier shown in the menu and persisted as telemetryUart:
    // "<productName> (vid:pid)" or just "(vid:pid)" if no product name is reported.
    private fun deviceIdentifier(d: UsbDevice): String {
        val vidpid = "%04x:%04x".format(d.vendorId, d.productId)
        val name = d.productName?.takeIf { it.isNotBlank() }
        return if (name != null) "$name ($vidpid)" else "($vidpid)"
    }

    // Returns the set of UsbDevices that usb-serial-for-android can actually
    // drive as serial ports. Rejects same-VID non-UART peripherals (e.g. the
    // WCH USB-Ethernet adapter that shares VID 0x1A86 with CH340).
    private fun listSerialDevices(): List<UsbDevice> {
        return UsbSerialProber.getDefaultProber()
            .findAllDrivers(usbManager)
            .map { it.device }
            // Exclude RTL adapters owned by the WiFi raw-broadcast transport.
            .filterNot { RtlUsbDeviceAllowlist.isSupported(it) }
    }

    private fun findSupportedDevice(selection: String): UsbDevice? {
        val candidates = listSerialDevices()
        return when (selection) {
            "auto" -> candidates.firstOrNull()
            else -> candidates.firstOrNull { deviceIdentifier(it) == selection }
        }
    }

    private fun publishUartList() {
        val list = listSerialDevices()
            .map { deviceIdentifier(it) }
            .toTypedArray()
        try {
            NativeCore.publishTelemetryUarts(list)
        } catch (_: Throwable) {
            // Native side may not be ready yet on very early calls.
        }
    }

    private fun requestPermission(device: UsbDevice) {
        val pendingIntent = PendingIntent.getBroadcast(
            activity, 0,
            Intent(actionUsbPermission),
            PendingIntent.FLAG_IMMUTABLE
        )
        usbManager.requestPermission(device, pendingIntent)
    }

    private fun Intent.getUsbDevice(): UsbDevice? {
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            getParcelableExtra(UsbManager.EXTRA_DEVICE, UsbDevice::class.java)
        } else {
            @Suppress("DEPRECATION")
            getParcelableExtra(UsbManager.EXTRA_DEVICE)
        }
    }

    // Called from NativeCore (JNI thread) to push outbound telemetry bytes.
    private fun writeBytesFromNative(data: ByteArray) {
        val port = activeSerialPort ?: return
        try {
            port.write(data, WRITE_TIMEOUT_MS)
        } catch (t: Throwable) {
            Log.w(LOG_TAG, "serial write failed", t)
        }
    }

    private companion object {
        const val LOG_TAG = "SerialTelemetryUsb"
        const val PERMISSION_OWNER = "serial-telemetry"
        const val WRITE_TIMEOUT_MS = 100
    }
}
