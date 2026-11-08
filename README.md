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

## What this fork adds

Based on NVIDIA-RTX/godot of 2 November 2026, which brings path traced transparency, traced particles and decals, volumetric fog with traced shadows, shadow groups and asynchronous pipeline compilation. The fork keeps NVIDIA's code wherever it covers the same ground, and adds:

### Lighting
- Up to 4096 lights per scene (upstream: 64 per frame), via a light grid around the camera and resampled importance sampling, so lights over a large world stay cheap. A street of 460 lamps with 2000 more over 10 km keeps the quality of the street alone (17.8 dB at 4 samples per pixel).
- Emissive surfaces are lights: glowing signs, screens and windows (StandardMaterial3D, textured or not) get their own light sample, weighed against bounce rays by multiple importance sampling. On a street lit only by signs: 21.2 to 24.0 dB at 4 spp, +2.5 dB at equal time.
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
- Raster transparents are laid over Ray Reconstruction's output, particles at output size and unjittered, so thin rain stays continuous.
- Additive, subtractive and `blend_mul` surfaces composite correctly.
- Emissive detail keeps its edges.
- Ray Reconstruction without DLSS upscaling warns instead of failing silently.
- Frame generation presents generated frames.

### Performance
- Cached shadows for still casters; far lights update less often.
- Optional time-sliced reflection probes.

### API
- `RenderingServer.is_pathtracing_ready()`: true once `get_rendering_not_ready_reasons()` is empty.
- `RenderingDevice.get_video_memory_budget()` / `get_video_memory_usage()`

### Stability
- Fixes for leaks, shutdown crashes and threading races; storage images declared with their textures' formats.
- VK_EXT_device_fault reports on device loss.

## Comparisons

Same scene, same samples per pixel, no denoiser, rendered with NVIDIA-RTX/godot of 2 November 2026 and with this fork. The scenes are in [docs/media/showcase](docs/media/showcase): `godot --path docs/media/showcase -- --shot=city --spp=16 --out=city.png` (shots: `city`, `lights`, `textures`, `glass`).

<details>
<summary>Many lights: a city block grid of 1024 lamps (the image at the top)</summary>

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

Top: NVIDIA-RTX/godot has no texture level of detail in its ray tracing shaders and samples the full resolution texture at every distance, so the floor turns to moire. Middle: this fork. Bottom: the raster renderer for reference.
</details>

## References

| Area | Sources |
|---|---|
| Code | [NVIDIA-RTX/godot](https://github.com/NVIDIA-RTX/godot), the path tracer and DLSS integration this fork is built on |
| Many lights | Talbot et al., *Importance Resampling for Global Illumination*, EGSR 2005<br>Boksansky et al., *Rendering Many Lights with Grid-Based Reservoirs* (ReGIR), Ray Tracing Gems II, 2021<br>Bitterli et al., *Spatiotemporal Reservoir Resampling* (ReSTIR DI), SIGGRAPH 2020<br>Olsson et al., *Clustered Deferred and Forward Shading*, HPG 2012<br>Narkowicz and Costa, *MegaLights: Stochastic Direct Lighting*, SIGGRAPH Advances 2025<br>NVIDIA, Spatial Hash Radiance Cache (SHaRC), RTXGI SDK |
| Lit transparents | Wronski, *Volumetric Fog: Unified Compute Shader Based Solution to Atmospheric Scattering*, SIGGRAPH 2014<br>Hillaire, *Physically Based and Unified Volumetric Rendering in Frostbite*, SIGGRAPH 2015<br>NVIDIA, RTX Remix volumetric radiance cache for particles<br>Garg, Nayar, *Photorealistic Rendering of Rain Streaks*, SIGGRAPH 2006<br>Henyey, Greenstein, *Diffuse Radiation in the Galaxy*, Astrophysical Journal, 1941<br>McGuire, Bavoil, *Weighted Blended Order-Independent Transparency*, JCGT 2013 |
| Texture filtering | Akenine-Möller et al., *Texture Level of Detail Strategies for Real-Time Ray Tracing*, Ray Tracing Gems, 2019<br>Akenine-Möller et al., *Improved Shader and Texture Level of Detail Using Ray Cones*, JCGT 2021 |

## Everything else

Docs, build instructions and community: [godotengine/godot](https://github.com/godotengine/godot) and [docs.godotengine.org](https://docs.godotengine.org). Build this fork the same way.

MIT, like Godot. Third-party licenses in `thirdparty/` and `COPYRIGHT.txt`. NVIDIA binaries stay under NVIDIA's terms.
