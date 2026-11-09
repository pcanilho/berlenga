# Subsurface scattering in the path tracer

## What the engine does today

- **Raster.** BaseMaterial3D's Subsurface Scattering section (`subsurf_scatter_strength`, its
  texture, Skin Mode) and custom shaders' `SSS_STRENGTH` write a strength into the diffuse buffer's
  alpha. A screen-space pass then blurs the diffuse lighting of those pixels: a separable kernel
  (Jimenez et al., "Separable Subsurface Scattering", 2015), Gaussian in all channels, or a fixed
  three-colour skin kernel in Skin Mode. The blur width is `subsurface_scattering_scale` (0.05) of
  the screen times the strength, mixed toward a depth-scaled width by
  `subsurface_scattering_depth_scale` (0.01). So the radius is mostly a screen fraction, not a
  distance in the world. Light through thin parts (ears, fingers) is a separate Transmittance term
  read from the shadow map depth.
- **Path tracer.** Nothing. The strength is parsed and dropped; skin, wax, marble and leaves trace as
  plain Lambertian diffuse, so a lit face has hard terminator lines and no red bleed into shadow.

## How others do it

- **Cycles (Blender), Random Walk.** The production reference. A subsurface bounce enters the mesh
  through a Lambertian (cosine) lobe about the inverted normal, then walks a homogeneous medium:
  free flights drawn on one colour channel, chosen in proportion to throughput times albedo and
  combined over all three by the balance heuristic (spectral MIS), isotropic or Henyey-Greenstein
  scattering, until a ray inside the object hits its own surface or 256 steps pass. The exit point
  becomes a white Lambertian surface: light sampling and the next bounce happen there. The medium
  comes from the surface colour and radius with an albedo inversion so that the walk's result has
  that colour: since 2023 the van de Hulst form of d'Eon ("A Hitchhiker's Guide to Multiple
  Scattering", eq. 53.7), `s = 4.09712 + 4.20863 A - sqrt(9.59217 + 41.6808 A + 17.7126 A^2)`,
  single-scatter albedo `(1 - s^2) / (1 - g s^2)`, extinction `1 / radius`; before that Chiang, Kutz
  and Burley's fit ("Practical and Controllable Subsurface Scattering for Production Path Tracing",
  SIGGRAPH 2016 talk). Single-scatter albedos under 0.2 are raised to 0.2 and the weight scaled
  down to match. Variance is cut by Dwivedi guided sampling (Kriv&aacute;nek and d'Eon, "A
  Zero-variance-based Sampling Scheme for Monte Carlo Subsurface Scattering", SIGGRAPH 2014;
  Meng et al. 2016), mixed with the classic sampling by MIS. Source read:
  `src/kernel/integrator/subsurface_random_walk.h` and `subsurface.h`.
- **RenderMan PxrSurface / Pixar.** Path-traced SSS by random walk too, with anisotropic phase
  functions and non-exponential free flights (Wrenninge, Villemin and Hery, "Path Traced
  Subsurface Scattering using Anisotropic Phase Functions and Non-Exponential Free Flights", 2017).
- **Unreal Engine 5 Path Tracer.** Subsurface and Subsurface Profile materials are traced as a random
  walk with the profile's mean free path as the radius (Chiang 2016 mapping); the rasterizer keeps
  Burley's normalized diffusion as a screen-space filter (Christensen and Burley, "Approximate
  Reflectance Profiles for Efficient Subsurface Scattering", 2015).
- **PBRT-v4.** A tabulated BSSRDF (photon beam diffusion, Habel et al. 2013) sampled with probe rays
  along three axes. It assumes a locally flat, semi-infinite medium, so it fails on thin parts.
- **Falcor and RTXPT.** No volumetric SSS; StandardBSDF only has diffuse transmission for thin
  surfaces. **RTX Remix** exposes subsurface transmittance colour, measurement distance and
  single-scattering albedo, a thin single-scattering approximation for game assets.
- **Measured skin.** Jensen et al., "A Practical Model for Subsurface Light Transport" (2001), skin1:
  diffuse mean free paths 3.67, 1.37 and 0.68 mm for red, green and blue. Christensen and Burley
  2015 use the same numbers.

## Choice

Cycles' random walk. It is the only one of these that needs no flat-surface assumption, so ears,
noses, fingers, candles and leaves with real thickness pass light through without a separate
Transmittance term, and it reuses what the path tracer already has: medium free flights with
spectral MIS (water, #163), light sampling at a point (`lights_evaluate_direct_lighting`), and
payload codes for media. Diffusion profiles (Burley, photon beam diffusion) would need probe rays
plus a flat-surface fit, which breaks on thin geometry, the case raster handles worst.

## Design

1. **Material inputs.** BaseMaterial3D's strength (times the texture's red channel) and Skin Mode;
   custom shaders' `SSS_STRENGTH` and `render_mode sss_mode_skin`. The colour is the surface albedo,
   as in Cycles, so texture detail at the entry point stays sharp: only the light under it spreads.
2. **Radius.** Godot's raster radius is a screen fraction, so the path tracer needs a world unit:
   `rendering/pathtracing/subsurface_scattering_scale`, the radius in metres at strength 1 (default
   0.01). Skin Mode scales the channels by measured skin (Jensen 2001: 1, 0.373, 0.185); otherwise
   all channels share the radius, as raster's Gaussian kernel.
3. **Entry.** With the feature on and the path able to continue, the base's diffuse lobe becomes the
   subsurface lobe: its direct light at the entry point is dropped (it arrives through the walk) and
   a diffuse bounce enters along a cosine direction about -N. The weight is the diffuse lobe's
   without the albedo, which the walk adds. Peel layers and the last bounce stay plain diffuse.
4. **Walk (raygen).** From the entry, at most 256 steps: pick a channel by throughput times
   single-scatter albedo, draw a free flight, trace a probe ray with no face culling to the nearest
   surface. A back face nearer than the flight is the exit; otherwise scatter isotropically there.
   Throughput is updated with the balance-heuristic over channels (Cycles' estimator). Russian
   roulette ends faint walks without bias.
5. **Closed meshes only.** If the first probe does not leave through a back face (an open card, a
   single-sided leaf, or another object behind), the surface is treated as thin and shades as plain
   diffuse at the entry (raster's look); Back Lighting stays the way to pass light through cards.
   Any later probe that escapes or meets another object's front face ends the walk.
6. **Exit.** A white Lambertian surface at the exit with the exit's interpolated normal: one light
   sample with the same light selection as surfaces, then a cosine bounce on. Emissive surfaces are
   not light-sampled at the exit, so emission a later ray finds counts in full.
7. **Probe rays.** A payload flag makes the closest-hit shader return distance, facing, normal and
   entry without evaluating the material, and the miss shader return at once.
8. **Ray Reconstruction.** The diffuse albedo guide is the surface albedo, as for diffuse: the
   walk's light divided by it is smooth, which the denoiser keeps, and pores and freckles stay in
   the albedo.
9. **Toggle.** `rendering/pathtracing/subsurface_scattering`; off traces subsurface materials as
   plain diffuse, as before.

## Later

- Dwivedi guided sampling (Cycles' MIS mix of classic and guided) to shorten walks in high-albedo
  media such as marble and milk.
- Anisotropy (Cycles' `subsurface_anisotropy`, skin about 0.8) once a material input exists.
- Raster's Transmittance colour and depth for open cards, as diffuse transmission.
- A refractive, rough entry (Cycles' Random Walk Skin) for wet skin and gems.
