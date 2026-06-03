package com.esp32camfpv.questgs

import android.content.Intent
import android.os.Bundle
import androidx.activity.ComponentActivity

class UsbAttachActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val launchIntent = Intent(this, MainActivity::class.java).apply {
            action = intent?.action
            putExtras(intent ?: Intent())
            addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP or Intent.FLAG_ACTIVITY_CLEAR_TOP)
        }
        startActivity(launchIntent)
        finish()
    }
}
