# Changelog

What changed in this fork, newest first. Upstream Godot's own changes are in its release notes.

Every pull request adds a line under **Unreleased**, in one of: Added, Changed, Fixed, Removed. Write what changes for someone using the engine, in one line, with the PR number. The release workflow publishes the Unreleased entries as the release notes and moves them under that release's heading.

## Unreleased

### Fixed
- Fog, sky, particle and texture blit shaders no longer fail to compile when two of them compile at once on the main and rendering threads (separate rendering thread) (#96).

## build-a3c71eda (2026-11-06)

### Added
- Research notes: one short attribution rule; outside projects are credited in the README References (#92).
- Research note `docs/ideas/physx.md`: PhysX 5 physics server module (uno1982/godot) assessed for later: features, licences, GPU sync cost under path tracing, Flow gas not traced, platforms, CI cost (#91).
- Research note `docs/ideas/valence-engine-rtx-takeaways.md`: RTX 4090 extension check (no linear swept spheres; cluster AS, partitioned TLAS and opacity micromaps present) (#90).
- Research note `docs/ideas/godot-rtx-forks-survey.md`: what other Godot RTX forks add and what is worth taking, with attribution (#89).
- Research note `docs/ideas/valence-engine-rtx-takeaways.md`: general engine fixes worth taking (index variants, pipeline precompile, blend shapes, Jolt cast_motion, render graph barriers) and how anything taken from it is attributed (#88).
- Research note `docs/ideas/valence-engine-rtx-takeaways.md`: open-world additions (strand hair, subdivision, sky sampling, water medium, subsurface) (#87).
- Research note `docs/ideas/valence-engine-rtx-takeaways.md`: how valence-engine-rtx implements light trees, emissive lights, material lobes, opacity micromaps, cluster LOD and partitioned TLAS, with the measured traps, for later reimplementation (#86).
- Research note `docs/ideas/valence-engine-rtx.md`: ranking of its features for a large open world (cluster LOD, partitioned TLAS, streaming, light tree) (#85).
- Research note `docs/ideas/valence-engine-rtx.md`: assessment of another NVIDIA path tracer fork and which of its features to reimplement here (#84).
- Research note `docs/ideas/metal-path-tracing.md`: assessment of a Metal (macOS) path tracing port and how it would fit this fork (#83).
- Path tracer: a new pipeline variant (path tracing turned on, a samples, bounces or denoiser change) builds on a worker thread; meanwhile the view keeps tracing with the previous variant, or draws raster for those frames, instead of stalling.
- README: NVIDIA-RTX/godot credited in the References.
- Path tracer: compiled shader stages are cached on disk and shared between settings that differ only in samples or bounces, so a second launch or a samples change skips the shader compile.
- Release workflow: a `build_only` run builds any branch and keeps the zips as workflow artifacts, without publishing a release.

### Changed
- Rebased on NVIDIA-RTX/godot of 2 November 2026: path traced transparency (on by default, `rendering/pathtracing/max_transparency_layers`), traced particles, decals and volumetric fog shadows, shadow groups, dynamic BLAS budgets, asynchronous pipeline compilation (`get_rendering_not_ready_reasons()`). Where NVIDIA now covers a fork change (orthographic rays, triplanar, UV2 for custom shaders, MultiMesh motion, BLAS lifetime, traced fog) the fork uses NVIDIA's.
- Kept on top: the 4096-light grid with resampled light selection, ray-cone texture filtering, emission in the Ray Reconstruction guide, frame generation fixes, device fault reports, storage image formats. With `max_transparency_layers` 0 transparents use the fork's raster layer over Ray Reconstruction with the traced light cache, while reflections and refractions still trace them.

### Fixed
- Path tracer: transparent surfaces filter shadow rays by their coverage instead of a random coin flip, so clear glass casts an even, light shadow without blotches (#94).
- DLSS: resources are tagged for the frame being rendered, so Ray Reconstruction, Super Resolution and NIS work with the separate rendering thread.
- DLSS: the render size clamp leaves a size that a Ray Reconstruction mode accepts, so a driver override that pins Super Resolution no longer shrinks a path-traced view; a failed Ray Reconstruction evaluate names the inputs it was given.
- Release retry with immutable releases: while a build waits for a runner the release stays a draft, and is published once the retry has added its zip (or after the third attempt) (#82).
- Release retry: its job had five minutes, all spent waiting before the re-run (#81).

## build-3b0494b5 (2026-11-05)

### Fixed
- A release build that GitHub found no runner for (macOS when hosted capacity is short) is retried automatically, and the re-run adds its zip to the existing release instead of failing (#80).

### Changed
- Release notes list the downloads in a table: one row per build with its link and the zip's SHA-256; the executables' hashes are in a collapsed section (#80).

## build-ad774282 (2026-11-05)

### Added
- Path tracer: particles show in reflections and refractions (rain in puddles and glass), lit from the light cache; `rendering/pathtracing/transparent_lighting/particles_in_reflections` and `particle_reflection_distance` (#79).

### Fixed
- CodeQL scans run to completion: a push to `main` waits behind a running scan instead of cancelling it (#77).

## build-2d41bc16 (2026-11-05)

### Added
- Path tracer: particles lit by a forward-scattering phase function from the light cache, so rain glints in front of lamps and headlights; `rendering/pathtracing/transparent_lighting/particle_anisotropy` (#76).
- Path tracer lights particles and transparents: a traced light cache with shadowed lights, sky and bounce replaces the raster lights and ambient for them; `rendering/pathtracing/transparent_lighting/*` settings (#74).
- CHANGELOG.md: release notes come from its Unreleased entries, and pull requests are checked for one (#73).

## build-57db5e08 (2026-11-05)

### Fixed
- Linux arm64 release build used an x86_64 Python (#71).

## build-c28b7788 (2026-11-05)

### Added
- Linux (x86_64, arm64) and macOS (universal, raster only) editor builds in each release (#66, #67).
- CodeQL scan of the C++ sources (#67).

### Changed
- Releases are built on GitHub Actions: on demand, and every Monday when `main` has moved (#65).
- CI actions pinned by commit to their latest releases, least-privilege tokens, Dependabot for action updates (#68).

### Fixed
- Path tracer: storage images declared with their textures' formats (#70).

### Removed
- Upstream's CI workflows and unused composite actions (#67).

## build-c7e111a0 (2026-11-05)

### Fixed
- Crash when resizing the window: the screen pass drew past the swap chain image (#64).

## fixes-85b5084a (2026-11-05)

First release.

### Added
- `RenderingDevice.get_video_memory_budget()` and `get_video_memory_usage()` (#62).
- `RenderingServer.is_pathtracing_ready()`; path tracer pipeline variants compile on a worker with a SPIR-V disk cache (#59).

### Changed
- Path tracer samples textures with raster's mipmap bias and ray-cone gradients (#55).
- Path tracing with Ray Reconstruction draws `blend_mul` transparents over the output (#56).

### Fixed
- Editor help no longer runs doc work after the editor is freed (#60).
- Vulkan device fault reports kept when the driver returns VK_INCOMPLETE (#52); deviceFault feature enabled before use (#40).
- WorkerThreadPool releases task Callables before scripting languages finish (#48, #50).
- ClassDB default property value cache guarded for worker threads (#46).
- PrimitiveMesh builds surface arrays on the CPU instead of reading them back (#44).
- Path tracer gives custom shaders the mesh's UV2 (#42).
