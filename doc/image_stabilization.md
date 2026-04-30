# Image Stabilization

## What is Image Stabilization?

Image stabilization reduces visible shake in the received video by estimating motion between consecutive frames and warping the displayed image to compensate for that motion.

The current GS implementation tracks visual features inside a central region of interest, estimates a partial affine transform between frames, smooths the measured camera motion, then warps the previous frame before display. This adds about one frame of latency.

## Parameters

| Parameter | Default | Meaning |
|---|---:|---|
| `enabled` | Off | Turns image stabilization on or off. When disabled, frames pass through unchanged and no OpenCV stabilization work is done. |
| `roi_divisor` | `3.5` | Controls the size of the central region used for motion tracking. Larger values use a larger ROI because the cropped margins are smaller; smaller values use a tighter center crop. Tracking the center avoids OSD edges and moving border artifacts influencing motion estimation. |
| `zoom_factor` | `0.9` | Scales the stabilized image to hide black or repeated borders created by frame warping. Lower values crop/zoom more and hide more border motion; values closer to `1.0` preserve more field of view but may expose edges. |
| `process_var` | `0.03` | Process variance used by the motion smoothing filter. Higher values let the smoothed camera path react faster to new motion; lower values produce stronger smoothing but can feel more delayed. |
| `measurement_var` | `2.0` | Measurement variance used by the motion smoothing filter. Higher values trust measured frame-to-frame motion less and smooth more; lower values trust measurements more and follow motion more closely. |
| `max_corners` | `400` | Maximum number of feature points OpenCV tries to detect in the ROI. Higher values can improve motion estimation in difficult scenes but cost more CPU. |
| `quality_level` | `0.01` | Minimum feature quality for corner detection. Lower values accept weaker features; higher values keep only stronger features. |
| `min_distance` | `30` | Minimum pixel distance between detected feature points. Higher values spread features farther apart; lower values allow denser tracking but can cluster points on the same object. |

## Notes

- Stabilization is most reliable when the center of the image contains stable scene detail.
- It can fail or fall back to pass-through on frames with too few trackable points, heavy blur, or very large frame-to-frame motion.
- The first frame after enabling, reset, or resolution change is used as a reference and is not stabilized.
