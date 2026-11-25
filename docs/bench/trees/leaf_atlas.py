#!/usr/bin/env python3
"""Scanned leaves as leaf cards for TreeGenerator (load_scan_set, leaf_atlas; docs/ideas/trees.md 5bn).

Each scanned leaf of an atlas (a photo of whole leaves on black with an opacity mask, e.g. ambientCG LeafSet) is laid
into a card in its true proportions, the way leaf cards are made for real-time trees (SpeedTree and Megascans leaf
atlases): v 0-1 from the lamina's base to its tip along the leaf's own midline (the line from the base of the lamina to
the tip, so a tilted leaf is turned upright), u 0-1 across, the midline on u 0.5, the card `aspect` x the leaf's length
wide each side. The alpha channel is the scan's own opacity: the generator cuts the outline with it (teeth, lobes,
sinuses, a cordate base), so the card's rectangle never shows.

Colour is detail only, as the bark and ground scans: the red, green and blue channels hold the leaf's linear luminance
over its local mean (a Gaussian a twentieth of the leaf's length, inside the leaf), times 0.85, the procedural
venation factor's mean; the generator multiplies it over its drawn venation (TreeGenerator._leaf_albedo), since a
scan photographed under soft light hardly shows its veins in colour. Outside the leaf the factor is the mean.

Columns side by side, one per leaf, each inset by GUTTER of its width so mips do not bleed across. Prints the card's
aspect and the leaves' fill (the share of the card the leaf covers) for scans.cfg: the generator sizes each card so
the cut leaf keeps the area of the blade it replaces.

Usage: leaf_atlas.py COLOR.jpg OPACITY.jpg OUT.png [--leaves=0,2,5] [--size=384x512] [--rotate=90]
(--rotate turns the scan counter-clockwise first, for atlases whose leaves lie on their side: the tip must point up.)
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


def bilinear(img, x, y):
    h, w = img.shape
    x = np.clip(x, 0, w - 1.001)
    y = np.clip(y, 0, h - 1.001)
    x0, y0 = x.astype(int), y.astype(int)
    fx, fy = x - x0, y - y0
    return (img[y0, x0] * (1 - fx) * (1 - fy) + img[y0, x0 + 1] * fx * (1 - fy) + img[y0 + 1, x0] * (1 - fx) * fy +
            img[y0 + 1, x0 + 1] * fx * fy)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    opts = dict(a[2:].split("=", 1) for a in sys.argv[1:] if a.startswith("--") and "=" in a)
    w, h = (int(x) for x in opts.get("size", "384x512").split("x"))
    rot = int(opts.get("rotate", "0"))
    col = np.asarray(Image.open(args[0]).convert("RGB").rotate(rot, expand=True), dtype=np.float64) / 255.0
    opa = np.asarray(Image.open(args[1]).convert("L").rotate(rot, expand=True), dtype=np.float64) / 255.0
    lum = srgb_to_linear(col) @ np.array([0.2126, 0.7152, 0.0722])
    mask = opa > 0.5
    labels, n = ndimage.label(mask)
    sizes = ndimage.sum(mask, labels, range(1, n + 1))
    big = [i + 1 for i in np.argsort(-sizes) if sizes[i] > 0.1 * sizes.max()]
    cents = {i: ndimage.center_of_mass(mask, labels, i) for i in big}
    big.sort(key=lambda i: (round(cents[i][0] / (mask.shape[0] / 4)), cents[i][1]))
    if "leaves" in opts:
        big = [big[int(k)] for k in opts["leaves"].split(",")]
    frames = []
    for lab in big:
        m = labels == lab
        ys, xs = np.nonzero(m)
        width = np.array([m[y].sum() for y in range(m.shape[0])])
        lam = np.nonzero(width > 0.12 * width.max())[0]
        top, base = ys.min(), lam.max()  # The tip at the top, the petiole below the lamina's base.
        b = np.array([xs[ys == base].mean(), float(base)])
        t = np.array([xs[ys <= top + 2].mean(), float(top)])
        along = t - b
        length = np.linalg.norm(along)
        along /= length
        perp = np.array([along[1], -along[0]])  # u 0 to 1 runs from the leaf's left to its right, seen from above.
        rel = np.stack([xs, ys], axis=1) - b
        half = np.abs(rel @ perp).max() / length
        # The local mean inside the leaf (normalised convolution): the scans are lit brighter in their middle.
        sig = max(2.0, length / 20.0)
        num = ndimage.gaussian_filter(np.where(m, lum, 0.0), sig)
        den = ndimage.gaussian_filter(m.astype(np.float64), sig)
        local = num / np.maximum(den, 1e-6)
        frames.append((m, b, along, perp, length, half, local))
    aspect = max(f[5] for f in frames) * 1.03
    cols, alphas = [], []
    us = (np.arange(w) + 0.5) / w
    uu = (us - GUTTER) / (1.0 - 2.0 * GUTTER)
    vs = (np.arange(h) + 0.5) / h
    U, V = np.meshgrid(uu, vs)
    for m, b, along, perp, length, half, local in frames:
        # The leaf's own opacity (not the label: a neighbour's edge never enters the card), clipped to this leaf.
        own = np.where(ndimage.binary_dilation(m, iterations=3), opa, 0.0)
        p = b[None, None, :] + (V[..., None] * along[None, None, :] + ((U - 0.5) * 2.0 * aspect)[..., None] * perp[None, None, :]) * length
        x, y = p[..., 0], p[..., 1]
        a = bilinear(own, x, y)
        a = np.where((U < 0) | (U > 1), 0.0, a)
        ratio = bilinear(lum, x, y) / np.maximum(bilinear(local, x, y), 1e-6)
        inside = bilinear(m.astype(np.float64), x, y) > 0.99
        ratio = np.where(inside, np.clip(ratio, 0.45, 1.6), 1.0)
        cols.append(0.85 * ratio)
        alphas.append(a)
    atlas = np.concatenate(cols, axis=1)
    alpha = np.concatenate(alphas, axis=1)
    g = np.round(np.clip(linear_to_srgb(atlas), 0, 1) * 255).astype(np.uint8)
    a8 = np.round(np.clip(alpha, 0, 1) * 255).astype(np.uint8)
    Image.fromarray(np.dstack([g, g, g, a8]), "RGBA").save(args[2])
    fill = float((alpha > 0.5).mean() / (1.0 - 2.0 * GUTTER))
    ratio = atlas[alpha > 0.5] / 0.85
    print(f"{args[2]}: {len(cols)} leaves, {atlas.shape[1]}x{atlas.shape[0]}, aspect={aspect:.3f} fill={fill:.3f}, "
          f"p98/mean {np.percentile(ratio, 98) / ratio.mean():.2f}")


if __name__ == "__main__":
    main()
