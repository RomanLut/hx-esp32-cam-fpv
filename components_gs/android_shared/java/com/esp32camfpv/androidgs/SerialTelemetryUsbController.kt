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
import com.hoho.android.usbserial.driver.UsbSerialPort
import com.hoho.android.usbserial.driver.UsbSerialProber
import com.hoho.android.usbserial.util.SerialInputOutputManager
import java.util.concurrent.Executors
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock

// Bridges a USB-UART adapter (CP210x / CH34x / FTDI / Prolific / CDC-ACM) to the
// native ISerialTelemetry channel used by gs_session_core. Mirrors the
// permission/attach-broadcast pattern used by RawBroadcastUsbController so all
// USB transports behave consistently.
class SerialTelemetryUsbController(
    private val activity: ComponentActivity
) {
    private val usbManager =
        activity.applicationContext.getSystemService(Context.USB_SERVICE) as UsbManager

    private var syncJob: Job? = null
    private var receiverRegistered = false
    private val syncMutex = Mutex()

    private var activeDeviceName: String? = null
    private var activeConnection: UsbDeviceConnection? = null
    private var activeSerialPort: UsbSerialPort? = null
    private var activeIoManager: SerialInputOutputManager? = null
    private val ioExecutor = Executors.newSingleThreadExecutor()

    private val receiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            when (intent?.action) {
                ACTION_USB_PERMISSION,
                UsbManager.ACTION_USB_DEVICE_DETACHED,
                UsbManager.ACTION_USB_DEVICE_ATTACHED -> {
                    activity.lifecycleScope.launch(Dispatchers.Main) { syncNow() }
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

        NativeCore.setSerialTelemetryWriter(::writeBytesFromNative)

        if (syncJob != null) return
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
            syncMutex.withLock { teardownActive() }
        }
        if (receiverRegistered) {
            activity.unregisterReceiver(receiver)
            receiverRegistered = false
        }
        NativeCore.setSerialTelemetryWriter(null)
    }

    private suspend fun syncNow() {
        syncMutex.withLock {
            // Publish the current set of attached UART devices first so the OSD
            // menu list reflects the latest state even when "None" is selected.
            publishUartList()

            val selection = try { NativeCore.getTelemetryUartSelection() } catch (_: Throwable) { "auto" }
            if (selection == "none") {
                if (activeDeviceName != null) {
                    Log.i(LOG_TAG, "Adapter closed (telemetry UART set to None)")
                    teardownActive()
                }
                return
            }

            val target = findSupportedDevice(selection)
            if (target == null) {
                if (activeDeviceName != null) {
                    Log.i(LOG_TAG, "Adapter detached or no longer matches selection")
                    teardownActive()
                }
                return
            }

            if (activeDeviceName == target.deviceName) {
                return
            }

            if (!usbManager.hasPermission(target)) {
                requestPermission(target)
                return
            }

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
                // Some FTDI/FC pairings need DTR asserted before the chip will
                // actually transmit bytes received from the host (the line stays
                // idle otherwise). Match the behavior of the reference Android
                // taranis-smartport project.
                try { port.dtr = true } catch (_: Throwable) {}
                try { port.rts = true } catch (_: Throwable) {}
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
            .filter { it.vendorId != RTL_VENDOR_ID }
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
            Intent(ACTION_USB_PERMISSION),
            PendingIntent.FLAG_IMMUTABLE
        )
        usbManager.requestPermission(device, pendingIntent)
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
        const val ACTION_USB_PERMISSION = "com.esp32camfpv.androidgs.USB_PERMISSION"
        const val WRITE_TIMEOUT_MS = 100
        // RTL adapters are owned by the WiFi raw-broadcast transport and must
        // not be opened as a serial port.
        const val RTL_VENDOR_ID = 0x0BDA
    }
}
