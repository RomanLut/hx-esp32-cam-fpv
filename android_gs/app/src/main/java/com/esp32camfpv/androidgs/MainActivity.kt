package com.esp32camfpv.androidgs

import android.content.pm.ActivityInfo
import android.os.Bundle
import android.view.MotionEvent
import android.view.KeyEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
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
import androidx.compose.runtime.rememberCoroutineScope
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.compose.ui.viewinterop.AndroidView
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import java.util.concurrent.atomic.AtomicReference
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

private val VideoBackgroundColor = Color(0xFF0A0D14)
class MainActivity : ComponentActivity() {
    private var inputNativeHandle: Long = 0L
    private val inputBuildInfo: String by lazy { NativeCore.getBuildInfo() }
    private lateinit var apfpvWifiController: ApfpvWifiController

    private fun applyImmersiveFullscreen() {
        WindowCompat.setDecorFitsSystemWindows(window, false)
        WindowInsetsControllerCompat(window, window.decorView).let { controller ->
            controller.hide(WindowInsetsCompat.Type.systemBars())
            controller.systemBarsBehavior =
                WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        NativeCore.setAssetManager(assets)
        NativeCore.setSettingsPath(filesDir.resolve("gs.ini").absolutePath)
        apfpvWifiController = ApfpvWifiController(this) { inputNativeHandle }
        requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE
        enableEdgeToEdge()
        applyImmersiveFullscreen()

        setContent {
            MaterialTheme {
                Surface(modifier = Modifier.fillMaxSize()) {
                    AndroidGsApp(
                        onHandleChanged = { inputNativeHandle = it },
                        onUserInteraction = { applyImmersiveFullscreen() },
                        onExitApp = { finishAffinity() }
                    )
                }
            }
        }

        apfpvWifiController.start()
    }

    override fun onResume() {
        super.onResume()
        applyImmersiveFullscreen()
        apfpvWifiController.start()
    }

    override fun onDestroy() {
        apfpvWifiController.stop()
        super.onDestroy()
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) {
            applyImmersiveFullscreen()
        }
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        val nativeHandle = inputNativeHandle
        if (nativeHandle != 0L && event.action == KeyEvent.ACTION_DOWN) {
            applyImmersiveFullscreen()
            if (NativeCore.handleKey(nativeHandle, event.keyCode)) {
                if (NativeCore.consumeExitRequested(nativeHandle)) {
                    finishAffinity()
                }
                return true
            }
        }
        return super.dispatchKeyEvent(event)
    }
}

@Composable
private fun AndroidGsApp(
    onHandleChanged: (Long) -> Unit,
    onUserInteraction: () -> Unit,
    onExitApp: () -> Unit
) {
    val nativeHandle = remember { NativeCore.createHandle(1) }
    val scope = rememberCoroutineScope()
    DisposableEffect(nativeHandle) {
        onHandleChanged(nativeHandle)
        onDispose {
            onHandleChanged(0L)
            NativeCore.stopUdpClient(nativeHandle)
            NativeCore.destroyHandle(nativeHandle)
        }
    }

    val buildInfo = remember { NativeCore.getBuildInfo() }
    fun refreshNativeState() {
        NativeCore.setRendererScreenMode(nativeHandle, NativeCore.getScreenAspectRatio(nativeHandle))
        NativeCore.setRendererVrMode(nativeHandle, NativeCore.isVrModeEnabled(nativeHandle))
        NativeCore.syncRendererOverlay(nativeHandle, buildInfo)
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
            val isRunning = withContext(Dispatchers.Default) {
                NativeCore.isUdpClientRunning(nativeHandle)
            }
            delay(if (isRunning) 16 else 250)
        }
    }

    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(VideoBackgroundColor)
    ) {
        NativeVideoSurface(
            nativeHandle = nativeHandle,
            onUserInteraction = onUserInteraction,
            modifier = Modifier.fillMaxSize()
        )
    }
}

@Composable
private fun NativeVideoSurface(
    nativeHandle: Long,
    onUserInteraction: () -> Unit,
    modifier: Modifier = Modifier
) {
    val scope = rememberCoroutineScope()
    val lifecycleOwner = LocalLifecycleOwner.current
    val surfaceViewRef = remember { AtomicReference<SurfaceView?>(null) }

    DisposableEffect(nativeHandle, lifecycleOwner) {
        val observer = LifecycleEventObserver { _, event ->
            if (event == Lifecycle.Event.ON_RESUME) {
                val surfaceView = surfaceViewRef.get()
                val surface = surfaceView?.holder?.surface
                if (surface != null && surface.isValid) {
                    NativeCore.setRenderSurface(nativeHandle, surface)
                }
            }
        }
        lifecycleOwner.lifecycle.addObserver(observer)
        onDispose {
            lifecycleOwner.lifecycle.removeObserver(observer)
        }
    }

    AndroidView(
        modifier = modifier,
        factory = { context ->
            SurfaceView(context).apply {
                surfaceViewRef.set(this)
                isClickable = true
                isFocusable = true
                setOnTouchListener { _, event ->
                    if (event.action == MotionEvent.ACTION_UP) {
                        onUserInteraction()
                        scope.launch(Dispatchers.Default) {
                            NativeCore.handleTap(
                                nativeHandle,
                                event.x,
                                event.y,
                                width.toFloat(),
                                height.toFloat()
                            )
                        }
                        true
                    } else {
                        true
                    }
                }
                holder.addCallback(object : SurfaceHolder.Callback {
                    override fun surfaceCreated(holder: SurfaceHolder) {
                        NativeCore.setRenderSurface(nativeHandle, holder.surface)
                    }

                    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
                        NativeCore.setRenderSurface(nativeHandle, holder.surface)
                    }

                    override fun surfaceDestroyed(holder: SurfaceHolder) {
                        NativeCore.clearRenderSurface(nativeHandle)
                    }
                })
            }
        },
        update = { view ->
            surfaceViewRef.set(view)
            val surface = view.holder.surface
            if (surface != null && surface.isValid) {
                NativeCore.setRenderSurface(nativeHandle, surface)
            }
        }
    )
}
