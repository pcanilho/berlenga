# Godot, path tracing fork

Godot 4 with real-time path tracing on Vulkan and DLSS Ray Reconstruction. Built on [NVIDIA-RTX/godot](https://github.com/NVIDIA-RTX/godot).

**Download:** [latest Windows editor](https://github.com/pcanilho/godot/releases/latest). Exes only; get the DLSS, Streamline and Reflex DLLs from NVIDIA and place them next to the exe.

## What this fork adds

### Lighting
- Up to 4096 lights per scene (upstream: 64 per frame), via a light grid and resampled importance sampling.
- Traced volumetric fog lit by sun, omni and spot lights, through FogVolumes.

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
| Texture filtering | Akenine-Möller et al., *Texture Level of Detail Strategies for Real-Time Ray Tracing*, Ray Tracing Gems, 2019<br>Akenine-Möller et al., *Improved Shader and Texture Level of Detail Using Ray Cones*, JCGT 2021 |

## Everything else

Docs, build instructions and community: [godotengine/godot](https://github.com/godotengine/godot) and [docs.godotengine.org](https://docs.godotengine.org). Build this fork the same way.

MIT, like Godot. Third-party licenses in `thirdparty/` and `COPYRIGHT.txt`. NVIDIA binaries stay under NVIDIA's terms.
