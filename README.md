# Godot, path tracing improved

[![Release](https://github.com/pcanilho/godot/actions/workflows/release.yml/badge.svg)](https://github.com/pcanilho/godot/actions/workflows/release.yml) [![CodeQL](https://github.com/pcanilho/godot/actions/workflows/codeql.yml/badge.svg)](https://github.com/pcanilho/godot/actions/workflows/codeql.yml) [![Latest release](https://img.shields.io/github/v/release/pcanilho/godot?label=latest)](https://github.com/pcanilho/godot/releases/latest)

Godot 4 with real-time path tracing on Vulkan and DLSS Ray Reconstruction. Built on [NVIDIA-RTX/godot](https://github.com/NVIDIA-RTX/godot).

**Download:** [latest release](https://github.com/pcanilho/godot/releases/latest) for Windows, macOS (universal) and Linux (x86_64, arm64).
- Windows: full path tracing with DLSS Ray Reconstruction. Get the DLSS, Streamline and Reflex DLLs from NVIDIA and place them next to the exe.
- Linux: path tracing without Ray Reconstruction.
- macOS: raster only.

**Changes:** [CHANGELOG.md](CHANGELOG.md).

## What this fork adds

### Lighting
- Up to 4096 lights per scene (upstream: 64 per frame), via a light grid and resampled importance sampling.
- Traced volumetric fog lit by sun, omni and spot lights, through FogVolumes.
- Particles and transparents lit by the path tracer: a traced froxel light cache with shadowed lights, sky and bounce.

### Materials and textures
- Texture sharpness matches raster: ray-cone mip selection, mipmap bias, anisotropic filtering.
- Emission without a texture, uv1 triplanar, UV2, ALPHA in custom shaders, `IN_SHADOW_PASS`.
- Shadow-only casters bounce light; Cast Shadow Off is respected.
- Orthographic cameras.

### DLSS Ray Reconstruction
- Transparents and particles drawn at output size, unjittered.
- Additive, subtractive and `blend_mul` surfaces composite correctly.
- Emissive detail keeps its edges.
- Correct motion vectors for MultiMesh instances.
- Frame generation presents generated frames.

### Performance
- Pipeline variants compile on a worker with a SPIR-V disk cache; no stall on settings changes.
- Cached shadows for still casters; far lights update less often.
- Optional time-sliced reflection probes.

### API
- `RenderingServer.is_pathtracing_ready()`
- `RenderingDevice.get_video_memory_budget()` / `get_video_memory_usage()`

### Stability
- Fixes for BLAS double frees, leaks, shutdown crashes and threading races.
- VK_EXT_device_fault reports on device loss.

## References

| Area | Papers |
|---|---|
| Many lights | Talbot et al., *Importance Resampling for Global Illumination*, EGSR 2005<br>Boksansky et al., *Rendering Many Lights with Grid-Based Reservoirs* (ReGIR), Ray Tracing Gems II, 2021<br>Bitterli et al., *Spatiotemporal Reservoir Resampling* (ReSTIR DI), SIGGRAPH 2020<br>Olsson et al., *Clustered Deferred and Forward Shading*, HPG 2012 |
| Traced fog | Kulla, Fajardo, *Importance Sampling Techniques for Path Tracing in Participating Media*, EGSR 2012<br>Sun et al., *A Practical Analytic Single Scattering Model for Real Time Rendering*, SIGGRAPH 2005<br>Lin, Wyman, Yuksel, *Fast Volume Rendering with Spatiotemporal Reservoir Resampling*, SIGGRAPH Asia 2021 |
| Lit transparents | Wronski, *Volumetric Fog: Unified Compute Shader Based Solution to Atmospheric Scattering*, SIGGRAPH 2014<br>Hillaire, *Physically Based and Unified Volumetric Rendering in Frostbite*, SIGGRAPH 2015<br>NVIDIA, RTX Remix volumetric radiance cache for particles |
| Texture filtering | Akenine-Möller et al., *Texture Level of Detail Strategies for Real-Time Ray Tracing*, Ray Tracing Gems, 2019<br>Akenine-Möller et al., *Improved Shader and Texture Level of Detail Using Ray Cones*, JCGT 2021 |

## Everything else

Docs, build instructions and community: [godotengine/godot](https://github.com/godotengine/godot) and [docs.godotengine.org](https://docs.godotengine.org). Build this fork the same way.

MIT, like Godot. Third-party licenses in `thirdparty/` and `COPYRIGHT.txt`. NVIDIA binaries stay under NVIDIA's terms.
