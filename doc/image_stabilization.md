# Image Stabilization

## What is Image Stabilization?

Image stabilization reduces visible shake in the received video by estimating motion between consecutive frames and compensating for that motion at render time.

The GS implementation tracks visual features inside a central region of interest using Lucas-Kanade optical flow with RANSAC-based affine fitting. 
Measured frame-to-frame translation and rotation are accumulated into a trajectory that is smoothed by exponential decay. 
The resulting offset is applied as an affine transform when the frame is rendered.

## Parameters

| Parameter | Default | Meaning |
|---|---:|---|
| `enabled` | Off | Turns image stabilization on or off. When disabled, frames pass through unchanged and no OpenCV work is done. |
| `rc_channel` | `None` | RC control channel (1–18) that toggles stabilization on or off from the transmitter. Set to None to disable RC control. |
| `roi_divisor` | `4.0` | Controls the size of the central region used for motion tracking. Larger values use a larger ROI because the cropped margins are smaller; smaller values use a tighter center crop. Tracking the center avoids OSD edges and moving border artifacts influencing motion estimation. |
| `zoom` | `1.15` | Magnification factor applied to the stabilized image to hide black or repeated borders created by frame warping. Values above `1.0` zoom in more and hide more border motion; values closer to `1.0` preserve more field of view but may expose edges. Range: `1.0`–`2.0`. |
| `stabilization_decay` | `0.05` | Fraction of trajectory offset removed per 30 fps frame. Higher values return the image center more quickly after a sudden camera movement; lower values smooth more but allow larger accumulated offsets. Range: `0.01`–`1.0`. |
| `limit_release_boost` | `0.5` | Extra decay multiplier applied when the trajectory approaches the safety limits (±50% of screen size, ±45°). Higher values snap back from the limits more aggressively, preventing the image from staying pinned near a boundary. Range: `0.0`–`10.0`. |
| `debug` | Off | Draws the tracking ROI outline and per-feature motion vectors on screen. Green vectors indicate tracked points; red indicates lost tracks. Useful for diagnosing poor tracking scenes. |

## Notes

- Stabilization is most reliable when the center of the image contains stable scene detail.
