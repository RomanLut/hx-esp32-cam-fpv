package com.esp32camfpv.androidgs

import android.content.pm.ActivityInfo
import android.os.Bundle
import androidx.activity.enableEdgeToEdge
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.offset
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import kotlinx.coroutines.delay

private val MenuTitleColor = Color(0xFF618969)
private val MenuItemColor = Color(0xFF253358)
private val MenuItemSelectedColor = Color(0xFF4D89CD)
private val MenuStatusColor = Color(0xFF303030)
private val MenuScreenOverlay = Color(0x66000000)
private val VideoBackgroundColor = Color(0xFF0A0D14)
private val OverlayChipColor = Color(0xCC6A6A6A)
private val OverlayChipAlert = Color(0xCC8A4949)

data class AndroidMenuState(
    val visible: Boolean = false,
    val title: String = "",
    val items: List<String> = emptyList(),
    val statuses: List<String> = emptyList(),
    val statusLines: List<String> = emptyList(),
    val selectedIndex: Int = 0
)

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE
        enableEdgeToEdge()
        WindowCompat.setDecorFitsSystemWindows(window, false)
        WindowInsetsControllerCompat(window, window.decorView).let { controller ->
            controller.hide(WindowInsetsCompat.Type.systemBars())
            controller.systemBarsBehavior =
                WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }

        setContent {
            MaterialTheme {
                Surface(modifier = Modifier.fillMaxSize()) {
                    AndroidGsApp(
                        onExitApp = { finishAffinity() }
                    )
                }
            }
        }
    }
}

@Composable
private fun AndroidGsApp(
    onExitApp: () -> Unit
) {
    val nativeHandle = remember { NativeCore.createHandle(1) }
    val screenTapSource = remember { MutableInteractionSource() }
    DisposableEffect(nativeHandle) {
        onDispose {
            NativeCore.destroyHandle(nativeHandle)
        }
    }

    val buildInfo = remember { NativeCore.getBuildInfo() }
    var sessionInfo by remember(nativeHandle) { mutableStateOf(NativeCore.describeHandle(nativeHandle)) }
    var udpState by remember { mutableStateOf(UdpClientState()) }
    var latestFrame by remember { mutableStateOf<ImageBitmap?>(null) }
    var menuState by remember(nativeHandle) { mutableStateOf(readMenuState(nativeHandle)) }
    fun refreshNativeState() {
        sessionInfo = NativeCore.describeHandle(nativeHandle)
        menuState = readMenuState(nativeHandle)
    }

    val udpClient = remember(nativeHandle) {
        UdpGroundStationClient(
            nativeHandle = nativeHandle,
            onStateChanged = { state ->
                udpState = state
                refreshNativeState()
            },
            onFrameDecoded = { frame ->
                latestFrame = frame
            }
        )
    }

    DisposableEffect(udpClient) {
        onDispose {
            udpClient.stop()
        }
    }

    LaunchedEffect(nativeHandle) {
        refreshNativeState()
    }

    LaunchedEffect(udpClient) {
        if (!udpState.running) {
            udpClient.start()
        }
    }

    LaunchedEffect(nativeHandle, udpState.running) {
        while (true) {
            refreshNativeState()
            if (NativeCore.consumeExitRequested(nativeHandle)) {
                onExitApp()
                return@LaunchedEffect
            }
            delay(if (udpState.running) 250 else 600)
        }
    }

    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(VideoBackgroundColor)
            .clickable(
                interactionSource = screenTapSource,
                indication = null
            ) {
                if (!menuState.visible) {
                    NativeCore.setMenuVisible(nativeHandle, true)
                    menuState = readMenuState(nativeHandle)
                }
            }
    ) {
        latestFrame?.let { frame ->
            Image(
                bitmap = frame,
                contentDescription = "Video frame",
                modifier = Modifier.fillMaxSize(),
                contentScale = ContentScale.Fit
            )
        } ?: Box(
            modifier = Modifier
                .fillMaxSize()
                .background(VideoBackgroundColor),
            contentAlignment = Alignment.Center
        ) {
            Text(
                text = "No frame yet",
                color = Color(0xFFC7D4EE),
                fontSize = 24.sp,
                fontFamily = FontFamily.Monospace
            )
        }

        TopStatusOverlay(
            sessionInfo = sessionInfo,
            udpState = udpState,
            modifier = Modifier
                .align(Alignment.TopStart)
                .padding(top = 8.dp, start = 8.dp, end = 8.dp)
        )

        if (menuState.visible) {
            val sensorName = if (sessionInfo.contains("OV5640")) "OV5640" else "OV2640"
            val mainMenuTitle = "ESP32-CAM-FPV v0.5.3 $sensorName"
            AndroidGsMenuOverlay(
                menuState = menuState,
                buildInfo = buildInfo,
                headerTitleOverride = if (menuState.title == "ESP32-CAM-FPV") mainMenuTitle else null,
                headerRightText = null,
                onBack = {
                    NativeCore.menuGoBack(nativeHandle)
                    menuState = readMenuState(nativeHandle)
                },
                onItemSelected = { itemIndex ->
                    NativeCore.menuSelectItem(nativeHandle, itemIndex)
                    menuState = readMenuState(nativeHandle)
                }
            )
        }
    }
}

