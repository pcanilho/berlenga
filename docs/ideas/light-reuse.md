# Light reuse across pixels and frames (ReSTIR DI)

Status: built, off by default (`rendering/pathtracing/light_reuse`); GPU cost to measure.

## Where we start

Positional lights are picked per shading point by resampled importance sampling
(`lights_select_positional`, raytracing_lights_inc.glsl): up to 16 candidates from the light
grid's lists, half of them from the region's visible-light list (the light visibility memory,
857c72c019), each weighted by its unshadowed contribution. One shadow ray then tests the light
that won. Every pixel starts from nothing every frame, so a street with many lamps in range
still needs many samples before each pixel finds the lamps that light it.

## The technique

**ReSTIR DI**, spatiotemporal reservoir resampling (Bitterli, Wyman, Pharr, Shirley, Lefohn
and Jarosz, SIGGRAPH 2020). Each pixel keeps the light it chose as a *reservoir*: the light,
its contribution weight W, and a confidence M (how many samples it stands for). The next frame
the pixel combines its fresh candidates with its own reservoir from last frame (temporal reuse)
and with a few neighbours' (spatial reuse), each weighted by its target at this pixel times W
times M. A good light found by one pixel spreads to the pixels around it and stays found.
The paper reports 6x to 60x lower error than RIS alone at equal time with thousands of lights.

The production form is NVIDIA's RTXDI (Wyman and Panteleev, "Rearchitecting Spatiotemporal
Resampling for Production", HPG 2021), used in Cyberpunk 2077's path tracing and Portal RTX.
Its tested choices, which we copy:
- M is capped at 20 frames of history, so lights that change are relearnt in under a second.
- Each initial reservoir counts as M = 1 however many candidates built it.
- Neighbours are taken only where the surfaces agree: normals within a threshold and depth
  within 10%. Lights are tracked through a remap table when the light list's order changes.
- The "biased" mode: neighbours' MIS weights are 1/M over the reservoirs that pass
  validation, with each sample's target re-evaluated at the current surface only. Bias is a
  little darkening where neighbours could not have chosen the light. The unbiased mode needs a
  shadow ray or a target evaluation per neighbour pair.

**Generalized RIS** (Lin, Kettunen, Bitterli, Pantaleoni, Yuksel and Wyman, SIGGRAPH 2022)
gives the general weights. We use its balance heuristic between the current surface and the
stored ones: each stored reservoir keeps its surface's position and normal, so the target of a
candidate light can be evaluated at each stored surface too. For temporal and two spatial
reservoirs that is 3 x 3 cheap target evaluations (no rays), and it removes the 1/M darkening.

**What other engines do.**
- Unreal 5.5+ MegaLights does not use ReSTIR. It traces a few rays per pixel to lights picked
  by importance, keeps a visible-light hash per screen tile, and denoises. Epic avoided
  reservoir reuse for its lag on moving lights and its blotchy correlated noise under its own
  denoiser. Our visibility memory is the MegaLights half; this adds the reuse half.
- Unity HDRP and Frostbite ship no ReSTIR in their path tracers. Embark's kajiya uses ReSTIR GI.
- RTXDI and DLSS Ray Reconstruction were tuned together: RR expects ReSTIR's correlated noise.

## Design here

One pass, inside the existing trace: no extra dispatch.
- Only at the primary hit of the main path (bounce 0), positional lights only. The
  directional group and bounces keep today's selection.
- A new buffer (binding 51): a remap table (last frame's light index to this frame's), then
  two frames of reservoirs, one per internal-resolution pixel, 24 bytes each: light + 1 and M,
  W, the surface's position (relative to the anchor), its normal. 48 bytes a pixel in total:
  100 MB at 1080p internal, 177 MB at 1440p.
- Sample 0 of a pixel reads last frame's reservoirs: its own (reprojected with the surface's
  motion, as the motion vectors) and two neighbours within 16 pixels, each validated by
  position and normal. Later samples of the pixel keep today's independent choice: tried
  first, they all resampled the same stored light, and 4 spp lost 0.2 dB to that correlation.
- The combined sample is shaded with the one shadow ray we trace today. Visibility is not
  folded into the stored weight (RTXDI's "visibility reuse"): that saves noise in shadow but
  darkens soft shadow edges, which breaks the no-degradation rule. The visibility memory
  already steers candidates away from hidden lights.
- Where the directional group is picked, the pixel carries its reprojected reservoir forward
  unchanged, so history survives frames that sampled the sun.
- Off (project setting) matches today exactly.

## Acceptance

The engine floor: ship only if image quality improves at a small cost. Measured on the
showcase night street and the long street of 300 lamps, 480x270, 4 spp single frame against
1024 spp, after 16 frames of history: PSNR, mean luminance (no darkening), and trace time on
the CPU rasteriser (lavapipe). GPU cost goes on the PC queue.

## Results (lavapipe, 480x270, 16 frames of history, PSNR against 1024 spp)

| Scene | spp | Off | On |
|---|---|---|---|
| Plaza, 400 festoon lights reaching 30 m, direct only | 1 | 12.99 dB | 14.14 dB |
| Plaza, full paths | 1 | 13.14 dB | 14.31 dB |
| Plaza, direct only | 4 | 16.93 dB | 17.10 dB |
| Plaza, full paths | 4 | 17.14 dB | 17.31 dB |
| Long street, 300 lamps reaching 10 m | 4 | 24.93 dB | 24.93 dB |

At 1 spp, temporal reuse alone gave +0.9 dB and spatial alone +0.65 dB. The gain is far
below the paper's because selection already does RIS over 16 candidates with the visibility
memory: most of what is left is which of many similar lights is shadowed, which only
visibility reuse (biased, darker soft shadows) or more shadow rays would remove. Where at most
16 lights reach a point, every light is already enumerated and nothing changes. Hence off by
default until the GPU numbers are in: it should help a 1 spp image under Ray Reconstruction
in scenes dense with lights, at 48 bytes a pixel and about 16 target evaluations per pixel.

## Sources

- Bitterli et al., "Spatiotemporal reservoir resampling for real-time ray tracing with dynamic
  direct lighting", ACM TOG 39(4), SIGGRAPH 2020.
- Wyman and Panteleev, "Rearchitecting Spatiotemporal Resampling for Production", HPG 2021.
- Lin et al., "Generalized Resampled Importance Sampling: Foundations of ReSTIR", ACM TOG 41(4),
  SIGGRAPH 2022.
- NVIDIA RTXDI SDK documentation (github.com/NVIDIA-RTX/RTXDI), ReSTIR DI parameters.
- Talbot, Cline and Egbert, "Importance Resampling for Global Illumination", EGSR 2005.
- Epic Games, "MegaLights: Stochastic Direct Lighting in Unreal Engine 5", SIGGRAPH 2025
  Advances in Real-Time Rendering.
