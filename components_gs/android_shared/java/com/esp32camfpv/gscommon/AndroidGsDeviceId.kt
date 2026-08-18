package com.esp32camfpv.gscommon

import android.content.Context
import android.os.Build
import android.provider.Settings
import java.nio.charset.StandardCharsets
import java.security.MessageDigest

//===================================================================================
//===================================================================================
// Generates a stable nonzero 16-bit GS ID from Android device properties.
object AndroidGsDeviceId
{
    fun fromContext(context: Context): Int
    {
        val androidId = Settings.Secure.getString(
            context.contentResolver,
            Settings.Secure.ANDROID_ID
        ).orEmpty()
        val identity = listOf(
            "esp32-cam-fpv-gs",
            androidId,
            Build.BOARD,
            Build.DEVICE,
            Build.MANUFACTURER,
            Build.MODEL
        ).joinToString("|")
        val digest = MessageDigest.getInstance("SHA-256")
            .digest(identity.toByteArray(StandardCharsets.UTF_8))
        val deviceId = ((digest[0].toInt() and 0xff) shl 8) or
            (digest[1].toInt() and 0xff)
        return if (deviceId == 0) 1 else deviceId
    }
}
