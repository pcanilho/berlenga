# Indirect light reuse across pixels and frames (ReSTIR GI)

Status: built, off by default (`rendering/pathtracing/indirect_reuse`); GPU cost to measure.

## Where we start

Each pixel's path picks its first bounce direction from the surface's BSDF alone: cosine
weighted for the diffuse lobe. Where the indirect light comes from a small bright patch (a
sunlit floor seen from a dark room, a lit wall at the end of a corridor), few directions find
it, and a pixel at 1 spp either finds it (bright speck) or not (black). The radiance cache
(SHaRC style, raytracing_radiance_cache_inc.glsl) already makes what each direction finds
cheap and smooth: a bounce that reaches a rough surface reads the light that cell sends back
and the path ends. It does nothing for which direction is taken. That is the remaining noise
of indoor and shaded light, and it is what ReSTIR GI removes.

## The technique

**ReSTIR GI** (Ouyang, Liu, Kettunen, Pharr and Pantaleoni, HPG 2021). The first bounce of
each pixel's path is kept as a sample: the point it reached (x2), that point's normal, and the
radiance that came back from it (everything the rest of the path gathered, divided by the
throughput at the first bounce). A pixel resamples its new sample together with last frame's
reservoir for the same surface and with neighbours' reservoirs, by reconnecting its own
surface (x1) to their x2. Since x2 is a point, not a direction, a neighbour's sample is valid
here after a change of variables: the Jacobian of the reconnection shift,

    J = (cos θ2' / cos θ2) · (|x2 − x1|² / |x2 − x1'|²),

where θ2 is the angle at x2 to the surface that found it and θ2' to the one reusing it. The
paper reports 9.3x to 166x lower MSE than plain path tracing at equal time, the most indoors.
It treats the radiance from x2 as the same in every direction, so x2 should be rough.

**Generalized RIS** (Lin, Kettunen, Bitterli, Pantaleoni, Yuksel and Wyman, SIGGRAPH 2022)
gives the weights for reservoirs from other domains: each reservoir's sample is weighted by
the balance heuristic over every contributing surface's target for that x2, with the
Jacobian of the shift to each. Every term is a target evaluation (no rays): 4 reservoirs give
16. This is the same scheme as our light reuse (docs/ideas/light-reuse.md).

**ReSTIR PT** (Lin et al., SIGGRAPH 2022, and "ReSTIR PT Enhanced", 2026) generalises this to
whole paths with hybrid shifts (reconnect at the first rough vertex). It handles glossy
first surfaces, at the price of replaying path prefixes with extra rays per reuse. Not for a
first step: our glossy first hits keep their own paths (below), and NVIDIA's RTXDI SDK still
ships ReSTIR GI as its production path.

**Production practice** (RTXDI's ReSTIR GI, used with DLSS Ray Reconstruction in RTX Remix
and Cyberpunk 2077's path tracing; Kajiya, Embark):
- Temporal history capped at 8 to 30 frames; spatial reuse from 1 to 3 neighbours within
  about 30 pixels, taken only where depth and normal agree.
- Secondary surfaces treated as Lambertian; RTXDI recommends clamping their roughness, and
  says ReSTIR GI does not resolve caustics.
- For shiny first surfaces, RTXDI mixes the resampled result with the unresampled path by MIS
  in the final shading, since resampling gives sparse noise there that denoisers handle badly.
- Bias correction: a final visibility ray from x1 to the chosen x2. Without it, light from a
  neighbour's x2 leaks through thin walls and into contact shadows.

**Other engines.** Unreal's Lumen gathers screen probes with its own importance sampling and
a world radiance cache, no reservoirs. Unity HDRP's path tracer and Frostbite ship no ReSTIR.
Kajiya uses ReSTIR GI for diffuse only, over an irradiance cache, the closest design to ours.

## How it fits with the radiance cache

They do different jobs and stack. The radiance cache answers "what light comes back from x2"
at low variance; ReSTIR GI answers "which x2 to look at". With the cache on, the radiance a
sample carries is already smooth, which is the case ReSTIR GI resamples best (the paper's
target is the radiance's luminance, so noisy radiance makes noisy weights). Neither replaces
the other: the cache alone leaves direction noise, ReSTIR GI alone resamples noisy samples.

## Design here

One pass inside the existing trace, as light reuse: no new dispatch.
- Only at a camera ray's first hit, only for the samples that took the diffuse lobe there (the
  coat, specular, transmission, back light and hair lobes keep their own paths, as RTXDI's MIS
  keeps shiny first surfaces). The lobe is chosen at random with its probability, so replacing
  the diffuse lobe's estimate with another unbiased estimate of the same integral keeps the
  image unbiased. Every sample of a pixel hits the same first surface, so all the diffuse
  samples' first bounces are candidates for one reservoir (resampled importance sampling), and
  all of them are shaded with its result.
- The closest hit records the surface for raygen in a per-pixel slot: the throughput before
  the diffuse weight times the diffuse reflectance, its normals, roughness (for Burley's
  factor) and where it was last frame. Raygen keeps the first bounce's x2 and, at the path's
  end, the radiance that came back.
- The integrand matches the path tracer exactly, including its recovery of directions below
  the surface (mirrored across the geometric plane, so a direction's density is the sum of
  the two cosines that reach it). The fresh sample's weight is then the path's own.
- Target: radiance luminance times that cosine. Temporal reuse from the reprojected pixel,
  spatial from two neighbours within 16 pixels of last frame, each validated by normal and
  plane distance. History capped at 20 frames (as our light reuse). Weights: generalized RIS
  balance heuristic with reconnection Jacobians.
- Neighbours' samples are reconnected only when their x2 is rough (roughness at least 0.4, as the radiance cache,
  no transmission or hair); others serve only their own surface's temporal reuse.
