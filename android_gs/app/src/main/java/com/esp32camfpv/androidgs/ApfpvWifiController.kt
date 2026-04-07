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
    private val wifiManager =
        activity.applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
    private val connectivityManager =
        activity.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager

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
        releaseRequestedNetwork()
        bindToNetwork(null)
    }

    private suspend fun syncNow() {
        val handle = currentNativeHandle()
        if (handle == 0L) {
            releaseRequestedNetwork()
            bindToNetwork(null)
            return
        }

        val activeTransportKind = withContext(Dispatchers.Default) {
            NativeCore.getActiveTransportKind(handle)
        }
        if (activeTransportKind != NativeCore.TRANSPORT_APFPV) {
            releaseRequestedNetwork()
            bindToNetwork(null)
            return
        }

        if (!hasRequiredPermissions()) {
            requestRequiredPermissions()
            return
        }

        val matchingSsid = withContext(Dispatchers.Default) {
            findMatchingCameraSsid()
        }
        if (matchingSsid == null) {
            wifiManager.startScan()
            return
        }

        connectToCameraNetwork(handle, matchingSsid)
    }

    private fun findMatchingCameraSsid(): String? {
        val scanResults = wifiManager.scanResults ?: return null
        val bestResult = scanResults
            .asSequence()
            .filter { !it.SSID.isNullOrBlank() }
            .filter { it.SSID.startsWith(CAMERA_SSID_PREFIX) }
            .maxByOrNull { it.level }

        if (bestResult != null) {
            Log.i(LOG_TAG, "APFPV camera SSID detected: ${bestResult.SSID}")
        }
        return bestResult?.SSID
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
                activity.lifecycleScope.launch(Dispatchers.Default) {
                    NativeCore.stopUdpClient(handle)
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
                activity.lifecycleScope.launch(Dispatchers.Default) {
                    NativeCore.stopUdpClient(handle)
                }
            }

            override fun onUnavailable() {
                Log.w(LOG_TAG, "APFPV Wi-Fi network unavailable: $ssid")
                if (requestedSsid == ssid) {
                    releaseRequestedNetwork()
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
        activity.lifecycleScope.launch(Dispatchers.Default) {
            NativeCore.stopUdpClient(handle)
        }
        Log.i(LOG_TAG, "Connected to legacy APFPV Wi-Fi network: $ssid")
    }

    private fun bindToNetwork(network: Network?) {
        boundNetwork = network
        connectivityManager.bindProcessToNetwork(network)
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

    private companion object {
        const val LOG_TAG = "ApfpvWifiController"
        const val CAMERA_SSID_PREFIX = "esp32cam-fpv-"
        const val SCAN_INTERVAL_MS = 3_000L
    }
}
