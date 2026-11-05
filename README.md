# Godot, path tracing fork

A Godot 4 fork for real-time path tracing on Vulkan with DLSS Ray Reconstruction. Built on [NVIDIA-RTX/godot](https://github.com/NVIDIA-RTX/godot).

Windows editor builds: [latest release](https://github.com/pcanilho/godot/releases/latest). The zip holds the exes only. The DLSS, Streamline and Reflex DLLs come from NVIDIA (see the release notes) and go next to the exe.

## What this fork adds

Path tracer
- Up to 4096 lights, sampled through a world grid of light lists with resampled importance sampling (upstream picks 64 per frame).
- Traced volumetric fog lit by the sun, omni and spot lights, through FogVolumes, with or without Ray Reconstruction.
- Texture detail matches raster: ray-cone mip selection, raster's mipmap bias, anisotropic filtering where the material asks for it.
- New pipeline variants build on a worker thread with a SPIR-V disk cache, so changing settings does not stall the render thread. `RenderingServer.is_pathtracing_ready()` reports when the current variant is in use.
- Material coverage closer to raster: emission without an emission texture, uv1 triplanar, custom shaders that write ALPHA, UV2 and `IN_SHADOW_PASS` in custom shaders, back-face normals in custom vertex code, non-2D textures.
- Shadow-only casters take part in bounces and shadow rays, opted-in casters with alpha as transmittance. Cast Shadow Off is respected.
- Orthographic cameras get parallel primary rays.

DLSS Ray Reconstruction and frame generation
- Transparents drawn in their own layer over the RR output, at output size and unjittered, so thin transparents and particles stay continuous. Additive, subtractive and `blend_mul` surfaces handled.
- Compositor effects can draw into that layer.
- Emission feeds the diffuse albedo guide, so emissive detail keeps its edges.
- Correct motion vectors for moving and merged MultiMesh instances.
- Traces without RR guides when the viewport does not scale with DLSS, instead of failing.
- Frame generation presents its generated frames, and its state is exposed to scripts.
- Render size respected when a DLSS mode is forced, and super resolution state follows a resize.

Shadows and reflections
- Positional lights keep their still casters' shadow and redraw only what moves.
- Far lamps redraw moving casters every 2nd to 4th frame. Far directional cascades can update every other frame while light and camera hold still.
- Optional time-sliced updates for UPDATE_ALWAYS reflection probes.

Engine and stability
- `RenderingDevice.get_video_memory_budget()` and `get_video_memory_usage()` (VK_EXT_memory_budget).
- Fixes for BLAS double frees, the reflection atlas leak, shutdown crashes in the worker thread pool, render thread and editor docs, texture updates from threads, and ray tracing lists racing compute.
- VK_EXT_device_fault reports kept on device loss.

## References

- Many lights: Talbot, Cline, Egbert, "Importance Resampling for Global Illumination", EGSR 2005. Boksansky, Jukarainen, Wyman, "Rendering Many Lights with Grid-Based Reservoirs" (ReGIR), Ray Tracing Gems II, 2021. Bitterli et al., "Spatiotemporal Reservoir Resampling for Real-Time Ray Tracing with Dynamic Direct Lighting" (ReSTIR DI), SIGGRAPH 2020. Olsson, Billeter, Assarsson, "Clustered Deferred and Forward Shading", HPG 2012.
- Traced fog: Kulla, Fajardo, "Importance Sampling Techniques for Path Tracing in Participating Media", EGSR 2012. Sun, Ramamoorthi, Narasimhan, Nayar, "A Practical Analytic Single Scattering Model for Real Time Rendering", SIGGRAPH 2005. Lin, Wyman, Yuksel, "Fast Volume Rendering with Spatiotemporal Reservoir Resampling", SIGGRAPH Asia 2021.
- Texture filtering: Akenine-Möller et al., "Texture Level of Detail Strategies for Real-Time Ray Tracing", Ray Tracing Gems, 2019, ch. 20. Akenine-Möller et al., "Improved Shader and Texture Level of Detail Using Ray Cones", JCGT 10(1), 2021.

Issues: [pcanilho/godot/issues](https://github.com/pcanilho/godot/issues).

## Everything else

For the engine itself, documentation, build instructions and community, see [godotengine/godot](https://github.com/godotengine/godot) and [docs.godotengine.org](https://docs.godotengine.org). Build this fork the same way. The Windows release is built with:

    scons platform=windows target=editor optimize=speed

Licensed under MIT like Godot. Third-party components keep their own licenses (see `thirdparty/` and `COPYRIGHT.txt`). NVIDIA's DLSS, Streamline and Reflex binaries are not part of this repository's releases and stay under NVIDIA's terms.
