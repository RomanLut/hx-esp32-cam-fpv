package com.esp32camfpv.gscommon

import android.Manifest
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.net.wifi.WifiConfiguration
import android.net.wifi.WifiManager
import android.net.wifi.WifiNetworkSpecifier
import android.os.Build
import android.os.SystemClock
import android.provider.Settings
import android.util.Log
import androidx.activity.ComponentActivity
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

//===================================================================================
//===================================================================================
// Discovers APFPV cameras and manages the Wi-Fi connection and streaming lock.
//
// requestVrFocusRecovery is a no-op on the phone build; the Quest build uses it to
// reclaim VR input after a system approval dialog has taken focus away.
class ApfpvWifiController(
    private val activity: ComponentActivity,
    private val currentNativeHandle: () -> Long,
    private val requestVrFocusRecovery: (String) -> Unit = {}
)
{
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
            recoverVrFocus("wifiPermissionResult")
            if (hasRequiredPermissions()) {
                activity.lifecycleScope.launch(Dispatchers.Main) {
                    syncNow()
                }
            }
        }

    private var syncJob: Job? = null
    private var requestedSsid: String? = null
    private var requestedNetworkCallback: ConnectivityManager.NetworkCallback? = null
    private var lastScanRequestElapsedMs: Long = 0L
    private var lastPermissionPromptElapsedMs: Long = 0L
    private var boundNetwork: Network? = null
    private var permissionRequestInFlight = false
    private var lastAirApfpvModeEnabled: Boolean? = null
    private var awaitingMenuCameraSelection = false

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
        lastAirApfpvModeEnabled = null
        awaitingMenuCameraSelection = false
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
            lastAirApfpvModeEnabled = null
            awaitingMenuCameraSelection = false
            syncCameraState(emptyList(), null)
            releaseRequestedNetwork()
            releaseWifiStreamingLock()
            bindToNetwork(null)
            return
        }

        val airApfpvModeEnabled = withContext(Dispatchers.Default) {
            NativeCore.isAirApfpvModeEnabled(handle)
        }
        val apfpvModeChanged = lastAirApfpvModeEnabled != null && lastAirApfpvModeEnabled != airApfpvModeEnabled
        lastAirApfpvModeEnabled = airApfpvModeEnabled
        if (apfpvModeChanged) {
            // Android scanResults is a platform cache. Clear the published camera list first so
            // the menu does not keep stale APFPV SSIDs after the air unit switches mode. Quest 2
            // cannot scan while its low-latency Wi-Fi lock is held, so leave it released until a
            // camera connection is active again.
            syncCameraState(emptyList(), null)
            awaitingMenuCameraSelection = false
            releaseWifiStreamingLock()
            requestWifiScan("apfpvModeChanged", force = true)
            return
        }

        val explicitPromptRequested = withContext(Dispatchers.Default) {
            NativeCore.consumeApfpvWifiScanPermissionPromptRequest(handle)
        }

        if (!hasRequiredPermissions()) {
            releaseWifiStreamingLock()
            syncScanPermissionError(handle, true)
            requestRequiredPermissions(force = explicitPromptRequested)
            return
        }
        syncScanPermissionError(handle, false)

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
            )
        }
        val preferredCameraId = nativeState[0]
        val searchActive = nativeState[1] != 0
        val reconnectRequested = nativeState[2] != 0

        if (searchActive) {
            // Explicit Search must leave the ground station disconnected until the user chooses
            // a rendered Connect-to row. Otherwise the retained preferred ID immediately opens
            // another system approval dialog and hides the search results.
            awaitingMenuCameraSelection = true
            releaseWifiStreamingLock()
            handleMenuSearch(handle, cameraNetworks, currentSsid)
            return
        }

        if (reconnectRequested) {
            awaitingMenuCameraSelection = false
        }

        val preferredNetwork = cameraNetworks.firstOrNull { it.deviceId == preferredCameraId }
        if (currentSsid != null) {
            if (reconnectRequested) {
                releaseWifiStreamingLock()
                if (preferredNetwork != null && preferredNetwork.ssid != currentSsid) {
                    connectToCameraNetwork(handle, preferredNetwork.ssid)
                    return
                }

                val currentCameraId = parseCameraId(currentSsid)
                if (preferredCameraId != 0 && currentCameraId != preferredCameraId) {
                    disconnectFromCurrentCamera(handle)
                    requestWifiScan("preferredMismatch")
                    return
                }
            } else {
                acquireWifiStreamingLock()
            }

            syncCameraState(cameraNetworks, currentSsid)
            return
        }

        releaseWifiStreamingLock()
        if (awaitingMenuCameraSelection) {
            syncCameraState(cameraNetworks, null)
            return
        }
        if (preferredNetwork != null) {
            connectToCameraNetwork(handle, preferredNetwork.ssid)
            return
        }

        if (preferredCameraId != 0 || reconnectRequested) {
            requestWifiScan("preferredOrReconnect")
            return
        }

        syncCameraState(cameraNetworks, null)
    }

    private fun findCameraNetworks(): List<CameraNetwork> {
        val scanResults = try {
            wifiManager.scanResults
        } catch (securityException: SecurityException) {
            Log.w(LOG_TAG, "Wi-Fi scan permission error while reading scan results", securityException)
            val handle = currentNativeHandle()
            if (handle != 0L) {
                syncScanPermissionError(handle, true)
            }
            return emptyList()
        } ?: return emptyList()
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

    //===================================================================================
    //===================================================================================
    // Advances an explicit menu search. A single discovered camera is connected directly;
    // two or more are published as Connect-to rows for the user to choose between.
    private suspend fun handleMenuSearch(
        handle: Long,
        cameraNetworks: List<CameraNetwork>,
        currentSsid: String?
    ) {
        if (currentSsid != null) {
            disconnectFromCurrentCamera(handle)
            syncCameraState(cameraNetworks, null)
            requestWifiScan("menuSearchDisconnect")
            return
        }

        // Always request a fresh scan for an explicit menu search. Cached Android results can
        // describe cameras that have moved channel or are no longer powered on.
        requestWifiScan("menuSearch")

        if (cameraNetworks.size >= 2) {
            syncCameraState(cameraNetworks, null)
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

        requestWifiScan("menuSearchNoTargets")
    }

    private suspend fun disconnectFromCurrentCamera(handle: Long) {
        releaseWifiStreamingLock()
        releaseRequestedNetwork()
        bindToNetwork(null)
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
            @Suppress("DEPRECATION")
            wifiManager.disconnect()
        }
        withContext(Dispatchers.Default) {
            NativeCore.stopUdpClient(handle)
            NativeCore.resetSession(handle)
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

        releaseWifiStreamingLock()
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
                acquireWifiStreamingLock()
                syncCameraState(findCameraNetworks(), ssid)
                recoverVrFocus("apfpvNetworkAvailable")
                activity.lifecycleScope.launch(Dispatchers.Default) {
                    NativeCore.stopUdpClient(handle)
                    NativeCore.resetSession(handle)
                }
            }

            override fun onLost(network: Network) {
                Log.w(LOG_TAG, "APFPV Wi-Fi network lost: $ssid")
                releaseWifiStreamingLock()
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
                }
            }

            override fun onUnavailable() {
                Log.w(LOG_TAG, "APFPV Wi-Fi network unavailable: $ssid")
                releaseWifiStreamingLock()
                if (requestedSsid == ssid) {
                    releaseRequestedNetwork()
                }
                // A rejected or stale system approval must not immediately open the same
                // dialog again. Leave reconnection to an explicit menu selection.
                awaitingMenuCameraSelection = true
                syncCameraState(findCameraNetworks(), null)
                recoverVrFocus("apfpvNetworkUnavailable")
                activity.lifecycleScope.launch(Dispatchers.Default) {
                    NativeCore.stopUdpClient(handle)
                    NativeCore.resetSession(handle)
                }
            }
        }

        requestedSsid = ssid
        requestedNetworkCallback = callback
        // Quest can dismiss a stale Horizon approval activity without delivering a result.
        // The bounded request guarantees onUnavailable() eventually restores VR focus.
        connectivityManager.requestNetwork(request, callback, NETWORK_REQUEST_TIMEOUT_MS)
        Log.i(LOG_TAG, "Requested APFPV Wi-Fi network: $ssid")
    }

    @Suppress("DEPRECATION")
    private fun connectWithLegacyWifiManager(handle: Long, ssid: String) {
        val currentSsid = wifiManager.connectionInfo?.ssid?.trim('"')
        if (currentSsid == ssid) {
            syncCameraState(findCameraNetworks(), ssid)
            return
        }

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

        // Quest 2 Horizon OS 14/QCA6390 aborts every off-channel Wi-Fi scan while this
        // low-latency lock is held. Do not acquire it before APFPV discovery completes.
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

    private fun syncScanPermissionError(handle: Long, enabled: Boolean) {
        activity.lifecycleScope.launch(Dispatchers.Default) {
            NativeCore.setApfpvWifiScanPermissionError(handle, enabled)
        }
    }

    private fun requestWifiScan(reason: String, force: Boolean = false): Boolean {
        // Quest 2/QCA6390 reports NL80211 scan-aborted while this lock is held. Release it even
        // when Android throttles this particular request so the next framework scan can run.
        releaseWifiStreamingLock()

        val now = SystemClock.elapsedRealtime()
        if (!force && lastScanRequestElapsedMs != 0L &&
            (now - lastScanRequestElapsedMs) < SCAN_REQUEST_MIN_INTERVAL_MS) {
            return false
        }

        if (!hasRequiredPermissions()) {
            val handle = currentNativeHandle()
            if (handle != 0L) {
                syncScanPermissionError(handle, true)
            }
            return false
        }

        return try {
            val started = wifiManager.startScan()
            lastScanRequestElapsedMs = now
            if (!started) {
                Log.w(LOG_TAG, "Wi-Fi scan request rejected by framework, reason=$reason")
            }
            started
        } catch (securityException: SecurityException) {
            val handle = currentNativeHandle()
            if (handle != 0L) {
                syncScanPermissionError(handle, true)
            }
            Log.w(LOG_TAG, "Wi-Fi scan permission error, reason=$reason", securityException)
            false
        }
    }

    private fun requestRequiredPermissions(force: Boolean = false) {
        if (!force) {
            return
        }

        val now = SystemClock.elapsedRealtime()
        if (lastPermissionPromptElapsedMs != 0L &&
            (now - lastPermissionPromptElapsedMs) < PERMISSION_PROMPT_MIN_INTERVAL_MS) {
            return
        }

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
        lastPermissionPromptElapsedMs = now
        Log.i(LOG_TAG, "Requesting Wi-Fi scan permissions due to explicit APFPV search action")
        if (requiresManualPermissionGrant(permissions)) {
            Log.w(LOG_TAG, "Permissions appear permanently denied; opening app settings")
            openAppPermissionSettings()
        }
        permissionLauncher.launch(permissions.toTypedArray())
    }

    private fun requiresManualPermissionGrant(permissions: List<String>): Boolean {
        return permissions.any { permission ->
            ContextCompat.checkSelfPermission(activity, permission) !=
                android.content.pm.PackageManager.PERMISSION_GRANTED &&
                !ActivityCompat.shouldShowRequestPermissionRationale(activity, permission)
        }
    }

    private fun openAppPermissionSettings() {
        try {
            val intent = Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS).apply {
                data = Uri.fromParts("package", activity.packageName, null)
                addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            }
            activity.startActivity(intent)
        } catch (t: Throwable) {
            Log.w(LOG_TAG, "Failed to open app settings: ${t.message}")
        }
    }

    private fun syncCameraState(networks: List<CameraNetwork>, activeSsid: String?) {
        val handle = currentNativeHandle()
        if (handle == 0L) {
            return
        }

        val discoveredSsids = networks.map { it.ssid }.toTypedArray()
        val gsRssiDbm = currentConnectedCameraRssiDbm(activeSsid)
        val connectingSsid = if (activeSsid == null) requestedSsid else null
        activity.lifecycleScope.launch(Dispatchers.Default) {
            NativeCore.syncApfpvCameraState(handle, discoveredSsids, activeSsid, gsRssiDbm, connectingSsid)
        }
    }

    private fun recoverVrFocus(reason: String)
    {
        activity.runOnUiThread {
            requestVrFocusRecovery(reason)
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
        const val SCAN_REQUEST_MIN_INTERVAL_MS = 30_000L
        const val PERMISSION_PROMPT_MIN_INTERVAL_MS = 30_000L
        const val NETWORK_REQUEST_TIMEOUT_MS = 120_000
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
