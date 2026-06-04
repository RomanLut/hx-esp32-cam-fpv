package com.esp32camfpv.questgs

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.ActivityInfo
import android.os.BatteryManager
import android.os.Build
import android.os.Bundle
import android.os.Environment
import android.os.PowerManager
import android.view.KeyEvent
import android.view.WindowManager
import android.util.Log
import androidx.activity.enableEdgeToEdge
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.core.splashscreen.SplashScreen.Companion.installSplashScreen
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import java.util.concurrent.atomic.AtomicInteger
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext

private val VideoBackgroundColor = Color(0xFF0A0D14)
class MainActivity : ComponentActivity() {
    companion object {
        private const val TAG = "QuestGs"
    }
    private val autoStartUsbControllers = true
    private var openXrStarted = false
    private var inputNativeHandle: Long = 0L
    private val inputBuildInfo: String by lazy { NativeCore.getBuildInfo() }
    private val currentThermalStatus = AtomicInteger(0)
    private val currentBatteryPercent = AtomicInteger(-1)
    private var batteryReceiver: BroadcastReceiver? = null
    private lateinit var apfpvWifiController: ApfpvWifiController
    private lateinit var rawBroadcastUsbController: RawBroadcastUsbController
    private lateinit var wifiScanUsbController: WifiScanUsbController
    private lateinit var serialTelemetryUsbController: SerialTelemetryUsbController
    private var powerManager: PowerManager? = null
    private var thermalStatusListener: PowerManager.OnThermalStatusChangedListener? = null

    private fun exitFromRuntimeMenu() {
        stopService(Intent(this, KeepAliveService::class.java))
        finishAndRemoveTask()
        // Kill the process so the next launch starts fresh. Quest's OpenXR runtime
        // and the renderer's EGL share-group bridge keep process-global state that
        // does not survive Activity recreation cleanly — relaunching in the same
        // process hangs (stale EGLContext in openxr_video_bridge, second
        // xrCreateInstance refused by the Quest runtime).
        android.os.Process.killProcess(android.os.Process.myPid())
    }

