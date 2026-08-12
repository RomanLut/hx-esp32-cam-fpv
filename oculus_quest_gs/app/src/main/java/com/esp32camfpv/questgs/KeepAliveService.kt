package com.esp32camfpv.questgs

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.IBinder
import android.os.PowerManager

//===================================================================================
//===================================================================================
// Keeps the Quest process awake without preventing APFPV Wi-Fi discovery scans.
class KeepAliveService : Service()
{
    companion object {
        const val CHANNEL_ID = "quest_gs_keepalive"
        const val NOTIFICATION_ID = 1001
    }

    private var wakeLock: PowerManager.WakeLock? = null

    override fun onCreate() {
        super.onCreate()
        createChannelIfNeeded()
        startForeground(NOTIFICATION_ID, buildNotification())
        acquireWakeLock()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (wakeLock?.isHeld != true) {
            acquireWakeLock()
        }
        return START_STICKY
    }

    override fun onDestroy() {
        releaseWakeLock()
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? {
        return null
    }

    private fun acquireWakeLock() {
        val pm = getSystemService(Context.POWER_SERVICE) as? PowerManager
        if (pm != null && wakeLock == null) {
            wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "quest_gs:keepalive")
            wakeLock?.setReferenceCounted(false)
            wakeLock?.acquire()
        }
    }

    private fun releaseWakeLock() {
        wakeLock?.let {
            if (it.isHeld) {
                it.release()
            }
        }
        wakeLock = null
    }

    private fun createChannelIfNeeded() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) {
            return
        }
        val mgr = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        val existing = mgr.getNotificationChannel(CHANNEL_ID)
        if (existing != null) {
            return
        }
        val channel = NotificationChannel(
            CHANNEL_ID,
            "Quest GS Keepalive",
            NotificationManager.IMPORTANCE_LOW
        )
        mgr.createNotificationChannel(channel)
    }

    private fun buildNotification(): Notification {
        val builder = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            Notification.Builder(this, CHANNEL_ID)
        } else {
            Notification.Builder(this)
        }
        return builder
            .setContentTitle("esp32-cam-fpv Quest")
            .setContentText("Running in background")
            .setSmallIcon(android.R.drawable.presence_video_online)
            .setOngoing(true)
            .build()
    }
}

