# Godot, path tracing improved

[![Release](https://github.com/pcanilho/godot/actions/workflows/release.yml/badge.svg)](https://github.com/pcanilho/godot/actions/workflows/release.yml) [![CodeQL](https://github.com/pcanilho/godot/actions/workflows/codeql.yml/badge.svg)](https://github.com/pcanilho/godot/actions/workflows/codeql.yml) [![Latest release](https://img.shields.io/github/v/release/pcanilho/godot?label=latest)](https://github.com/pcanilho/godot/releases/latest)

Godot 4 with real-time path tracing on Vulkan and DLSS Ray Reconstruction. Built on [NVIDIA-RTX/godot](https://github.com/NVIDIA-RTX/godot).

**Download:** [latest release](https://github.com/pcanilho/godot/releases/latest) for Windows, macOS (universal) and Linux (x86_64, arm64).
- Windows: full path tracing with DLSS Ray Reconstruction. Get the DLSS, Streamline and Reflex DLLs from NVIDIA and place them next to the exe.
- Linux: path tracing without Ray Reconstruction.
- macOS: raster only.

**Changes:** [CHANGELOG.md](CHANGELOG.md).

![1024 street lamps path traced at 16 samples per pixel: NVIDIA-RTX/godot on the left keeps 64 of them, this fork on the right lights them all](docs/media/many-lights.jpg)

*1024 omni lights, 16 samples per pixel, no denoiser. Left: NVIDIA-RTX/godot (64 lights per frame). Right: this fork.*

**About the images in this README:** they are raw path tracer output, almost all rendered on the CPU with a software Vulkan driver (only the fork and raster strips of the texture comparison come from a GPU). None of them uses a denoiser or DLSS Ray Reconstruction, so the grain you see is what the denoiser receives; on a GPU with Ray Reconstruction the final frames are clean. They compare the quality of the raw samples, not finished frames.

## For open-world games

The fork is built with large open worlds in mind, where the limits are float precision far from the origin, the number of lights and the cost of lighting what lies far from the camera:
- Camera-relative tracing keeps shadows, reflections and contact points exact kilometres from the world origin.
- The light grid follows the camera and holds up to 4096 lights, so a city or a road of lamps stays cheap; light visibility memory stops lights hidden behind terrain or buildings from taking samples.
- Emissive surfaces light their surroundings without placing a light for each sign or window.
- The radiance cache is camera-following and world-space, so it covers any map size at a fixed 16 MiB.
- PhysX cloth, destruction, smoke and water work alongside Jolt.

Next for large worlds, in order: RTX Mega Geometry (cluster acceleration structures with level of detail, so distant geometry stays in the trace at low cost), partitioned acceleration structures for scenes with many static objects, and geometry streaming. Features that change the look or the cost have a project setting to turn them off. Research notes are in [docs/ideas](docs/ideas).

## What this fork adds

Based on NVIDIA-RTX/godot of 2 November 2026, which brings path traced transparency, traced particles and decals, volumetric fog with traced shadows, shadow groups and asynchronous pipeline compilation. The fork keeps NVIDIA's code wherever it covers the same ground, and adds:

### Lighting
- Up to 4096 lights per scene (upstream: 64 per frame), via a light grid around the camera and resampled importance sampling, so lights over a large world stay cheap. A street of 460 lamps with 2000 more over 10 km keeps the quality of the street alone (17.8 dB at 4 samples per pixel).
- Emissive surfaces are lights: glowing signs, screens and windows (StandardMaterial3D, textured or not) get their own light sample, weighed against bounce rays by multiple importance sampling. On a street lit only by signs: 21.2 to 24.0 dB at 4 spp, +2.5 dB at equal time (`rendering/pathtracing/emissive_lights`).
- Light selection remembers which lights actually lit each part of the scene, so lamps behind walls or a sun under a roof stop taking samples (`rendering/pathtracing/light_visibility_memory`). On a test street among lit buildings: 19.0 to 25.3 dB at 4 spp, +5.6 dB at equal time.
- Radiance cache: rough surfaces learn, in world-space cells around the camera, the light they send back, and a path that bounces onto one reads it there instead of tracing on (`rendering/pathtracing/radiance_cache`). On test streets at 4 bounces and equal render time, 1.0 to 1.7 dB less error; what the camera sees directly is never cached.
- Shadow-only casters bounce light, so hidden ceilings and walls still light a room.
- Raster transparents under the path tracer (`rendering/pathtracing/max_transparency_layers` 0): lit by a traced froxel light cache with shadowed lights, sky and bounce; particles scatter forward, so rain glints against lamps and headlights. Reflections and refractions still trace them (rain in puddles and glass).

### Materials and textures
- Textures filter like raster instead of aliasing at a distance: ray-cone mip selection, mipmap bias, anisotropic filtering.
- Back faces pass their own normal to `vertex()` code; `IN_SHADOW_PASS` in custom shaders.

### Glass
- Transparent surfaces filter shadow rays by their coverage, so clear glass casts an even, tinted shadow instead of a speckled one.

### DLSS Ray Reconstruction
- Runs with NVIDIA's DLSS 4.5 DLLs (nvngx_dlssd 310.5). The model is chosen with `rendering/streamline/dlss_ray_reconstruction_preset`.
- Raster transparents are laid over Ray Reconstruction's output, particles at output size and unjittered, so thin rain stays continuous.
- Additive, subtractive and `blend_mul` surfaces composite correctly.
- Emissive detail keeps its edges.
- Ray Reconstruction without DLSS upscaling warns instead of failing silently.
- Frame generation presents generated frames.

### Performance
- Cached shadows for still casters; far lights update less often.
- Optional time-sliced reflection probes.

### Large worlds
- The path tracer works in positions relative to the camera's cell on a 1024 m grid, so shadows, reflections and contact points stay exact kilometres from the world origin (`rendering/pathtracing/relative_origin_grid`, 0 for absolute positions). Without it a scene 65 km out shows speckled shadows and broken reflections, and one 16 km out is 2% darker. Shaders still see absolute world positions.
- Ray origins move off the surface by at least a few float steps of the largest coordinate and of the ray's length, so a floor near y = 0 does not shadow itself far from the origin.
- Optional mesh LOD in the path tracer (`rendering/pathtracing/mesh_lod`, off by default): static meshes trace at the level of detail the rasterizer draws them at, each level built once and shared. 120 unique rocks over 1 km: buffer memory 346 to 110 MB, frames 7% faster on the CPU, near rocks unchanged.

### Physics (PhysX 5)
Windows x64 and Linux x86_64 builds include a PhysX 5 physics server with NVIDIA Blast destruction and NVIDIA Flow smoke and fire. macOS and Linux ARM64 builds do not.
- Pick the engine per project in **Project Settings > Physics > 3D > Physics Engine**: `Jolt Physics` or `PhysX`. Every 3D physics node works with either.
- Works with either engine: `PhysXCloth3D` (free cloth: flags, tarps, umbrellas), `PhysXSkinnedCloth3D` (cloth on skinned characters, painted max distance per vertex), `PhysXDestructible3D` (Blast fracture, editor fracture tool), `PhysXFlow3D` (smoke and fire), ocean water with buoyancy and boats, compute-shader particle fluids and granular material.
- Needs the PhysX engine: PhysX vehicles, CUDA particle fluids and deformables, GPU rigid body dynamics (CUDA builds only; the release builds use the CPU solver, which is faster while the GPU is busy path tracing).
- Without the SDKs (`physx_sdk=`, `blast_sdk=`, `flow_sdk=`, see `modules/godot_physx/README.md`) the module is skipped and the build is stock.

### API
- `RenderingServer.is_pathtracing_ready()`: true once `get_rendering_not_ready_reasons()` is empty.
- `RenderingDevice.get_video_memory_budget()` / `get_video_memory_usage()`

### Stability
- Fixes for leaks, shutdown crashes and threading races; storage images declared with their textures' formats.
- VK_EXT_device_fault reports on device loss.

## Comparisons

Same scene, same samples per pixel, no denoiser, rendered with NVIDIA-RTX/godot of 2 November 2026 and with this fork. The scenes are in [docs/media/showcase](docs/media/showcase): `godot --path docs/media/showcase -- --shot=city --spp=16 --out=city.png` (shots: `city`, `lights`, `textures`, `glass`).

<details>
<summary>Many lights: a city block grid of 1024 lamps</summary>

![1024 street lamps: NVIDIA-RTX/godot keeps 64 of them, this fork lights them all](docs/media/many-lights.jpg)

NVIDIA-RTX/godot passes the 64 highest scoring lights to the path tracer each frame; the rest go dark. The fork samples all of them through the light grid.
</details>

<details>
<summary>Glass shadows: a clear pane in sunlight</summary>

![Glass pane shadow: speckled on NVIDIA-RTX/godot, even and tinted on this fork](docs/media/glass-shadow.jpg)

16 samples per pixel. Shadow rays through the pane pass or stop at random on NVIDIA-RTX/godot; the fork weights them by the pane's coverage and colour.
</details>

<details>
<summary>Texture filtering: a checker floor to the horizon</summary>

![Checker floor: moire on NVIDIA-RTX/godot, filtered on this fork, raster for reference](docs/media/texture-filtering.jpg)

Top: NVIDIA-RTX/godot. Its ray tracing shaders pick no texture level of detail, so distant tiles are sampled at full resolution and break up into moire. Middle: this fork. Each ray carries its footprint and samples the matching mip level with anisotropic filtering, so the floor stays clean to the horizon. Bottom: the raster renderer, for reference.
</details>

### Fork features on and off

Each of these has a project setting to turn it off. Same build, same scene, no denoiser, 960x540. The scenes are in [docs/media/light-selection](docs/media/light-selection): `godot --path docs/media/light-selection --resolution 960x540 -- --scene=signs --spp=4` (scenes: `signs`, `city`, `street`).

<details>
<summary>Emissive surfaces as lights (<code>rendering/pathtracing/emissive_lights</code>)</summary>

![A pavement lit by a glowing window: dark and speckled with the setting off, evenly lit with it on](docs/media/emissive-lights.jpg)

A street lit only by a glowing shop window and small signs, 4 samples per pixel, both sides brightened by the same amount. Look at the pavement under the window: off, it stays almost black with scattered dots, because light from the window is only found when a random bounce ray happens to hit it. On, every shaded point also aims a sample at the window, so the pavement and the kerb come out evenly lit and the reflection on the wet road fills in.
</details>

<details>
<summary>Light visibility memory (<code>rendering/pathtracing/light_visibility_memory</code>)</summary>

![A street among buildings with lamps hidden inside them: noisy with the setting off, much cleaner with it on](docs/media/light-visibility-memory.jpg)

4 samples per pixel. The buildings hold lamps that are in range of the street but hidden from it, and an arcade roof hides the moon. Off, those lamps keep taking samples that end in shadow; on, the scene learns which lights reach each part of it.
</details>

<details>
<summary>Radiance cache (<code>rendering/pathtracing/radiance_cache</code>)</summary>

![The same street at 4 bounces and equal render time: 4 samples per pixel with the cache off, 6 with it on](docs/media/radiance-cache.jpg)

4 bounces at equal render time: off at 4 samples per pixel, on at 6, since paths that read the cache stop early. What the camera sees directly is never cached.
</details>

<details>
<summary>Large worlds: the same scene 65 km from the world origin</summary>

![A cube, a mirror ball and a lamp 65 km from the origin: speckled cube and a broken reflection with absolute positions, the same as at the origin with camera-relative positions](docs/media/far-origin.jpg)

16 samples per pixel, `godot --path docs/media/showcase -- --shot=far --far=65536` with `rendering/pathtracing/relative_origin_grid` at 0 (left) and at its default (right). With absolute positions the cube's lit face speckles and the ball reflects the cube in the wrong place. Camera-relative, the frame matches the same scene at the origin to within noise.
</details>

## References

| Area | Sources |
|---|---|
| Code | [NVIDIA-RTX/godot](https://github.com/NVIDIA-RTX/godot), the path tracer and DLSS integration this fork is built on<br>[uno1982/godot](https://github.com/uno1982/godot) `feature/physx5-module` (Wild Ox Studios, MIT), the PhysX 5 module; [NVIDIA-Omniverse/PhysX](https://github.com/NVIDIA-Omniverse/PhysX) (BSD-3-Clause), PhysX, Blast and Flow |
| Many lights | Talbot et al., *Importance Resampling for Global Illumination*, EGSR 2005<br>Boksansky et al., *Rendering Many Lights with Grid-Based Reservoirs* (ReGIR), Ray Tracing Gems II, 2021<br>Bitterli et al., *Spatiotemporal Reservoir Resampling* (ReSTIR DI), SIGGRAPH 2020<br>Olsson et al., *Clustered Deferred and Forward Shading*, HPG 2012<br>Narkowicz and Costa, *MegaLights: Stochastic Direct Lighting*, SIGGRAPH Advances 2025<br>NVIDIA, Spatial Hash Radiance Cache (SHaRC), RTXGI SDK |
| Lit transparents | Wronski, *Volumetric Fog: Unified Compute Shader Based Solution to Atmospheric Scattering*, SIGGRAPH 2014<br>Hillaire, *Physically Based and Unified Volumetric Rendering in Frostbite*, SIGGRAPH 2015<br>NVIDIA, RTX Remix volumetric radiance cache for particles<br>Garg, Nayar, *Photorealistic Rendering of Rain Streaks*, SIGGRAPH 2006<br>Henyey, Greenstein, *Diffuse Radiation in the Galaxy*, Astrophysical Journal, 1941<br>McGuire, Bavoil, *Weighted Blended Order-Independent Transparency*, JCGT 2013 |
| Large worlds | Wächter, Binder, *A Fast and Robust Method for Avoiding Self-Intersection*, Ray Tracing Gems, 2019<br>[valence-engine-rtx](https://github.com/Rman887/valence-engine-rtx), camera-relative tracing and the ray offset floor |
| Texture filtering | Akenine-Möller et al., *Texture Level of Detail Strategies for Real-Time Ray Tracing*, Ray Tracing Gems, 2019<br>Akenine-Möller et al., *Improved Shader and Texture Level of Detail Using Ray Cones*, JCGT 2021 |

## Everything else

Docs, build instructions and community: [godotengine/godot](https://github.com/godotengine/godot) and [docs.godotengine.org](https://docs.godotengine.org). Build this fork the same way.

MIT, like Godot. Third-party licenses in `thirdparty/` and `COPYRIGHT.txt`. NVIDIA binaries stay under NVIDIA's terms.
