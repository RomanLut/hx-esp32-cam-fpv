package com.esp32camfpv.androidgs

import android.content.pm.ActivityInfo
import android.os.Bundle
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
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.ui.viewinterop.AndroidView
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.graphics.Color
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import kotlinx.coroutines.delay

private val VideoBackgroundColor = Color(0xFF0A0D14)

class MainActivity : ComponentActivity() {
    private var inputNativeHandle: Long = 0L
    private val inputBuildInfo: String by lazy { NativeCore.getBuildInfo() }

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
    }

    override fun onResume() {
        super.onResume()
        applyImmersiveFullscreen()
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
                NativeCore.setRendererScreenMode(nativeHandle, NativeCore.getScreenAspectRatio(nativeHandle))
                NativeCore.syncRendererOverlay(nativeHandle, inputBuildInfo)
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
        NativeCore.syncRendererOverlay(nativeHandle, buildInfo)
    }

    LaunchedEffect(nativeHandle) {
        refreshNativeState()
        NativeCore.startUdpClient(nativeHandle)
    }

    LaunchedEffect(nativeHandle) {
        while (true) {
            refreshNativeState()
            if (NativeCore.consumeExitRequested(nativeHandle)) {
                onExitApp()
                return@LaunchedEffect
            }
            delay(if (NativeCore.isUdpClientRunning(nativeHandle)) 16 else 250)
        }
    }

    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(VideoBackgroundColor)
            .pointerInput(nativeHandle) {
                detectTapGestures { offset ->
                    onUserInteraction()
                    NativeCore.handleTap(
                        nativeHandle,
                        offset.x,
                        offset.y,
                        size.width.toFloat(),
                        size.height.toFloat()
                    )
                    refreshNativeState()
                }
            }
    ) {
        NativeVideoSurface(
            nativeHandle = nativeHandle,
            modifier = Modifier.fillMaxSize()
        )
    }
}

@Composable
private fun NativeVideoSurface(
    nativeHandle: Long,
    modifier: Modifier = Modifier
) {
    AndroidView(
        modifier = modifier,
        factory = { context ->
            SurfaceView(context).apply {
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
        }
    )
}
