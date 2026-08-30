#!/usr/bin/env python3
"""Verify the URSA Cine Immersive calibration -> equirect warp math.

Implements the Mei-Rives unified model (radial2ProjectionOffsetTangential2):
  d_cam (unit) -> m = (x/(z+xi), y/(z+xi)) -> radial(k1,k2) + tangential(p1,p2)
  -> pixel via K = [[fx, skew, cx], [0, fy, cy]].
Camera frame: x right, y down, z forward (CV convention, matches image axes).

Tests both quaternion conventions (R@d vs R.T@d) and measures inter-eye
vertical disparity on the warped equirect pair; the correct convention
minimizes vertical disparity (that is what dualRectification is for).
"""
import json
import numpy as np
from PIL import Image

SCRATCH = "/private/tmp/claude-501/-Users-matthewcelia-Documents-repos-LSVR-ResolveOFX-NDIOutput/b3ff2140-5692-4e1f-9d8a-f03c8c4cb41f/scratchpad"

cal = json.load(open(f"{SCRATCH}/OpticalProjectionData.txt"))
views = {v["viewDescription"]: v for v in cal["captureDevice"]["views"]}

def quat_to_R(q):
    x, y, z, w = q
    n = np.sqrt(x*x + y*y + z*z + w*w)
    x, y, z, w = x/n, y/n, z/n, w/n
    return np.array([
        [1-2*(y*y+z*z), 2*(x*y-z*w),   2*(x*z+y*w)],
        [2*(x*y+z*w),   1-2*(x*x+z*z), 2*(y*z-x*w)],
        [2*(x*z-y*w),   2*(y*z+x*w),   1-2*(x*x+y*y)],
    ])

def project_mei(d, intr):
    """d: (...,3) unit dirs in camera frame -> pixel coords (u,v), valid mask."""
    k1, k2, xi, p1, p2 = intr["distortions"]
    fx, fy = intr["fx"], intr["fy"]
    cx, cy = intr["centerX"], intr["centerY"]
    skew = intr["skew"]
    lim = np.deg2rad(intr["calibrationLimitRadialAngle"])

    x, y, z = d[..., 0], d[..., 1], d[..., 2]
    theta = np.arccos(np.clip(z, -1, 1))
    denom = z + xi
    valid = (denom > 1e-6) & (theta <= lim)
    denom = np.where(valid, denom, 1.0)
    mx, my = x / denom, y / denom
    r2 = mx*mx + my*my
    L = 1 + k1*r2 + k2*r2*r2
    xd = L*mx + 2*p1*mx*my + p2*(r2 + 2*mx*mx)
    yd = L*my + p1*(r2 + 2*my*my) + 2*p2*mx*my
    u = fx*xd + skew*yd + cx
    v = fy*yd + cy
    return u, v, valid

def equirect_dirs(size, hfov_deg=180.0, vfov_deg=180.0):
    """Unit dirs for each output pixel, x right / y down / z forward."""
    h = w = size
    us = (np.arange(w) + 0.5) / w
    vs = (np.arange(h) + 0.5) / h
    uu, vv = np.meshgrid(us, vs)
    lon = (uu - 0.5) * np.deg2rad(hfov_deg)   # + right
    lat = (0.5 - vv) * np.deg2rad(vfov_deg)   # + up
    d = np.stack([
        np.cos(lat) * np.sin(lon),
        -np.sin(lat),
        np.cos(lat) * np.cos(lon),
    ], axis=-1)
    return d

def warp_eye(img, view, R_mode, out_size=1024):
    intr = view["intrinsics"]
    W_cal, H_cal = view["imageSize"]
    R = quat_to_R(view["extrinsics"][0]["quat"])
    d = equirect_dirs(out_size)
    if R_mode == "R":
        dc = d @ R.T          # d_cam = R @ d_rig  (row-vector form)
    else:
        dc = d @ R            # d_cam = R.T @ d_rig
    u, v, valid = project_mei(dc, intr)
    # normalize by calibration size, then scale into the actual decoded image
    H_img, W_img = img.shape[:2]
    sx = u / W_cal * W_img
    sy = v / H_cal * H_img
    xi_ = np.clip(sx.astype(np.int32), 0, W_img - 1)
    yi_ = np.clip(sy.astype(np.int32), 0, H_img - 1)
    out = img[yi_, xi_].astype(np.float32)
    out[~valid] = 0
    return out.astype(np.uint8)

def load_ppm(path):
    return np.asarray(Image.open(path))

def band_vshift(L, R, y0, y1, x0, x1, search=8):
    """Best vertical shift of R vs L for a patch, by SSD over gray images."""
    gl = L[..., :3].mean(axis=-1)
    gr = R[..., :3].mean(axis=-1)
    ref = gl[y0:y1, x0:x1]
    best, bestdy = None, 0
    for dy in range(-search, search + 1):
        cand = gr[y0+dy:y1+dy, x0:x1]
        ssd = np.mean((ref - cand) ** 2)
        if best is None or ssd < best:
            best, bestdy = ssd, dy
    return bestdy

imgL = load_ppm(f"{SCRATCH}/eye_left.ppm")
imgR = load_ppm(f"{SCRATCH}/eye_right.ppm")
print("decoded:", imgL.shape, imgR.shape)

for mode in ["R", "Rt"]:
    eqL = warp_eye(imgL, views["left"], mode)
    eqR = warp_eye(imgR, views["right"], mode)
    Image.fromarray(eqL).save(f"{SCRATCH}/equirect_left_{mode}.png")
    Image.fromarray(eqR).save(f"{SCRATCH}/equirect_right_{mode}.png")
    # vertical disparity over several central patches (avoid edges/mask)
    shifts = []
    for (y0, y1, x0, x1) in [
        (380, 560, 300, 480), (380, 560, 540, 720),
        (560, 740, 300, 480), (560, 740, 540, 720),
        (300, 460, 420, 620), (620, 780, 420, 620),
    ]:
        shifts.append(band_vshift(eqL, eqR, y0, y1, x0, x1))
    print(f"mode {mode}: per-patch vertical shifts (px @1024): {shifts}  mean|dy|={np.mean(np.abs(shifts)):.2f}")

# also save a raw fisheye reference and an anaglyph of the better mode for eyeballing
Image.fromarray(imgL[::3, ::3]).save(f"{SCRATCH}/fisheye_left_small.png")
print("done")
