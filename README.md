# Godot, path tracing improved

[![Release](https://github.com/pcanilho/godot/actions/workflows/release.yml/badge.svg)](https://github.com/pcanilho/godot/actions/workflows/release.yml) [![CodeQL](https://github.com/pcanilho/godot/actions/workflows/codeql.yml/badge.svg)](https://github.com/pcanilho/godot/actions/workflows/codeql.yml) [![Latest release](https://img.shields.io/github/v/release/pcanilho/godot?label=latest)](https://github.com/pcanilho/godot/releases/latest)

Godot 4 with real-time path tracing on Vulkan and DLSS Ray Reconstruction, built for large open worlds. Based on [NVIDIA-RTX/godot](https://github.com/NVIDIA-RTX/godot) of 2 November 2026; NVIDIA's code is kept wherever it covers the same ground.

**[Download](https://github.com/pcanilho/godot/releases/latest):** Windows (path tracing with Ray Reconstruction; add NVIDIA's DLSS, Streamline and Reflex DLLs next to the exe), Linux x86_64 and arm64 (path tracing, no Ray Reconstruction), macOS universal (raster only). **Changes:** [CHANGELOG.md](CHANGELOG.md).

![1024 street lamps path traced at 16 samples per pixel: NVIDIA-RTX/godot on the left keeps 64 of them, this fork on the right lights them all](docs/media/many-lights.jpg)

*1024 lights, 16 samples per pixel. Left: NVIDIA-RTX/godot (64 lights per frame). Right: this fork.*

Images in this README are raw path tracer output, almost all rendered on the CPU (only the fork and raster strips of the texture comparison come from a GPU), with no denoiser or Ray Reconstruction. They compare raw sample quality, not finished frames.

## What this fork adds

Every feature that changes the look or the cost has a project setting to turn it off. Research notes and the large-world roadmap (RTX Mega Geometry, partitioned acceleration structures, geometry streaming) are in [docs/ideas](docs/ideas).

### Lighting
- **Many lights:** up to 4096 (upstream: 64 per frame), via a light grid that follows the camera and resampled importance sampling. 460 street lamps plus 2000 more over 10 km trace as well as the street alone.
- **Emissive surfaces are lights** (`rendering/pathtracing/emissive_lights`): signs, screens and windows get their own light sample, with multiple importance sampling. +2.5 dB at equal time on a sign-lit street.
- **Light visibility memory** (`rendering/pathtracing/light_visibility_memory`): lights hidden behind walls or roofs stop taking samples. +5.6 dB at equal time on a street among lit buildings.
- **Radiance cache** (`rendering/pathtracing/radiance_cache`): world-space, camera-following, fixed 16 MiB; bounces end on cached light instead of tracing on. Never used for what the camera sees directly.
- Shadow-only casters bounce light, so hidden ceilings and walls still light a room.
- Raster transparents (`rendering/pathtracing/max_transparency_layers` 0) lit by a traced light cache with shadows, sky and bounce; rain glints against lamps and still traces in reflections and refractions.

### Materials and glass
- Textures filter like raster instead of aliasing: ray-cone mip selection, mipmap bias, anisotropic filtering.
- Clear glass casts an even, tinted shadow instead of a speckled one.
- Back faces pass their own normal to `vertex()`; `IN_SHADOW_PASS` in custom shaders.

### Large worlds
- **Camera-relative tracing** (`rendering/pathtracing/relative_origin_grid`): shadows, reflections and contacts stay exact kilometres from the origin. Shaders still see world positions.
- Ray offsets scale with coordinate size, so floors do not self-shadow far out.
- **Mesh LOD in the path tracer** (`rendering/pathtracing/mesh_lod`, off by default): 120 rocks over 1 km, 346 to 110 MB.
- **Off-screen culling** (`rendering/pathtracing/cull_max_distance`, `cull_min_size`, off by default): small or far off-screen objects leave the acceleration structure; what the camera sees is always traced.

### Exposure
- **Histogram metering** (`rendering/camera/auto_exposure/metering`): bright lamps in view no longer black out a night street.
- **Dark-scene key** (`dark_scene_luminance`, `dark_scene_strength`, off by default): night stays night instead of turning into dusk.
- **Local exposure** (`local_highlight_contrast`, `local_shadow_contrast`, off by default): lit walls keep detail and dark facades lift, without halos.
- No overshoot flash on long frames.

### DLSS Ray Reconstruction
- Runs with the DLSS 4.5 DLLs (nvngx_dlssd 310.5); model chosen with `rendering/streamline/dlss_ray_reconstruction_preset`.
- Raster transparents and particles are laid over its output unjittered at output size, so thin rain stays continuous; additive, subtractive and `blend_mul` composite correctly; emissive detail keeps its edges.
- Warns when used without DLSS upscaling; frame generation presents generated frames.

### Physics (PhysX 5)
Windows x64 and Linux x86_64 builds include PhysX 5 with Blast destruction and Flow smoke and fire. Pick it per project in **Project Settings > Physics > 3D > Physics Engine** (`Jolt Physics` or `PhysX`); every 3D physics node works with either.
- With either engine: `PhysXCloth3D`, `PhysXSkinnedCloth3D`, `PhysXDestructible3D` (editor fracture tool), `PhysXFlow3D`, ocean water with buoyancy, compute-shader fluids and granular material.
- PhysX only: vehicles, CUDA fluids and deformables, GPU rigid bodies (CUDA builds; releases use the CPU solver).
- Builds without the SDKs (`physx_sdk=`, `blast_sdk=`, `flow_sdk=`; see `modules/godot_physx/README.md`) skip the module.

### Performance, API, stability
- Cached shadows for still casters; far lights update less often; optional time-sliced reflection probes.
- `RenderingServer.is_pathtracing_ready()`; `RenderingDevice.get_video_memory_budget()` / `get_video_memory_usage()`.
- Leak, shutdown and threading fixes; VK_EXT_device_fault reports on device loss.

## Comparisons

Same scene and samples per pixel, no denoiser. Scenes: [docs/media/showcase](docs/media/showcase) (`godot --path docs/media/showcase -- --shot=<city|lights|textures|glass|far|night>`) and [docs/media/light-selection](docs/media/light-selection) (`-- --scene=<signs|city|street>`).

<details>
<summary>Many lights: 1024 lamps, NVIDIA-RTX/godot vs this fork</summary>

![1024 street lamps: NVIDIA-RTX/godot keeps 64 of them, this fork lights them all](docs/media/many-lights.jpg)

NVIDIA keeps the 64 highest scoring lights per frame; the rest go dark.
</details>

<details>
<summary>Glass shadows: NVIDIA-RTX/godot vs this fork</summary>

![Glass pane shadow: speckled on NVIDIA-RTX/godot, even and tinted on this fork](docs/media/glass-shadow.jpg)

NVIDIA passes or stops each shadow ray at random; the fork weights it by the pane's coverage and colour.
</details>

<details>
<summary>Texture filtering: NVIDIA-RTX/godot, this fork, raster</summary>

![Checker floor: moire on NVIDIA-RTX/godot, filtered on this fork, raster for reference](docs/media/texture-filtering.jpg)

NVIDIA samples full resolution at every distance, so distant tiles turn to moire. The fork picks the mip level from each ray's footprint, like raster (bottom).
</details>

<details>
<summary>Emissive lights off vs on</summary>

![A pavement lit by a glowing window: dark and speckled with the setting off, evenly lit with it on](docs/media/emissive-lights.jpg)

4 samples per pixel, both sides brightened equally. Look at the pavement under the window: off, it is only lit when a bounce ray happens to hit the window.
</details>

<details>
<summary>Light visibility memory off vs on</summary>

![A street among buildings with lamps hidden inside them: noisy with the setting off, much cleaner with it on](docs/media/light-visibility-memory.jpg)

4 samples per pixel. Lamps inside the buildings are in range but hidden; off, they keep taking samples that end in shadow.
</details>

<details>
<summary>Radiance cache off vs on, equal time</summary>

![The same street at 4 bounces and equal render time: 4 samples per pixel with the cache off, 6 with it on](docs/media/radiance-cache.jpg)

4 bounces. The cache shortens paths, so the same time buys 6 samples per pixel instead of 4.
</details>

<details>
<summary>Large worlds: 65 km from the origin, absolute vs camera-relative</summary>

![A cube, a mirror ball and a lamp 65 km from the origin: speckled cube and a broken reflection with absolute positions, the same as at the origin with camera-relative positions](docs/media/far-origin.jpg)

`relative_origin_grid` 0 (left) and default (right). With absolute positions the lit face speckles and the reflection lands in the wrong place.
</details>

<details>
<summary>Exposure: average, histogram, histogram with dark-scene key</summary>

![A night street with lamp posts: dark with average metering, readable with histogram metering, darker again with the dark-scene key](docs/media/night-exposure.jpg)

Average metering lets the bulbs pull the street dark; histogram metering ignores them; the dark-scene key keeps it a night scene.
</details>

<details>
<summary>Local exposure off vs on</summary>

![The night street with histogram metering: hot spots on lit walls and black facades on the left, then with local exposure: the walls keep their texture and the facades read](docs/media/local-exposure.jpg)

Highlight and shadow contrast at 0.5: hot spots under the lamps flatten, facades lift, edges stay sharp.
</details>

## References

| Area | Sources |
|---|---|
| Code | [NVIDIA-RTX/godot](https://github.com/NVIDIA-RTX/godot), the path tracer and DLSS integration this fork is built on<br>[uno1982/godot](https://github.com/uno1982/godot) `feature/physx5-module` (Wild Ox Studios, MIT), the PhysX 5 module; [NVIDIA-Omniverse/PhysX](https://github.com/NVIDIA-Omniverse/PhysX) (BSD-3-Clause), PhysX, Blast and Flow |
| Many lights | Talbot et al., *Importance Resampling for Global Illumination*, EGSR 2005<br>Boksansky et al., *Rendering Many Lights with Grid-Based Reservoirs* (ReGIR), Ray Tracing Gems II, 2021<br>Bitterli et al., *Spatiotemporal Reservoir Resampling* (ReSTIR DI), SIGGRAPH 2020<br>Olsson et al., *Clustered Deferred and Forward Shading*, HPG 2012<br>Narkowicz and Costa, *MegaLights: Stochastic Direct Lighting*, SIGGRAPH Advances 2025<br>NVIDIA, Spatial Hash Radiance Cache (SHaRC), RTXGI SDK |
| Lit transparents | Wronski, *Volumetric Fog: Unified Compute Shader Based Solution to Atmospheric Scattering*, SIGGRAPH 2014<br>Hillaire, *Physically Based and Unified Volumetric Rendering in Frostbite*, SIGGRAPH 2015<br>NVIDIA, RTX Remix volumetric radiance cache for particles<br>Garg, Nayar, *Photorealistic Rendering of Rain Streaks*, SIGGRAPH 2006<br>Henyey, Greenstein, *Diffuse Radiation in the Galaxy*, Astrophysical Journal, 1941<br>McGuire, Bavoil, *Weighted Blended Order-Independent Transparency*, JCGT 2013 |
| Large worlds | Wächter, Binder, *A Fast and Robust Method for Avoiding Self-Intersection*, Ray Tracing Gems, 2019<br>[valence-engine-rtx](https://github.com/Rman887/valence-engine-rtx), camera-relative tracing and the ray offset floor |
| Exposure | Reinhard et al., *Photographic Tone Reproduction for Digital Images*, SIGGRAPH 2002<br>Epic Games, Unreal Engine *Auto Exposure* histogram metering<br>Krawczyk, Myszkowski, Seidel, *Lightness Perception in Tone Reproduction for High Dynamic Range Images*, Eurographics 2005<br>Chen, Paris, Durand, *Real-time Edge-Aware Image Processing with the Bilateral Grid*, SIGGRAPH 2007<br>Epic Games, Unreal Engine 5.1 *Local Exposure* |
| Texture filtering | Akenine-Möller et al., *Texture Level of Detail Strategies for Real-Time Ray Tracing*, Ray Tracing Gems, 2019<br>Akenine-Möller et al., *Improved Shader and Texture Level of Detail Using Ray Cones*, JCGT 2021 |

## Everything else

Docs, build instructions and community: [godotengine/godot](https://github.com/godotengine/godot) and [docs.godotengine.org](https://docs.godotengine.org). Build this fork the same way.

MIT, like Godot. Third-party licenses in `thirdparty/` and `COPYRIGHT.txt`. NVIDIA binaries stay under NVIDIA's terms.
