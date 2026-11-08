#!/usr/bin/env python3
"""cornertest_measure.py PHOTO.jpg [--rotate180] [--crop x0,y0,x1,y1] [--debug out.png]

Measures the rounded corners of the PicoBoy Color Plus panel from a camera photo of
tests/pbc/cornertest (grey visible area on the black glass). Steps:
  1. threshold the grey area against the black glass (midway between their levels),
  2. fit the four straight edges away from the corners (least squares), which gives the
     position, rotation and scale of the 240 x 280 visible area,
  3. map each corner's boundary into panel pixels and fit a circle tangent to both edges
     (radius R, centre at (R, R) from the corner): one radius per corner.
Robust to the PicoBoy shifting or turning a little between photos."""
import argparse, math, sys
import numpy as np
from PIL import Image

ap = argparse.ArgumentParser()
ap.add_argument("photo")
ap.add_argument("--rotate180", action="store_true")
ap.add_argument("--crop", default=None)
ap.add_argument("--debug", default=None)
ap.add_argument("--arcs", action="store_true", help="photo of the arcs pattern: test the arcs")
ap.add_argument("--r", type=int, default=0, help="with --arcs: the radius shown (else read from the bits)")
a = ap.parse_args()

im = Image.open(a.photo).convert("RGB")
if a.rotate180:
    im = im.rotate(180)
if a.crop:
    x0, y0, x1, y1 = map(int, a.crop.split(","))
    im = im.crop((x0, y0, x1, y1))
g = np.asarray(im.convert("L"), dtype=np.float32)
H, W = g.shape

