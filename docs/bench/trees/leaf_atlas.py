#!/usr/bin/env python3
"""Unwrap scanned leaves into the blade parameterisation of TreeGenerator's leaves (load_scan_set, leaf_atlas).

A generated blade's UV runs u 0-1 margin to margin across every row and v 0-1 from the base to the tip, with the
midrib on u 0.5 (as _leaf_texture draws its venation). Each scanned leaf of an atlas (a photo of whole leaves on black
with an opacity mask, e.g. ambientCG LeafSet) is resampled row by row into that frame: row v of the output is the
scan's row at the same share of the way from the lamina's base to its tip, stretched so its margins land on u 0 and 1.
The leaf's outline then comes from the generator's geometry and the scan supplies the lamina between: veins, mottling,
small spots. Palmate leaves (mode lobe) take the central lobe only, a lens round its midvein at most `lobe_width` of
its length wide each side, since the generator builds each lobe as its own blade.

Detail only, as the bark and ground scans: the output is the leaf's linear luminance over its local mean, times 0.85 (the
procedural venation factor's mean, which the leaf material's albedo lifts back to the species' colour), clamped, in
sRGB grey. Columns side by side, one per leaf, each inset by GUTTER of its width so mips do not bleed across.

The generator multiplies this over its own venation (TreeGenerator._leaf_albedo): a scan photographed under soft light
shows its veins hardly at all in colour (a hazel midrib is about 3% brighter than the lamina beside it), so the veins
stay the procedural ones, laid on the geometry's midrib, and the scan gives each leaf its own lamina.

Usage: leaf_atlas.py COLOR.jpg OPACITY.jpg OUT.png [--mode=simple|lobe] [--leaves=0,2,5] [--size=384x768]
Needs numpy, scipy, Pillow.
"""
import sys

import numpy as np
from PIL import Image
from scipy import ndimage

GUTTER = 0.03


def srgb_to_linear(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(c):
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(np.maximum(c, 0.0), 1.0 / 2.4) - 0.055)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    opts = dict(a[2:].split("=", 1) for a in sys.argv[1:] if a.startswith("--") and "=" in a)
    mode = opts.get("mode", "simple")
    lobe_width = float(opts.get("lobe_width", "0.24"))
    w, h = (int(x) for x in opts.get("size", "384x768").split("x"))
    col = np.asarray(Image.open(args[0]).convert("RGB"), dtype=np.float64) / 255.0
    opa = np.asarray(Image.open(args[1]).convert("L"), dtype=np.float64) / 255.0
    lin = srgb_to_linear(col)
    lum = lin @ np.array([0.2126, 0.7152, 0.0722])
    mask = opa > 0.5
    labels, n = ndimage.label(mask)
    sizes = ndimage.sum(mask, labels, range(1, n + 1))
    big = [i + 1 for i in np.argsort(-sizes) if sizes[i] > 0.1 * sizes.max()]
    # Reading order: rows of leaves top to bottom, left to right.
    cents = {i: ndimage.center_of_mass(mask, labels, i) for i in big}
    big.sort(key=lambda i: (round(cents[i][0] / (mask.shape[0] / 4)), cents[i][1]))
    if "leaves" in opts:
        big = [big[int(k)] for k in opts["leaves"].split(",")]
    cols = []
    for lab in big:
        m = labels == lab
        # Erode four texels: the scans' margins carry a dark fringe from the backdrop.
        m = ndimage.binary_erosion(m, iterations=4)
        ys, xs = np.nonzero(m)
        width = np.array([m[y].sum() for y in range(m.shape[0])])
        lam = np.nonzero(width > 0.12 * width.max())[0]
        top, base = ys.min(), lam.max()  # The tip at the top, the petiole at the bottom.
        length = base - top
        # The midline from the lamina's base to the tip.
        bx = xs[ys == base].mean()
        tx = xs[ys <= top + 2].mean()
        # The local mean (normalised convolution inside the leaf, sigma a twentieth of its length): the scans are lit
        # brighter in their middle, and the generator shades the blade itself. What stays is finer: veins, mottling.
        sig = max(2.0, length / 20.0)
        num = ndimage.gaussian_filter(np.where(m, lum, 0.0), sig)
        den = ndimage.gaussian_filter(m.astype(np.float64), sig)
        local = num / np.maximum(den, 1e-6)
        out = np.zeros((h, w))
        for j in range(h):
            v = (j + 0.5) / h  # 0 at the base.
            y = int(round(base - v * length))
            row = np.nonzero(m[y])[0]
            if row.size == 0:
                out[j] = 0.85
                continue
            cx = bx + (tx - bx) * v
            if mode == "lobe":
                # A smooth lens round the midvein, the central lobe's width: following the lamina's own runs stepped at
                # every sinus. Texels past the margin read as the mean.
                half = lobe_width * length * np.sin(np.pi * min(max(v, 0.04), 0.96) ** 0.8) ** 0.8
                x0, x1 = cx - half, cx + half
            else:
                x0, x1 = row.min(), row.max()
                cx = min(max(cx, x0 + 2), x1 - 2)
            us = (np.arange(w) + 0.5) / w
            # The column's gutter repeats its edge.
            uu = np.clip((us - GUTTER) / (1.0 - 2.0 * GUTTER), 0.0, 1.0)
            # Each half from the midline (base to tip) to its own margin, as the blade's two halves.
            xs_ = np.where(uu < 0.5, x0 + (cx - x0) * uu * 2.0, cx + (x1 - cx) * (uu - 0.5) * 2.0)
            # Past the lamina's run round the midline the row is mirrored back into it (the blade's outline is the
            # geometry's; a mirrored texel keeps the scan's texture where the mean would leave a flat band).
            runs = np.split(row, np.nonzero(np.diff(row) > 1)[0] + 1)
            run = min(runs, key=lambda r: 0 if r[0] <= cx <= r[-1] else min(abs(r[0] - cx), abs(r[-1] - cx)))
            a0, a1 = float(run[0]), float(run[-1])
            if a1 - a0 >= 2.0:
                span = a1 - a0
                t = np.mod(xs_ - a0, 2.0 * span)
                xs_ = a0 + np.where(t > span, 2.0 * span - t, t)
            xi = np.clip(xs_.astype(int), 0, m.shape[1] - 2)
            fx = xs_ - xi
            r = (lum[y, xi] * (1 - fx) + lum[y, xi + 1] * fx) / np.maximum(local[y, xi], 1e-6)
            inside = m[y, xi] & m[y, xi + 1]
            ratio = np.where(inside, r, 1.0)
            out[j] = 0.85 * np.clip(ratio, 0.45, 1.6)
        cols.append(out)
    atlas = np.concatenate(cols, axis=1)
    g = np.round(np.clip(linear_to_srgb(atlas), 0, 1) * 255).astype(np.uint8)
    Image.fromarray(g, "L").save(args[2])
    ratio = atlas / 0.85
    print(f"{args[2]}: {len(cols)} leaves, {atlas.shape[1]}x{atlas.shape[0]}, p98/mean {np.percentile(ratio, 98) / ratio.mean():.2f}")


if __name__ == "__main__":
    main()