@Composable
private fun TopStatusOverlay(
    sessionInfo: String,
    udpState: UdpClientState,
    modifier: Modifier = Modifier
) {
    val airRssi = sessionInfoValue(sessionInfo, "air_rssi") ?: "--"
    val queueMax = sessionInfoValue(sessionInfo, "queue_max") ?: "0"
    val resolution = sessionInfoValue(sessionInfo, "resolution") ?: "--"
    val wifiOvf = sessionInfoValue(sessionInfo, "wifi_ovf") == "1"
    val airRecord = sessionInfoValue(sessionInfo, "air_record") == "1"

    Row(
        modifier = modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.spacedBy(6.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        OverlayChip("AIR:$airRssi")
        OverlayChip("GS:UDP")
        OverlayChip("${queueMax}%")
        OverlayChip(String.format("%.1fMb", udpState.throughputMbps))
        OverlayChip(resolution)
        OverlayChip(String.format("%02d", udpState.videoFps.toInt()))
        if (wifiOvf) {
            OverlayChip("OVF", alert = true)
        }
        if (airRecord) {
            OverlayChip("AIR", alert = true)
        }
    }
}

@Composable
private fun OverlayChip(
    text: String,
    alert: Boolean = false
) {
    Box(
        modifier = Modifier
            .background(if (alert) OverlayChipAlert else OverlayChipColor)
            .padding(horizontal = 10.dp, vertical = 6.dp),
        contentAlignment = Alignment.Center
    ) {
        Text(
            text = text,
            color = Color.White,
            fontFamily = FontFamily.Monospace,
            fontSize = 16.sp,
            fontWeight = FontWeight.Medium,
            maxLines = 1
        )
    }
}

@Composable
private fun AndroidGsMenuOverlay(
    menuState: AndroidMenuState,
    buildInfo: String,
    headerTitleOverride: String?,
    headerRightText: String?,
    onBack: () -> Unit,
    onItemSelected: (Int) -> Unit
) {
    val dismissTapSource = remember { MutableInteractionSource() }
    val menuTapSource = remember { MutableInteractionSource() }
    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(MenuScreenOverlay)
            .clickable(
                interactionSource = dismissTapSource,
                indication = null
            ) { onBack() },
        contentAlignment = Alignment.Center
    ) {
        BoxWithConstraints(
            modifier = Modifier.fillMaxSize(),
            contentAlignment = Alignment.Center
        ) {
            val scrollState = rememberScrollState()
            val visibleItemSlots = maxOf(menuState.items.size, 7)
            val visibleStatusLines = menuState.statusLines.take(2)
            val rowSlots = visibleItemSlots + 2 + visibleStatusLines.size
            val maxMenuHeight = maxHeight * 0.99f
            val maxMenuWidth = maxWidth * 0.92f
            val menuHeight = minOf(maxMenuHeight, maxMenuWidth * 0.75f)
            val menuWidth = menuHeight * (4f / 3f)
            val rowHeight = fitRowHeight(menuHeight, rowSlots)
            val headerHeight = rowHeight + 4.dp
            val footerHeight = rowHeight
            val spacing = 4.dp
            val sectionSpacing = spacing * 2
            val statusWidth = minOf(180.dp, menuWidth * 0.28f)
            val titleFont = rowHeight.value.coerceAtLeast(28f) * 0.50f
            val itemFont = rowHeight.value.coerceAtLeast(28f) * 0.42f
            val statusFont = rowHeight.value.coerceAtLeast(28f) * 0.39f
            val footerFont = rowHeight.value.coerceAtLeast(28f) * 0.31f
            val headerBleed = menuWidth * 0.05f
            val statusAreaHeight =
                if (visibleStatusLines.isEmpty()) 0.dp
                else (rowHeight * visibleStatusLines.size) + (spacing * (visibleStatusLines.size - 1)) + sectionSpacing
            val itemAreaMaxHeight = menuHeight - headerHeight - footerHeight - statusAreaHeight - sectionSpacing

            Column(
                modifier = Modifier
                    .width(menuWidth)
                    .height(menuHeight)
                    .background(Color.Transparent)
                    .clickable(
                        interactionSource = menuTapSource,
                        indication = null
                    ) {}
                    .padding(6.dp),
                verticalArrangement = Arrangement.spacedBy(spacing)
            ) {
                Box(
                    modifier = Modifier
                        .width(menuWidth + headerBleed)
                        .offset(x = -headerBleed)
                ) {
                    MenuTitleRow(
                        title = headerTitleOverride ?: menuState.title.ifEmpty { "ESP32-CAM-FPV" },
                        rightText = headerRightText,
                        height = headerHeight,
                        titleFont = titleFont,
                        metaFont = footerFont
                    )
                }

                Spacer(modifier = Modifier.height(sectionSpacing))

                Column(
                    modifier = Modifier
                        .fillMaxWidth()
                        .heightIn(max = itemAreaMaxHeight)
                        .verticalScroll(scrollState),
                    verticalArrangement = Arrangement.spacedBy(spacing)
                ) {
                    menuState.items.forEachIndexed { index, caption ->
                        MenuItemRow(
                            caption = caption,
                            status = menuState.statuses.getOrNull(index).orEmpty(),
                            selected = index == menuState.selectedIndex,
                            rowHeight = rowHeight,
                            statusWidth = statusWidth,
                            itemFont = itemFont,
                            statusFont = statusFont,
                            onBack = onBack,
                            onClick = { onItemSelected(index) }
                        )
                    }

                    if (menuState.items.isEmpty()) {
                        MenuItemRow(
                            caption = "No items",
                            status = "",
                            selected = false,
                            rowHeight = rowHeight,
                            statusWidth = statusWidth,
                            itemFont = itemFont,
                            statusFont = statusFont,
                            onBack = onBack,
                            onClick = {}
                        )
                    }
                }

                if (visibleStatusLines.isNotEmpty()) {
                    Spacer(modifier = Modifier.height(sectionSpacing))
                    visibleStatusLines.forEach { line ->
                        MenuStatusLine(
                            text = line,
                            rowHeight = rowHeight,
                            fontSize = statusFont
                        )
                    }
                }

                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .height(footerHeight),
                    horizontalArrangement = Arrangement.End,
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Text(
                        text = buildInfo,
                        color = Color(0xFFB7C4DB),
                        fontSize = footerFont.sp,
                        fontFamily = FontFamily.Monospace,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis
                    )
                }
            }
        }
    }
}

