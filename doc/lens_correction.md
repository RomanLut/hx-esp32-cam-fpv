# Lens Correction

## What is Lens Correction?

Every camera lens introduces some optical distortion. Wide-angle lenses — common in FPV cameras — typically show **barrel distortion**: straight lines near the edges of the frame appear to curve outward. This can make the video look like it was shot through a fisheye lens.

Lens correction compensates for this distortion by remapping each pixel in the frame so that straight lines in real life appear straight on screen. The correction is applied in real time on the ground station (GS) using the GPU.

---

## Coefficients and What They Mean

The correction is described by a set of numbers called **coefficients**. These come from the standard OpenCV camera model:

### Camera Matrix (Intrinsics)

| Coefficient | Description |
|---|---|
| `fxnorm` | Normalized focal length in the horizontal direction |
| `fynorm` | Normalized focal length in the vertical direction |
| `cxnorm` | Normalized X position of the optical center (principal point) |
| `cynorm` | Normalized Y position of the optical center (principal point) |

These describe the geometry of the lens — roughly where the center of the image is and how much the lens "zooms". Values are stored normalized (divided by image resolution), so a value of 0.5 for `cxnorm` and `cynorm` means the optical center is at the middle of the frame.

### Distortion Coefficients

| Coefficient | Description |
|---|---|
| `k1`, `k2`, `k3` | Radial distortion — how much the image bends outward or inward at increasing distances from the center |
| `p1`, `p2` | Tangential distortion — a small correction for lenses that are not perfectly aligned with the image sensor |

For most FPV cameras, `k1` is the most significant. It is negative for barrel distortion (fisheye-like effect). The other coefficients are usually small.

---

## One Set of Coefficients Per Resolution

The coefficients are computed for — and only valid at — the **resolution at which calibration was performed**. If you change the camera resolution (e.g. from 640×480 to 1280×720), you need to recalibrate.

---

## Where to Enable Lens Correction

Lens correction is controlled from the OSD menu on the ground station:

```
Main Menu
└── Camera Settings
    └── Lens Correction...
        ├── Enabled: On / Off
        ├── Coefficients...    (view and manually edit values)
        └── Calibrate...       (run automatic calibration)
```

Toggle **Enabled** to turn correction on or off at any time. The coefficients remain saved even when disabled.

---

## Where Parameters are Stored

Currently, all lens correction parameters are stored on the **ground station** in the `gs.ini` settings file. This means:

- Parameters are tied to the GS installation, not the camera.
- If you use the same camera with a different GS device, you need to recalibrate (or copy `gs.ini`).

In the future, parameters will be stored on the camera itself, so calibration follows the camera wherever it is used.

---

## How to Calibrate

Calibration uses a standard **chessboard pattern** (a printed grid of black and white squares). You hold this in front of the camera while the GS captures a series of frames, then OpenCV computes the correction coefficients automatically.

Pattern: https://github.com/opencv/opencv/blob/4.x/doc/pattern.png


### Steps

1. Open the OSD menu and navigate to **Camera Settings → Lens Correction → Calibrate...**.
2. A full-screen overlay appears with instructions.
3. Hold the chessboard so it covers **more than 40% of the screen**.
4. Press **Enter** (or any REC button) to capture a frame. The overlay shows how many frames have been captured out of the required 10.
5. Between each capture, **move and tilt the chessboard** — vary the angle, distance, and position, and make sure the board appears near the edges of the screen at some point. Diverse angles give a more accurate calibration.
6. After 10 frames are captured, calibration runs automatically. The computed coefficients are saved and applied immediately.
7. You return to the Lens Correction menu. **Enabled** is turned on automatically.

### Tips for Good Calibration

- Use good, even lighting. Avoid glare on the printed chessboard.
- Keep the board flat and in focus.
- Cover different parts of the frame: center, corners, and edges.
- Tilt the board at various angles — do not always hold it parallel to the camera.
- After calibration, verify by watching the video with correction enabled. Grid lines and horizons should appear straight.
