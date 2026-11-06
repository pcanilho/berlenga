# Godot, path tracing improved

[![Release](https://github.com/pcanilho/godot/actions/workflows/release.yml/badge.svg)](https://github.com/pcanilho/godot/actions/workflows/release.yml) [![CodeQL](https://github.com/pcanilho/godot/actions/workflows/codeql.yml/badge.svg)](https://github.com/pcanilho/godot/actions/workflows/codeql.yml) [![Latest release](https://img.shields.io/github/v/release/pcanilho/godot?label=latest)](https://github.com/pcanilho/godot/releases/latest)

Godot 4 with real-time path tracing on Vulkan and DLSS Ray Reconstruction. Built on [NVIDIA-RTX/godot](https://github.com/NVIDIA-RTX/godot).

**Download:** [latest release](https://github.com/pcanilho/godot/releases/latest) for Windows, macOS (universal) and Linux (x86_64, arm64).
- Windows: full path tracing with DLSS Ray Reconstruction. Get the DLSS, Streamline and Reflex DLLs from NVIDIA and place them next to the exe.
- Linux: path tracing without Ray Reconstruction.
- macOS: raster only.

**Changes:** [CHANGELOG.md](CHANGELOG.md).

## What this fork adds

Based on NVIDIA-RTX/godot of 2 November 2026, which brings path traced transparency, traced particles and decals, volumetric fog with traced shadows, shadow groups and asynchronous pipeline compilation. The fork keeps NVIDIA's code wherever it covers the same ground, and adds:

### Lighting
- Up to 4096 lights per scene (upstream: 64 per frame), via a light grid around the camera and resampled importance sampling, so lights over a large world stay cheap.
- Shadow-only casters bounce light, so hidden ceilings and walls still light a room.
- Raster transparents under the path tracer (`rendering/pathtracing/max_transparency_layers` 0): lit by a traced froxel light cache with shadowed lights, sky and bounce; particles scatter forward, so rain glints against lamps and headlights. Reflections and refractions still trace them (rain in puddles and glass).

### Materials and textures
- Texture sharpness matches raster: ray-cone mip selection, mipmap bias, anisotropic filtering.
- Back faces pass their own normal to `vertex()` code; `IN_SHADOW_PASS` in custom shaders.

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

## References

| Area | Sources |
|---|---|
| Code | [NVIDIA-RTX/godot](https://github.com/NVIDIA-RTX/godot), the path tracer and DLSS integration this fork is built on |
| Many lights | Talbot et al., *Importance Resampling for Global Illumination*, EGSR 2005<br>Boksansky et al., *Rendering Many Lights with Grid-Based Reservoirs* (ReGIR), Ray Tracing Gems II, 2021<br>Bitterli et al., *Spatiotemporal Reservoir Resampling* (ReSTIR DI), SIGGRAPH 2020<br>Olsson et al., *Clustered Deferred and Forward Shading*, HPG 2012 |
| Lit transparents | Wronski, *Volumetric Fog: Unified Compute Shader Based Solution to Atmospheric Scattering*, SIGGRAPH 2014<br>Hillaire, *Physically Based and Unified Volumetric Rendering in Frostbite*, SIGGRAPH 2015<br>NVIDIA, RTX Remix volumetric radiance cache for particles<br>Garg, Nayar, *Photorealistic Rendering of Rain Streaks*, SIGGRAPH 2006<br>Henyey, Greenstein, *Diffuse Radiation in the Galaxy*, Astrophysical Journal, 1941<br>McGuire, Bavoil, *Weighted Blended Order-Independent Transparency*, JCGT 2013 |
| Texture filtering | Akenine-Möller et al., *Texture Level of Detail Strategies for Real-Time Ray Tracing*, Ray Tracing Gems, 2019<br>Akenine-Möller et al., *Improved Shader and Texture Level of Detail Using Ray Cones*, JCGT 2021 |

## Everything else

Docs, build instructions and community: [godotengine/godot](https://github.com/godotengine/godot) and [docs.godotengine.org](https://docs.godotengine.org). Build this fork the same way.

MIT, like Godot. Third-party licenses in `thirdparty/` and `COPYRIGHT.txt`. NVIDIA binaries stay under NVIDIA's terms.