@Composable
private fun MenuStatusLine(
    text: String,
    rowHeight: Dp,
    fontSize: Float
) {
    Box(
        modifier = Modifier
            .fillMaxWidth()
            .height(rowHeight)
            .background(MenuStatusColor)
            .padding(horizontal = 10.dp, vertical = 8.dp),
        contentAlignment = Alignment.CenterStart
    ) {
        Text(
            text = text,
            color = Color.White,
            fontFamily = FontFamily.Monospace,
            fontSize = fontSize.sp,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis
        )
    }
}

@Composable
private fun MenuTitleRow(
    title: String,
    rightText: String?,
    height: Dp,
    titleFont: Float,
    metaFont: Float
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .height(height)
            .background(MenuTitleColor)
            .padding(start = 12.dp, end = 12.dp, top = 8.dp, bottom = 8.dp),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically
    ) {
        Text(
            text = title,
            color = Color.White,
            fontFamily = FontFamily.Monospace,
            fontWeight = FontWeight.Bold,
            fontSize = titleFont.sp
        )
        if (!rightText.isNullOrEmpty()) {
            Text(
                text = rightText,
                color = Color.White,
                fontFamily = FontFamily.Monospace,
                fontSize = metaFont.sp,
                maxLines = 1
            )
        }
    }
}