# levels: the grey area is the bright middle; the glass around it dark
centre = g[H // 3: 2 * H // 3, W // 3: 2 * W // 3]
bright = np.median(centre)
dark = np.percentile(g, 8)
mask = g > (bright + dark) / 2

# the largest bright blob containing the centre: flood from the centre along rows/cols
cy, cx = H // 2, W // 2
rows = np.where(mask[:, cx])[0]
cols = np.where(mask[cy, :])[0]

def run_around(idx, c):
    # contiguous run of indices containing c
    if c not in set(idx.tolist()):
        return None
    lo = hi = c
    s = set(idx.tolist())
    while lo - 1 in s: lo -= 1
    while hi + 1 in s: hi += 1
    return lo, hi

top0, bot0 = run_around(rows, cy)
left0, right0 = run_around(cols, cx)

def edge_points(axis, side, lo, hi):
    """Sub-pixel boundary along scan lines between lo..hi (the middle part of an edge)."""
    pts = []
    thr = (bright + dark) / 2
    for t in range(lo, hi):
        line = g[:, t] if axis == "v" else g[t, :]
        c = cy if axis == "v" else cx
        if side < 0:   # walk from the centre towards index 0
            i = c
            while i > 0 and line[i] > thr: i -= 1
            # linear interpolation between i and i+1
            f = (thr - line[i]) / max(line[i + 1] - line[i], 1e-3)
            pts.append((t, i + f))
        else:
            i = c
            while i < len(line) - 1 and line[i] > thr: i += 1
            f = (line[i - 1] - thr) / max(line[i - 1] - line[i], 1e-3)
            pts.append((t, i - 1 + f))
    return np.array(pts)

wx, hy = right0 - left0, bot0 - top0
# the middle of each edge, well away from the corners (the middle third: in the arcs test the
# corners are black squares 72 pixels wide)
top = edge_points("v", -1, left0 + wx // 3, right0 - wx // 3)      # (x, y)
bot = edge_points("v", +1, left0 + wx // 3, right0 - wx // 3)
lef = edge_points("h", -1, top0 + hy // 3, bot0 - hy // 3)         # (y, x)
rig = edge_points("h", +1, top0 + hy // 3, bot0 - hy // 3)

def fit(p):
    # y = a*x + b
    A = np.vstack([p[:, 0], np.ones(len(p))]).T
    return np.linalg.lstsq(A, p[:, 1], rcond=None)[0]

ft, fb, fl, fr = fit(top), fit(bot), fit(lef), fit(rig)

def isect(h, v):
    # h: y = a x + b ; v: x = c y + d
    a1, b1 = h; c1, d1 = v
    y = (a1 * d1 + b1) / (1 - a1 * c1)
    return np.array([c1 * y + d1, y])

TL, TR = isect(ft, fl), isect(ft, fr)
BL, BR = isect(fb, fl), isect(fb, fr)
# panel pixel (u, v) -> photo, bilinear between the four corners (the outer edges of the
# outermost pixels: u in 0..240, v in 0..280)
def to_photo(u, v):
    s, t = u / 240.0, v / 280.0
    return (1 - s) * (1 - t) * TL + s * (1 - t) * TR + (1 - s) * t * BL + s * t * BR

def to_panel(p):
    # invert by Newton iterations on the bilinear map
    u, v = 120.0, 140.0
    for _ in range(20):
        q = to_photo(u, v)
        e = 1e-3
        du = (to_photo(u + e, v) - q) / e
        dv = (to_photo(u, v + e) - q) / e
        J = np.array([du, dv]).T
        d = np.linalg.solve(J, p - q)
        u += d[0]; v += d[1]
        if abs(d[0]) + abs(d[1]) < 1e-6: break
    return u, v

px_per_lcd = (np.linalg.norm(TR - TL) / 240 + np.linalg.norm(BL - TL) / 280) / 2
if not a.arcs: print(f"photo: visible area {np.linalg.norm(TR-TL):.0f} x {np.linalg.norm(BL-TL):.0f} px, "
      f"{px_per_lcd:.2f} photo px per panel pixel, rotation {math.degrees(math.atan(ft[0])):.2f} deg")

thr = (bright + dark) / 2
def sample(u, v):
    p = to_photo(u, v)
    x, y = p
    x0, y0 = int(math.floor(x)), int(math.floor(y))
    if x0 < 0 or y0 < 0 or x0 + 1 >= W or y0 + 1 >= H: return 0
    fx, fy = x - x0, y - y0
    return ((1-fx)*(1-fy)*g[y0, x0] + fx*(1-fy)*g[y0, x0+1] + (1-fx)*fy*g[y0+1, x0] + fx*fy*g[y0+1, x0+1])

if a.arcs:
    # tests/pbc/cornerarcs: the radius r shown is in 7 bits in the middle; the arc of each corner
    # is the corner curve of radius r. How bright is its middle (30..60 degrees) compared with
    # its ends (5..15 and 75..85 degrees, which the rounding never hides)?
    bits = 0
    for bit in range(7 if not a.r else 0):
        v = np.mean([sample(64 + bit * 16 + 8 + du, 140 + dv) for du in (-3, 0, 3) for dv in (-3, 0, 3)])
        bits = bits << 1 | (1 if v > thr else 0)   # black squares are 0
    if a.r:
        bits = a.r
    out = []
    for name, (sx, sy, ox, oy) in {"top left": (1, 1, 0, 0), "top right": (-1, 1, 240, 0),
                                   "bottom left": (1, -1, 0, 280), "bottom right": (-1, -1, 240, 280)}.items():
        def arc(lo, hi):
            vals = []
            for k in np.arange(lo, hi, 1.0):
                t = math.radians(k)
                lx, ly = bits - bits * math.cos(t), bits - bits * math.sin(t)
                vals.append(sample(ox + sx * lx, oy + sy * ly))
            return float(np.mean(vals))
        mid, ends = arc(30, 61), (arc(5, 16) + arc(75, 86)) / 2
        out.append(f"{name} {mid / max(ends, 1):4.2f}")
    print(f"r={bits:3d}  middle/ends brightness: " + ", ".join(out))
    sys.stdout.flush()
    sys.exit(0)

results = {}
# For each corner walk along rays from a point inside (95, 95 from the corner) towards the
# corner at many angles, find where the grey ends, in panel pixels; fit the radius R of the
# circle tangent to both edges: centre (R, R) from the corner, boundary at distance R.
for name, (sx, sy, ox, oy) in {"top left": (1, 1, 0, 0), "top right": (-1, 1, 240, 0),
                               "bottom left": (1, -1, 0, 280), "bottom right": (-1, -1, 240, 280)}.items():
    pts = []
    for k in range(5, 86):
        ang = math.radians(k)
        # ray from (95,95) inside the corner towards the outside, in corner-local coordinates
        cxl, cyl = 95.0, 95.0
        dx, dy = -math.cos(ang), -math.sin(ang)
        # the last bright point before the ray leaves the corner: dark marks inside the grey
        # (the test pattern's diagonals) do not end the visible area
        last = None
        for step in np.arange(0, 140, 0.1):
            lx, ly = cxl + dx * step, cyl + dy * step
            if lx < -4 or ly < -4: break
            u, v = ox + sx * lx, oy + sy * ly
            if sample(u, v) >= thr:
                last = (lx, ly)
        if last is not None:
            pts.append(last)
    pts = np.array(pts)
    # keep the points that belong to the rounding (both coordinates > 0.5 px inside)
    best = None
    for R in np.arange(2, 90, 0.25):
        # distance of each point to the circle; points on the straight parts (beyond R) count
        # against the straight edges instead
        errs = []
        for lx, ly in pts:
            if lx < R and ly < R:
                errs.append(abs(math.hypot(R - lx, R - ly) - R))
            else:
                errs.append(min(abs(lx), abs(ly)))
        e = float(np.mean(np.square(errs)))
        if best is None or e < best[0]:
            best = (e, R)
    results[name] = best[1]
    print(f"{name:13s}: radius {best[1]:5.2f} panel pixels (rms {math.sqrt(best[0]):.2f}), "
          f"diagonal cut {best[1]*(1-1/math.sqrt(2)):.1f} px")

if a.debug:
    dbg = im.copy()
    from PIL import ImageDraw
    d = ImageDraw.Draw(dbg)
    for P in (TL, TR, BR, BL):
        d.ellipse([P[0]-4, P[1]-4, P[0]+4, P[1]+4], outline=(255, 0, 0), width=2)
    for name, (sx, sy, ox, oy) in {"top left": (1, 1, 0, 0), "top right": (-1, 1, 240, 0),
                                   "bottom left": (1, -1, 0, 280), "bottom right": (-1, -1, 240, 280)}.items():
        R = results[name]
        pts = []
        for k in range(0, 91, 3):
            ang = math.radians(k)
            lx, ly = R - R * math.cos(ang), R - R * math.sin(ang)
            pts.append(tuple(to_photo(ox + sx * lx, oy + sy * ly)))
        d.line(pts, fill=(0, 255, 0), width=2)
    dbg.save(a.debug)
print("mean radius: %.2f panel pixels" % np.mean(list(results.values())))
