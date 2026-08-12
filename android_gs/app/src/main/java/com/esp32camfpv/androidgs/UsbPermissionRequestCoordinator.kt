package com.esp32camfpv.androidgs

//===================================================================================
//===================================================================================
// Serializes Android USB permission dialogs across all in-process USB controllers.
object UsbPermissionRequestCoordinator
{
    private var owner: String? = null
    private var deviceName: String? = null

    @Synchronized
    fun tryAcquire(requestOwner: String, requestDeviceName: String): Boolean
    {
        if (owner != null)
        {
            return owner == requestOwner && deviceName == requestDeviceName
        }

        owner = requestOwner
        deviceName = requestDeviceName
        return true
    }

    @Synchronized
    fun release(requestOwner: String)
    {
        if (owner == requestOwner)
        {
            owner = null
            deviceName = null
        }
    }
}