@Composable
private fun MenuItemRow(
    caption: String,
    status: String,
    selected: Boolean,
    rowHeight: Dp,
    statusWidth: Dp,
    itemFont: Float,
    statusFont: Float,
    onBack: () -> Unit,
    onClick: () -> Unit
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .height(rowHeight),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Box(
            modifier = Modifier
                .width(18.dp)
                .fillMaxSize()
                .clickable(onClick = onBack)
        )
        Spacer(modifier = Modifier.size(2.dp))
        Box(
            modifier = Modifier
                .weight(1f)
                .background(
                    if (selected) MenuItemSelectedColor else MenuItemColor
                )
                .clickable(onClick = onClick)
                .padding(start = 8.dp, end = 10.dp, top = 8.dp, bottom = 8.dp)
        ) {
            Text(
                text = caption,
                color = Color.White,
                fontFamily = FontFamily.Monospace,
                fontSize = itemFont.sp,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis
            )
        }

        if (status.isNotEmpty()) {
            Spacer(modifier = Modifier.size(6.dp))
            Box(
                modifier = Modifier
                    .width(statusWidth)
                    .background(MenuStatusColor)
                    .padding(horizontal = 10.dp, vertical = 8.dp)
            ) {
                Text(
                    text = status,
                    color = Color.White,
                    fontFamily = FontFamily.Monospace,
                    fontSize = statusFont.sp,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis
                )
            }
        }
    }
}

private fun readMenuState(nativeHandle: Long): AndroidMenuState {
    return AndroidMenuState(
        visible = NativeCore.isMenuVisible(nativeHandle),
        title = NativeCore.getMenuTitle(nativeHandle),
        items = NativeCore.getMenuItems(nativeHandle).toList(),
        statuses = NativeCore.getMenuStatuses(nativeHandle).toList(),
        statusLines = NativeCore.getMenuStatusLines(nativeHandle).toList(),
        selectedIndex = NativeCore.getMenuSelectedIndex(nativeHandle)
    )
}

private fun fitRowHeight(maxMenuHeight: Dp, rowSlots: Int): Dp {
    val totalSpacing = 4.dp * (rowSlots - 1)
    val available = (maxMenuHeight - totalSpacing).coerceAtLeast(180.dp)
    return (available / rowSlots).coerceIn(36.dp, 72.dp)
}

private fun sessionInfoValue(sessionInfo: String, key: String): String? {
    val marker = "$key="
    return sessionInfo
        .split('|')
        .map { it.trim() }
        .firstOrNull { it.startsWith(marker) }
        ?.substringAfter('=')
}