    private fun applyImmersiveFullscreen() {
        WindowCompat.setDecorFitsSystemWindows(window, false)
        WindowInsetsControllerCompat(window, window.decorView).let { controller ->
            controller.hide(WindowInsetsCompat.Type.systemBars())
            controller.systemBarsBehavior =
                WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        installSplashScreen()
        super.onCreate(savedInstanceState)
        setupThermalStatusMonitoring()
        setupBatteryMonitoring()
        NativeCore.setActivity(this)
        NativeCore.setAssetManager(assets)
        NativeCore.setSettingsPath(filesDir.resolve("gs.ini").absolutePath)
        NativeCore.setRecordingsPath(Environment.getExternalStorageDirectory().absolutePath)
        startService(Intent(this, KeepAliveService::class.java))
        if (!openXrStarted) {
            openXrStarted = NativeCore.startOpenXr(this)
            Log.i(TAG, "startOpenXr(onCreate) result=$openXrStarted")
        }
        apfpvWifiController = ApfpvWifiController(this) { inputNativeHandle }
        rawBroadcastUsbController = RawBroadcastUsbController(this) { inputNativeHandle }
        wifiScanUsbController = WifiScanUsbController(this) { inputNativeHandle }
        serialTelemetryUsbController = SerialTelemetryUsbController(this)
        requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        enableEdgeToEdge()
        applyImmersiveFullscreen()

        setContent {
            MaterialTheme {
                Surface(modifier = Modifier.fillMaxSize()) {
                    AndroidGsApp(
                        onHandleChanged = { inputNativeHandle = it },
                        thermalStatusProvider = { currentThermalStatusValue() },
                        batteryPercentProvider = { currentBatteryPercentValue() },
                        onExitApp = { exitFromRuntimeMenu() },
                        onScreenFlipVChanged = { flipV ->
                            requestedOrientation = if (flipV)
                                ActivityInfo.SCREEN_ORIENTATION_REVERSE_LANDSCAPE
                            else
                                ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE
                        }
                    )
                }
            }
        }

        apfpvWifiController.start()
        if (autoStartUsbControllers) {
            rawBroadcastUsbController.start()
            wifiScanUsbController.start()
            serialTelemetryUsbController.start()
        }
        handleUsbAttachIntent(intent)
    }

    override fun onResume() {
        super.onResume()
        if (!openXrStarted) {
            openXrStarted = NativeCore.startOpenXr(this)
            Log.i(TAG, "startOpenXr(onResume) result=$openXrStarted")
        }
        applyImmersiveFullscreen()
        apfpvWifiController.start()
        if (autoStartUsbControllers) {
            rawBroadcastUsbController.start()
            wifiScanUsbController.start()
            serialTelemetryUsbController.start()
        }
        handleUsbAttachIntent(intent)
    }

    override fun onPause() {
        super.onPause()
    }

    override fun onNewIntent(intent: android.content.Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        applyImmersiveFullscreen()
        handleUsbAttachIntent(intent)
    }

    private fun handleUsbAttachIntent(intent: Intent?) {
        if (intent?.action == android.hardware.usb.UsbManager.ACTION_USB_DEVICE_ATTACHED) {
            rawBroadcastUsbController.handleUsbTopologyChanged()
            wifiScanUsbController.handleUsbTopologyChanged()
        }
    }

    override fun onDestroy() {
        if (openXrStarted) {
            NativeCore.stopOpenXr()
            openXrStarted = false
        }
        NativeCore.setActivity(null)
        teardownThermalStatusMonitoring()
        teardownBatteryMonitoring()
        serialTelemetryUsbController.stop()
        wifiScanUsbController.stop()
        rawBroadcastUsbController.stop()
        apfpvWifiController.stop()
        super.onDestroy()
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) {
            applyImmersiveFullscreen()
        }
    }

    // Quest controller input bypasses Android's KeyEvent path entirely — when the
    // OpenXR session is FOCUSED no Android window has input focus, so KeyEvents
    // never reach the activity. Controller buttons / triggers / thumbsticks are
    // read via the OpenXR action system in syncControllerInputs() and routed to
    // ImGui through the openxr_video_bridge queue. dispatchKeyEvent here only
    // serves attached USB keyboards or other input devices.
    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        val nativeHandle = inputNativeHandle
        if (nativeHandle != 0L && event.action == KeyEvent.ACTION_DOWN) {
            applyImmersiveFullscreen()
            if (NativeCore.handleKey(nativeHandle, event.keyCode)) {
                if (NativeCore.consumeExitRequested(nativeHandle)) {
                    exitFromRuntimeMenu()
                }
                return true
            }
        }
        return super.dispatchKeyEvent(event)
    }

    private fun currentThermalStatusValue(): Int {
        return currentThermalStatus.get()
    }

    private fun currentBatteryPercentValue(): Int {
        return currentBatteryPercent.get()
    }

    private fun setupBatteryMonitoring() {
        val filter = IntentFilter(Intent.ACTION_BATTERY_CHANGED)
        val receiver = object : BroadcastReceiver() {
            override fun onReceive(context: Context, intent: Intent) {
                val level = intent.getIntExtra(BatteryManager.EXTRA_LEVEL, -1)
                val scale = intent.getIntExtra(BatteryManager.EXTRA_SCALE, -1)
                currentBatteryPercent.set(if (level >= 0 && scale > 0) level * 100 / scale else -1)
            }
        }
        batteryReceiver = receiver
        // ACTION_BATTERY_CHANGED is a sticky broadcast — registerReceiver returns the last intent
        val sticky = registerReceiver(receiver, filter)
        if (sticky != null) {
            val level = sticky.getIntExtra(BatteryManager.EXTRA_LEVEL, -1)
            val scale = sticky.getIntExtra(BatteryManager.EXTRA_SCALE, -1)
            currentBatteryPercent.set(if (level >= 0 && scale > 0) level * 100 / scale else -1)
        }
    }

    private fun teardownBatteryMonitoring() {
        batteryReceiver?.let { unregisterReceiver(it) }
        batteryReceiver = null
    }

    private fun setupThermalStatusMonitoring() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
            currentThermalStatus.set(0)
            return
        }

        val manager = getSystemService(PowerManager::class.java) ?: return
        powerManager = manager
        currentThermalStatus.set(manager.currentThermalStatus)
        val listener = PowerManager.OnThermalStatusChangedListener { status ->
            currentThermalStatus.set(status)
        }
        thermalStatusListener = listener
        manager.addThermalStatusListener(mainExecutor, listener)
    }

    private fun teardownThermalStatusMonitoring() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
            return
        }

        val manager = powerManager ?: return
        val listener = thermalStatusListener ?: return
        manager.removeThermalStatusListener(listener)
        thermalStatusListener = null
        powerManager = null
    }
}

