# Oculus Quest Ground Station

Native OpenXR Oculus Quest 2/3 GS application supports both RAW Broadcast mode with an RTL8812AU adapter, and APFPV mode using the built-in Oculus Quest Wi-Fi.

Any USB Wi-Fi adapter based on the RTL8812AU chipset should work. For example, the adapter included with the Eachine Sphere Link.

Currently only single adapter is supported.

In the future, this setup may become the recommended option, because the lens and screen quality of the Oculus Quest is significantly better than that of most FPV goggles.

The main downside of the Oculus Quest is the need to carry VR controller, since there is currently no other practical way to navigate the Oculus system menus. Hand tracking can be used instead, but its performance is poor under direct sunlight.

To solve this properly, support for dual adapters, hardware navigation buttons, and a dedicated 3D-printed GS unit still need to be developed.

**USB Serial** in **OTG USB port** can be used to transfer Mavlink stream.

![oculus gs](/doc/images/oculus_gs.jpg "oculus gs")