- One visibility ray for a chosen sample that is not the pixel's own new one.
- Off by default under environment fog, gas volumes and a camera in water (their scattering
  lies on the first bounce segment, which a reconnection would skip).
- Memory: two frames of 10-uint reservoirs plus a 6-uint surface record: 104 bytes a pixel,
  96 MB at 1280x720 internal, 215 MB at 1920x1080. Allocated only when on.

## Acceptance

Floor rule: ship only if quality improves at small cost. Measured on a sunlit room lit
mostly by indirect light, 480x270 at 1 and 4 spp after 16 frames, PSNR against 1024 spp
with reuse off; mean luminance to show any darkening; and on the showcase city and plaza to
show nothing else changes. GPU cost goes on the PC queue.

## Results (lavapipe, 480x270, 16 frames of history, PSNR against 1024 spp, reuse off)

| Scene | spp | Off | On |
|---|---|---|---|
| Room lit through one window (`--shot=room`) | 1 | 21.98 dB | 28.04 dB |
| Room | 4 | 22.82 dB | 29.96 dB |
| Room | 64 | 29.58 dB | 33.95 dB |
| City street (`--shot=city`) | 4 | 26.99 dB | 27.05 dB |
| Plaza, 400 lights (`--shot=plaza`) | 4 | 17.13 dB | 17.12 dB |

In the room, 4 spp with reuse beats 64 spp without it. At 64 spp the mean brightness is the same
on and off (0.0596 and 0.0595 of full scale), so no darkening from the visibility check shows.
Lower spp images are darker than the reference in both modes because their bright specks clip
at 8 bits. Outdoors, where direct light dominates, nothing changes. Frame time on the CPU
rasteriser is within run-to-run noise (25 s against 27 s for the 16-frame room run). The GPU
cost goes on the PC queue. Turn it on by default only if that cost is small.

Done along the way: all of a pixel's diffuse samples feed one reservoir. With only sample 0
reusing, 4 spp gained just 1.3 dB, because the other three samples' noise dominated.

## Sources

- Ouyang, Liu, Kettunen, Pharr and Pantaleoni, "ReSTIR GI: Path Resampling for Real-Time Path
  Tracing", Computer Graphics Forum 40(8), HPG 2021.
- Lin, Kettunen, Bitterli, Pantaleoni, Yuksel and Wyman, "Generalized Resampled Importance
  Sampling: Foundations of ReSTIR", ACM TOG 41(4), SIGGRAPH 2022.
- Lin et al., "ReSTIR PT Enhanced", NVIDIA Research, 2026.
- NVIDIA RTXDI SDK, doc/RestirGI.md (github.com/NVIDIAGameWorks/RTXDI).
- Bitterli et al., "Spatiotemporal reservoir resampling for real-time ray tracing with dynamic
  direct lighting", ACM TOG 39(4), SIGGRAPH 2020.
