# Lamps inside fog, smoke and murky water (equiangular sampling)

Status: PARKED on branch claude/equiangular, not merged. The gain measured on lavapipe is too small
for its cost, and on and off disagree on the mean by 2 to 4%; see Results.

## Where we start

A path inside a scattering medium (murky water, the RT_MAT_FLAG_VOLUME surfaces) draws its
next collision by free flight: the distance is sampled in proportion to the medium's
transmittance, `p(t) = sigma e^(-sigma t)`, on one colour channel picked at random and weighed
over all three (spectral MIS). At the collision it takes a light sample (`rt_volume_direct()`).

That is the right distance for a medium lit evenly, like water under the sun. It is the wrong
one for a lamp inside the medium. A point light's in-scattering along a ray falls off as
`1/r^2` around the point of the ray nearest the lamp, so almost all of a ray's glow comes from
a short stretch next to it. Free flight puts its collisions where the transmittance is high,
mostly near the camera, not where the lamp is: a ray that passes 20 cm from a lamp in 10 m of
water seldom collides within that stretch, and when it does, `1/r^2` makes it a bright speck.
The halo around an underwater lamp is the noisiest thing in such a shot, and it is still
noisy after many frames.

## The technique

**Equiangular sampling** (Kulla and Fajardo, *Importance Sampling Techniques for Path Tracing
in Participating Media*, EGSR 2012). For a light at distance `d` from the ray, with its foot at
`t0`, sample the angle seen from the light uniformly instead of the distance:

    t = t0 + d tan(mix(theta_a, theta_b, u)),   theta = atan(t - t0, d),
    p(t) = d / ((theta_b - theta_a) (d^2 + (t - t0)^2)).

The density is exactly the `1/r^2` of the light, so a point light's single scattering along the
ray is sampled with almost no variance (only transmittance, the phase function and the shadow
are left). The paper shows it beating distance sampling by an order of magnitude for lights in
thin and thick media, and combining it with distance sampling by MIS for the media where the
transmittance matters. It came from Arnold and is in Cycles ("Equiangular"
volume sampling, with MIS against distance sampling).

Compared:

- **Distance sampling only** (what we had): good for even light, bad for lamps.
- **Equiangular only:** bad for dense media, where the transmittance falls off faster than
  `1/r^2`; it ignores the medium entirely.
- **Both, MIS** (Kulla and Fajardo section 4; Cycles' default): each strategy weighed by the
  balance heuristic over the distance `t`. This is what we build.
- **Joint distance and light sampling** (Georgiev et al., *Joint Importance Sampling of
  Low-Order Volumetric Scattering*, 2013) and **product sampling** (Villeneuve et al. 2021): a
  further gain for double scattering and for very dense media, much more code. Not now.
- **Volumetric ReSTIR** (Lin, Wyman and Yuksel, *Fast Volume Rendering with Spatiotemporal
  Reservoir Resampling*, 2021): reuse across pixels, for heterogeneous gas. Our gas has its own
  path; this is for later if lamps inside gas become common.

## What we build

In `scene_raytracing_raygen.glsl`, for each segment of a path inside a homogeneous scattering
medium, besides the free flight (all in `rt_medium_equiangular()`):

1. **Pick a lamp.** A point `t_u` is drawn uniformly on the segment and the light grid is read
   there. Among the positional lights whose range sphere holds that point, one is chosen by
   RIS with target `power x (theta_b - theta_a) / d` (the integral of `1/r^2` over the part of
   the segment it reaches). A light's chance of being among the candidates is the share of the
   segment inside its range (`reach`); the estimate is divided by it, so it stays unbiased with
   many lamps and only the grid's local lists are read.
2. **Pick the distance** equiangularly on the chord of that light's range sphere.
3. **Shade** it: `sigma_s T(t) L_e(t) phase(t) visibility`, with a shadow ray as usual.
4. **MIS.** The weight is `p_e / (p_e + p_d)`, `p_d` the free flight's collision density
   (mean over the three channels, as it is drawn). The collision's own light sample, when it
   picks the same light, is weighed by `p_d / (p_e + p_d)`. Each pair of weights sums to one,
   so nothing is counted twice and nothing is lost.

Directional lights (the sun into water) stay with the collision's sample: they are even along
the ray. Heterogeneous gas stays as it is too: equiangular sampling there needs the
transmittance at an arbitrary point, which the gas path does not have analytically.

Haze around street lamps on land is Godot's froxel fog, not path sampled, so it is not
changed by this. Lamps in our own media (water, and any RT volume with scattering) are.

## Cost

One more light-grid read, up to `PLIGHT_CANDIDATES` light reads and one shadow ray per
scattering segment, only inside media with scattering and only when there is a positional
light. No memory. Off switch: `rendering/pathtracing/equiangular_media`.

## Results (lavapipe, 10 Nov)

`docs/bench/materials`, `--scene=water_lamps` (two shadowed omni lights, range 8 m, inside the
murky pool at night), `_silty` (light fades over 5 m) and `_above` (the lamps 1 to 3 m over the
silty water). 480x270, PSNR against 256 spp (on and off averaged):

| Scene | 4 spp off | 4 spp on | 1 spp off | 1 spp on |
|---|---|---|---|---|
| murky | 27.13 dB | 27.28 dB | 21.40 dB | 21.94 dB |
| silty | 27.34 dB | 29.12 dB | | |

- Time: 256 spp took 119 s off and 160 s on (silty), about 1.35x. At equal time that leaves
  about +0.5 dB in silty water and a loss in murky water. Lavapipe is not a GPU; the shadow ray
  and the light reads may cost less there, but not 3x less.
- Why so little: in murky water most of the glow is multiple scattering (albedo near 0.6 to
  0.7, a metre or two to the walls), and the noise is the lit floor and walls seen through it.
  Equiangular sampling fixes single scattering only. Limiting it to segments before the first
  collision kept the gain and removed nothing else.
- Mean: with linear tone mapping (`--linear`, no clipping) at 512 spp, the pool reads 3.8%
  brighter with it on (silty), 1.7% with the lamps 1 m and more over the water, and the same
  with the equiangular half alone (no MIS, the collision's light sample of those lights off).
  The ground outside the water matches to 0.002%. So the two estimators of the lamps' single
  scattering disagree. With the lamps inside the water, the free flight's estimate has
  unbounded variance near a lamp (1/r^2 with nothing to cancel it), so its mean is reached
  slowly and from below, which explains part of it; with the lamps out of the water it should
  not, so a remaining mismatch between the two estimators is not ruled out.

To pick up again: find the 1.7% (a CPU reference of one lamp over a slab of medium settles which
estimator is right), then measure GPU time on the PC. Worth it for lamps in fog and thin media
(Kulla and Fajardo's cases), less for our murky water.

## References

- C. Kulla, M. Fajardo, *Importance Sampling Techniques for Path Tracing in Participating
  Media*, EGSR 2012.
- I. Georgiev, J. Krivanek, T. Hachisuka, D. Nowrouzezahrai, W. Jarosz, *Joint Importance
  Sampling of Low-Order Volumetric Scattering*, SIGGRAPH Asia 2013.
- B. Miller, I. Georgiev, W. Jarosz, *A Null-Scattering Path Integral Formulation of Light
  Transport*, SIGGRAPH 2019 (the spectral MIS of the free flight).
- Blender Cycles, volume sampling method "Equiangular" / "Multiple Importance".
