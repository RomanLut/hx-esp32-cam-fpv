package com.esp32camfpv.androidgs

import android.Manifest
import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.net.wifi.WifiConfiguration
import android.net.wifi.WifiManager
import android.net.wifi.WifiNetworkSpecifier
import android.os.Build
import android.util.Log
import androidx.activity.ComponentActivity
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.content.ContextCompat
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

class ApfpvWifiController(
    private val activity: ComponentActivity,
    private val currentNativeHandle: () -> Long
) {
    private data class CameraNetwork(
        val ssid: String,
        val deviceId: Int,
        val level: Int
    )

    private val wifiManager =
        activity.applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
    private val connectivityManager =
        activity.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
    private val wifiStreamingLock =
        wifiManager.createWifiLock(getWifiStreamingLockMode(), "$LOG_TAG:Streaming").apply {
            setReferenceCounted(false)
        }

    private val permissionLauncher =
        activity.registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) {
            permissionRequestInFlight = false
            if (hasRequiredPermissions()) {
                activity.lifecycleScope.launch(Dispatchers.Main) {
                    syncNow()
                }
            }
        }

    private var syncJob: Job? = null
    private var requestedSsid: String? = null
    private var requestedNetworkCallback: ConnectivityManager.NetworkCallback? = null
    private var boundNetwork: Network? = null
    private var permissionRequestInFlight = false

    fun start() {
        if (syncJob != null) {
            return
        }

        syncJob = activity.lifecycleScope.launch(Dispatchers.Main) {
            while (true) {
                syncNow()
                delay(SCAN_INTERVAL_MS)
            }
        }
    }

    fun stop() {
        syncJob?.cancel()
        syncJob = null
        updateLinkState(NativeCore.LINK_STATE_NONE)
        syncCameraState(emptyList(), null)
        releaseRequestedNetwork()
        releaseWifiStreamingLock()
        bindToNetwork(null)
    }

    private suspend fun syncNow() {
        val handle = currentNativeHandle()
        if (handle == 0L) {
            releaseRequestedNetwork()
            releaseWifiStreamingLock()
            bindToNetwork(null)
            return
        }

        val activeTransportKind = withContext(Dispatchers.Default) {
            NativeCore.getActiveTransportKind(handle)
        }
        if (activeTransportKind != NativeCore.TRANSPORT_APFPV) {
            updateLinkState(NativeCore.LINK_STATE_NONE)
            syncCameraState(emptyList(), null)
            releaseRequestedNetwork()
            releaseWifiStreamingLock()
            bindToNetwork(null)
            return
        }

        acquireWifiStreamingLock()

        if (!hasRequiredPermissions()) {
            requestRequiredPermissions()
            return
        }

        val currentSsid = currentConnectedCameraSsid()
        val cameraNetworks = withContext(Dispatchers.Default) {
            findCameraNetworks()
        }
        syncCameraState(cameraNetworks, currentSsid)

        val nativeState = withContext(Dispatchers.Default) {
            listOf(
                NativeCore.getPreferredApfpvCameraId(handle),
                if (NativeCore.isApfpvMenuSearchActive(handle)) 1 else 0,
                if (NativeCore.consumeApfpvReconnectRequest(handle)) 1 else 0,
                if (NativeCore.hasSeenApfpvUdpPackets(handle)) 1 else 0
            )
        }
        val preferredCameraId = nativeState[0]
        val searchActive = nativeState[1] != 0
        val reconnectRequested = nativeState[2] != 0
        val hasSeenUdpPackets = nativeState[3] != 0

        if (searchActive) {
            handleMenuSearch(handle, cameraNetworks, currentSsid)
            return
        }

        val preferredNetwork = cameraNetworks.firstOrNull { it.deviceId == preferredCameraId }
        if (currentSsid != null) {
            if (reconnectRequested) {
                if (preferredNetwork != null && preferredNetwork.ssid != currentSsid) {
                    connectToCameraNetwork(handle, preferredNetwork.ssid)
                    return
                }

                val currentCameraId = parseCameraId(currentSsid)
                if (preferredCameraId != 0 && currentCameraId != preferredCameraId) {
                    disconnectFromCurrentCamera(handle)
                    updateLinkState(NativeCore.LINK_STATE_LOOKING_FOR_WIFI)
                    wifiManager.startScan()
                    return
                }
            }

            updateLinkState(
                if (hasSeenUdpPackets) NativeCore.LINK_STATE_NONE
                else NativeCore.LINK_STATE_CONNECTING_TO_STREAM
            )
            return
        }

        if (preferredNetwork != null) {
            connectToCameraNetwork(handle, preferredNetwork.ssid)
            return
        }

        if (preferredCameraId != 0 || reconnectRequested) {
            updateLinkState(NativeCore.LINK_STATE_LOOKING_FOR_WIFI)
            wifiManager.startScan()
            return
        }

        updateLinkState(NativeCore.LINK_STATE_NONE)
    }

    private fun findCameraNetworks(): List<CameraNetwork> {
        val scanResults = wifiManager.scanResults ?: return emptyList()
        val bestResults = scanResults
            .asSequence()
            .filter { !it.SSID.isNullOrBlank() }
            .filter { it.SSID.startsWith(CAMERA_SSID_PREFIX) }
            .mapNotNull { result ->
                val deviceId = parseCameraId(result.SSID) ?: return@mapNotNull null
                CameraNetwork(result.SSID, deviceId, result.level)
            }
            .groupBy { it.deviceId }
            .values
            .mapNotNull { candidates -> candidates.maxByOrNull { it.level } }
            .sortedBy { it.deviceId }
            .toList()

        if (bestResults.isNotEmpty()) {
            Log.i(LOG_TAG, "APFPV camera SSIDs detected: ${bestResults.joinToString { it.ssid }}")
        }
        return bestResults
    }

    private suspend fun handleMenuSearch(
        handle: Long,
        cameraNetworks: List<CameraNetwork>,
        currentSsid: String?
    ) {
        if (currentSsid != null) {
            disconnectFromCurrentCamera(handle)
            syncCameraState(cameraNetworks, null)
            wifiManager.startScan()
            return
        }

        if (cameraNetworks.size >= 2) {
            updateLinkState(NativeCore.LINK_STATE_NONE)
            return
        }

        if (cameraNetworks.size == 1) {
            val target = cameraNetworks.first()
            withContext(Dispatchers.Default) {
                NativeCore.setPreferredApfpvCameraId(handle, target.deviceId)
            }
            connectToCameraNetwork(handle, target.ssid)
            return
        }

        updateLinkState(NativeCore.LINK_STATE_LOOKING_FOR_WIFI)
        wifiManager.startScan()
    }

    private suspend fun disconnectFromCurrentCamera(handle: Long) {
        releaseRequestedNetwork()
        bindToNetwork(null)
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
            @Suppress("DEPRECATION")
            wifiManager.disconnect()
        }
        withContext(Dispatchers.Default) {
            NativeCore.stopUdpClient(handle)
            NativeCore.resetSession(handle)
            NativeCore.setLinkState(handle, NativeCore.LINK_STATE_LOOKING_FOR_WIFI)
        }
    }

    private suspend fun connectToCameraNetwork(handle: Long, ssid: String) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            connectWithNetworkSpecifier(handle, ssid)
            return
        }

        connectWithLegacyWifiManager(handle, ssid)
    }

    private fun connectWithNetworkSpecifier(handle: Long, ssid: String) {
        if (requestedSsid == ssid && requestedNetworkCallback != null) {
            return
        }

        updateLinkState(NativeCore.LINK_STATE_CONNECTING_TO_WIFI)
        releaseRequestedNetwork()

        val specifier = WifiNetworkSpecifier.Builder()
            .setSsid(ssid)
            .build()
        val request = NetworkRequest.Builder()
            .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
            .setNetworkSpecifier(specifier)
            .build()
        val callback = object : ConnectivityManager.NetworkCallback() {
            override fun onAvailable(network: Network) {
                Log.i(LOG_TAG, "APFPV Wi-Fi network available: $ssid")
                bindToNetwork(network)
                syncCameraState(findCameraNetworks(), ssid)
                activity.lifecycleScope.launch(Dispatchers.Default) {
                    NativeCore.stopUdpClient(handle)
                    NativeCore.resetSession(handle)
                    NativeCore.setLinkState(handle, NativeCore.LINK_STATE_CONNECTING_TO_STREAM)
                }
            }

            override fun onLost(network: Network) {
                Log.w(LOG_TAG, "APFPV Wi-Fi network lost: $ssid")
                if (boundNetwork == network) {
                    bindToNetwork(null)
                }
                if (requestedSsid == ssid) {
                    releaseRequestedNetwork()
                }
                syncCameraState(findCameraNetworks(), null)
                activity.lifecycleScope.launch(Dispatchers.Default) {
                    NativeCore.stopUdpClient(handle)
                    NativeCore.resetSession(handle)
                    NativeCore.setLinkState(handle, NativeCore.LINK_STATE_LOOKING_FOR_WIFI)
                }
            }

            override fun onUnavailable() {
                Log.w(LOG_TAG, "APFPV Wi-Fi network unavailable: $ssid")
                if (requestedSsid == ssid) {
                    releaseRequestedNetwork()
                }
                syncCameraState(findCameraNetworks(), null)
                activity.lifecycleScope.launch(Dispatchers.Default) {
                    NativeCore.stopUdpClient(handle)
                    NativeCore.resetSession(handle)
                    NativeCore.setLinkState(handle, NativeCore.LINK_STATE_LOOKING_FOR_WIFI)
                }
            }
        }

        requestedSsid = ssid
        requestedNetworkCallback = callback
        connectivityManager.requestNetwork(request, callback)
        Log.i(LOG_TAG, "Requested APFPV Wi-Fi network: $ssid")
    }

    @Suppress("DEPRECATION")
    private fun connectWithLegacyWifiManager(handle: Long, ssid: String) {
        val currentSsid = wifiManager.connectionInfo?.ssid?.trim('"')
        if (currentSsid == ssid) {
            syncCameraState(findCameraNetworks(), ssid)
            activity.lifecycleScope.launch(Dispatchers.Default) {
                NativeCore.setLinkState(handle, NativeCore.LINK_STATE_CONNECTING_TO_STREAM)
            }
            return
        }

        updateLinkState(NativeCore.LINK_STATE_CONNECTING_TO_WIFI)
        val configuration = WifiConfiguration().apply {
            SSID = "\"$ssid\""
            allowedKeyManagement.set(WifiConfiguration.KeyMgmt.NONE)
        }
        val networkId = wifiManager.addNetwork(configuration)
        if (networkId < 0) {
            Log.w(LOG_TAG, "Failed to add legacy APFPV Wi-Fi network: $ssid")
            return
        }

        wifiManager.disconnect()
        wifiManager.enableNetwork(networkId, true)
        wifiManager.reconnect()
        syncCameraState(findCameraNetworks(), ssid)
        activity.lifecycleScope.launch(Dispatchers.Default) {
            NativeCore.stopUdpClient(handle)
            NativeCore.resetSession(handle)
            NativeCore.setLinkState(handle, NativeCore.LINK_STATE_CONNECTING_TO_STREAM)
        }
        Log.i(LOG_TAG, "Connected to legacy APFPV Wi-Fi network: $ssid")
    }

    private fun bindToNetwork(network: Network?) {
        boundNetwork = network
        connectivityManager.bindProcessToNetwork(network)
    }

    private fun acquireWifiStreamingLock() {
        if (wifiStreamingLock.isHeld) {
            return
        }

        wifiStreamingLock.acquire()
        Log.i(LOG_TAG, "Acquired APFPV Wi-Fi streaming lock")
    }

    private fun releaseWifiStreamingLock() {
        if (!wifiStreamingLock.isHeld) {
            return
        }

        wifiStreamingLock.release()
        Log.i(LOG_TAG, "Released APFPV Wi-Fi streaming lock")
    }

    private fun releaseRequestedNetwork() {
        val callback = requestedNetworkCallback ?: return
        try {
            connectivityManager.unregisterNetworkCallback(callback)
        } catch (_: IllegalArgumentException) {
        }
        requestedNetworkCallback = null
        requestedSsid = null
    }

    private fun hasRequiredPermissions(): Boolean {
        val permissions = buildList {
            add(Manifest.permission.ACCESS_FINE_LOCATION)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                add(Manifest.permission.NEARBY_WIFI_DEVICES)
            }
        }
        return permissions.all { permission ->
            ContextCompat.checkSelfPermission(activity, permission) ==
                android.content.pm.PackageManager.PERMISSION_GRANTED
        }
    }

    private fun requestRequiredPermissions() {
        if (permissionRequestInFlight) {
            return
        }

        permissionRequestInFlight = true
        val permissions = buildList {
            add(Manifest.permission.ACCESS_FINE_LOCATION)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                add(Manifest.permission.NEARBY_WIFI_DEVICES)
            }
        }
        permissionLauncher.launch(permissions.toTypedArray())
    }

    private fun updateLinkState(state: Int) {
        val handle = currentNativeHandle()
        if (handle == 0L) {
            return
        }

        activity.lifecycleScope.launch(Dispatchers.Default) {
            NativeCore.setLinkState(handle, state)
        }
    }

    private fun syncCameraState(networks: List<CameraNetwork>, activeSsid: String?) {
        val handle = currentNativeHandle()
        if (handle == 0L) {
            return
        }

        val discoveredSsids = networks.map { it.ssid }.toTypedArray()
        val gsRssiDbm = currentConnectedCameraRssiDbm(activeSsid)
        activity.lifecycleScope.launch(Dispatchers.Default) {
            NativeCore.syncApfpvCameraState(handle, discoveredSsids, activeSsid, gsRssiDbm)
        }
    }

    private fun currentConnectedCameraSsid(): String? {
        val currentSsid = wifiManager.connectionInfo?.ssid?.trim('"') ?: return null
        return if (currentSsid.startsWith(CAMERA_SSID_PREFIX)) currentSsid else null
    }

    private fun currentConnectedCameraRssiDbm(activeSsid: String?): Int {
        if (activeSsid.isNullOrEmpty()) {
            return 0
        }

        @Suppress("DEPRECATION")
        val connectionInfo = wifiManager.connectionInfo ?: return 0
        val connectedSsid = connectionInfo.ssid?.trim('"') ?: return 0
        if (connectedSsid != activeSsid) {
            return 0
        }
        return connectionInfo.rssi
    }

    private fun parseCameraId(ssid: String): Int? {
        if (!ssid.startsWith(CAMERA_SSID_PREFIX)) {
            return null
        }
        return ssid.removePrefix(CAMERA_SSID_PREFIX).toIntOrNull(16)
    }

    private companion object {
        const val LOG_TAG = "ApfpvWifiController"
        const val CAMERA_SSID_PREFIX = "esp32cam-fpv-"
        const val SCAN_INTERVAL_MS = 3_000L
    }

    private fun getWifiStreamingLockMode(): Int {
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            WifiManager.WIFI_MODE_FULL_LOW_LATENCY
        } else {
            @Suppress("DEPRECATION")
            WifiManager.WIFI_MODE_FULL_HIGH_PERF
        }
    }
}
