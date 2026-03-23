package com.esp32camfpv.androidgs

import android.graphics.BitmapFactory
import android.util.Log
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

data class UdpClientState(
    val running: Boolean = false,
    val packetsReceived: Int = 0,
    val lastEventKind: Int = NativeCore.EVENT_IGNORE,
    val lastFrameBytes: Int = 0,
    val throughputMbps: Float = 0f,
    val videoFps: Float = 0f,
    val lastError: String? = null,
    val peer: String = "192.168.4.1:5600"
)

class UdpGroundStationClient(
    private val nativeHandle: Long,
    private val onStateChanged: (UdpClientState) -> Unit,
    private val onFrameDecoded: (ImageBitmap?) -> Unit
) {
    companion object {
        private const val TAG = "AndroidGS"
    }

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private var job: Job? = null

    fun start(peerHost: String = "192.168.4.1", peerPort: Int = 5600, localPort: Int = 5600) {
        if (job?.isActive == true) {
            return
        }

        job = scope.launch {
            runClient(peerHost, peerPort, localPort)
        }
    }

    fun stop() {
        job?.cancel()
        job = null
        onStateChanged(UdpClientState(running = false))
    }

    private suspend fun runClient(peerHost: String, peerPort: Int, localPort: Int) {
        val peerLabel = "$peerHost:$peerPort"
        val peerAddress = InetAddress.getByName(peerHost)

        DatagramSocket(localPort).use { socket ->
            socket.soTimeout = 250

            var packetsReceived = 0
            var bytesWindow = 0L
            var framesWindow = 0
            var throughputMbps = 0f
            var videoFps = 0f
            var statsWindowStart = System.currentTimeMillis()
            onStateChanged(UdpClientState(running = true, peer = peerLabel))

            coroutineScope {
                val senderJob = launch {
                    while (isActive) {
                        val controlPackets = NativeCore.buildControlTransportPackets(nativeHandle)
                        controlPackets?.forEach { controlPacket ->
                            if (controlPacket.isNotEmpty()) {
                                val packet = DatagramPacket(controlPacket, controlPacket.size, peerAddress, peerPort)
                                socket.send(packet)
                            }
                        }
                        delay(250)
                    }
                }

                try {
                    val buffer = ByteArray(2048)
                    while (currentCoroutineContext().isActive) {
                        try {
                            val packet = DatagramPacket(buffer, buffer.size)
                            socket.receive(packet)

                            packetsReceived += 1
                            val payload = packet.data.copyOf(packet.length)
                            bytesWindow += packet.length.toLong()
                            val eventKind = NativeCore.pushPacket(nativeHandle, payload, false, 0)
                            val frameBytes = NativeCore.takeCompletedFrame(nativeHandle)

                            var frameSize = 0
                            if (frameBytes != null && frameBytes.isNotEmpty()) {
                                frameSize = frameBytes.size
                                framesWindow += 1
                                val bitmap = BitmapFactory.decodeByteArray(frameBytes, 0, frameBytes.size)
                                withContext(Dispatchers.Main) {
                                    onFrameDecoded(bitmap?.asImageBitmap())
                                }
                            }

                            val now = System.currentTimeMillis()
                            val elapsedMs = now - statsWindowStart
                            if (elapsedMs >= 1000) {
                                throughputMbps = (bytesWindow * 8f) / (elapsedMs * 1000f)
                                videoFps = (framesWindow * 1000f) / elapsedMs
                                bytesWindow = 0
                                framesWindow = 0
                                statsWindowStart = now
                            }

                            onStateChanged(
                                UdpClientState(
                                    running = true,
                                    packetsReceived = packetsReceived,
                                    lastEventKind = eventKind,
                                    lastFrameBytes = frameSize,
                                    throughputMbps = throughputMbps,
                                    videoFps = videoFps,
                                    peer = peerLabel
                                )
                            )
                        } catch (_: java.net.SocketTimeoutException) {
                            onStateChanged(
                                UdpClientState(
                                    running = true,
                                    packetsReceived = packetsReceived,
                                    lastEventKind = NativeCore.getLastEventKind(nativeHandle),
                                    lastFrameBytes = 0,
                                    throughputMbps = throughputMbps,
                                    videoFps = videoFps,
                                    peer = peerLabel
                                )
                            )
                        }
                    }
                } catch (t: Throwable) {
                    Log.e(TAG, "UDP failed", t)
                    onStateChanged(
                        UdpClientState(
                            running = false,
                            packetsReceived = packetsReceived,
                            lastEventKind = NativeCore.getLastEventKind(nativeHandle),
                            throughputMbps = throughputMbps,
                            videoFps = videoFps,
                            lastError = t.message ?: t.javaClass.simpleName,
                            peer = peerLabel
                        )
                    )
                } finally {
                    senderJob.cancel()
                }
            }
        }
    }
}