@Composable
private fun AndroidGsApp(
    onHandleChanged: (Long) -> Unit,
    thermalStatusProvider: () -> Int,
    batteryPercentProvider: () -> Int,
    onExitApp: () -> Unit,
    onScreenFlipVChanged: (Boolean) -> Unit = {}
) {
    val nativeHandle = remember { NativeCore.createHandle(1) }
    DisposableEffect(nativeHandle) {
        onHandleChanged(nativeHandle)
        // Trigger renderer EGL init in offscreen pbuffer mode. The Quest build
        // doesn't display an Android Surface — drawing goes into a 1280x720 FBO
        // shared with the OpenXR thread. A null Surface here signals "init in
        // offscreen mode" to the JNI bridge.
        NativeCore.setRenderSurface(nativeHandle, null)
        onDispose {
            onHandleChanged(0L)
            NativeCore.clearRenderSurface(nativeHandle)
            NativeCore.stopUdpClient(nativeHandle)
            NativeCore.destroyHandle(nativeHandle)
        }
    }

    val buildInfo = remember { NativeCore.getBuildInfo() }
    fun refreshNativeState() {
        NativeCore.setRendererScreenMode(nativeHandle, NativeCore.getScreenAspectRatio(nativeHandle))
        NativeCore.setRendererVrMode(nativeHandle, NativeCore.isVrModeEnabled(nativeHandle))
        NativeCore.setThermalStatus(nativeHandle, thermalStatusProvider())
        NativeCore.setBatteryPercent(nativeHandle, batteryPercentProvider())
        NativeCore.syncRendererOverlay(nativeHandle, buildInfo)
        onScreenFlipVChanged(NativeCore.isScreenFlipVEnabled(nativeHandle))
    }

    LaunchedEffect(nativeHandle) {
        withContext(Dispatchers.Default) {
            refreshNativeState()
            NativeCore.setVideoUdpOutput(nativeHandle, "127.0.0.1", 5600)
        }
    }

    LaunchedEffect(nativeHandle) {
        while (true) {
            val exitRequested = withContext(Dispatchers.Default) {
                refreshNativeState()
                NativeCore.consumeExitRequested(nativeHandle)
            }
            if (exitRequested) {
                onExitApp()
                return@LaunchedEffect
            }
            val needsFastPoll = withContext(Dispatchers.Default) {
                NativeCore.isUdpClientRunning(nativeHandle) ||
                    NativeCore.isRawBroadcastUsbRunning(nativeHandle) ||
                    NativeCore.getActiveTransportKind(nativeHandle) == NativeCore.TRANSPORT_TEST
            }
            delay(if (needsFastPoll) 16 else 250)
        }
    }

    // No on-screen Compose content — the user only sees the OpenXR head-locked
    // composition layer. The empty background just satisfies the Surface slot.
    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(VideoBackgroundColor)
    )
}
