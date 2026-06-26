# FEC

Frames are sent using **Forward Error Correction Encoding**. Currently FEC is set to k=6, n=12 which means that bandwidth is doubled, but any 6 of 12 packets in block can be lost, and frame will still be recovered. It can be changed to 6/8 or 6/10 in OSD menu.

If single packet is lost and can not be recovered by FEC, the whole frame is lost. FEC is set to such high redundancy because lost frame at 30 fps looks very bad, even worse then overal image quality decrease caused by wasted bandwidth.
