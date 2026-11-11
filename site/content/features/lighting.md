---
title: "Lighting"
weight: 10
summary: "4096 path-traced lights (upstream 64); emissive surfaces count as lights; light visibility memory, radiance cache, light reuse."
---

- **4096 lights** (upstream 64): camera-following light grid with resampled importance sampling.
- **Emissive surfaces as lights** (`rendering/pathtracing/emissive_lights`).
- **Light visibility memory** (`rendering/pathtracing/light_visibility_memory`): hidden lights stop taking samples.
- **Radiance cache** (`rendering/pathtracing/radiance_cache`): bounces end on cached light.
- **Transmission bounces** (`rendering/pathtracing/transmission_bounces`): glass, ice and water get their own bounce budget; matches 6 bounces in about 70% of the time, so icicles under an eave no longer go dark at the default 3.
- **Light reuse** (`rendering/pathtracing/light_reuse`, off by default): ReSTIR DI reuses light samples across pixels and frames.
- **Indirect light reuse** (`rendering/pathtracing/indirect_reuse`, off by default): ReSTIR GI; rooms and shade lit by bounced light are far less noisy at the same samples.
- Shadow-only casters bounce light.
- Lit raster transparents and rain (`rendering/pathtracing/max_transparency_layers` 0), still traced in reflections.

## Images

![A window-lit room at 4 samples per pixel, indirect light reuse off and on](media/indirect-reuse.jpg)

*Indirect light reuse: the same 4 samples per pixel, off then on; both brightened 3x to show the noise.*
