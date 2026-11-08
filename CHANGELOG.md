# Changelog

What changed in this fork, newest first. Upstream Godot's own changes are in its release notes.

Every pull request adds a line under **Unreleased**, in one of: Added, Changed, Fixed, Removed. Write what changes for someone using the engine, in one line, with the PR number. The release workflow publishes the Unreleased entries as the release notes and moves them under that release's heading.

## Unreleased

### Changed
- README: DLSS Ray Reconstruction checked with the DLSS 4.5 DLLs (310.5) on an RTX 4090 (#112).

### Fixed
- Path tracer: color backgrounds (Background mode Custom Color or Clear Color) show behind the scene as in raster; camera rays that missed read the empty radiance map and came out black (#111).
- Release workflow: only commits on main are published as releases; a run on any other branch builds the zips as workflow artifacts only. build-3c75a9b9 was a test build of an unmerged branch published by mistake.

## build-3c75a9b9 (2026-11-08)

Test build of an unmerged branch, published by mistake. It is not a build of main; use build-5437261e or later.

## build-5437261e (2026-11-08)

### Added
- Path tracer: radiance cache. Rough surfaces learn, in hashed world-space cells sized by distance from the camera, the light they send back (own lighting and further bounces); a path that bounces onto one reads the cell and ends. One pixel in eight trains it each frame. Test streets at 4 bounces, equal render time: 25.4 to 27.1 dB and 20.1 to 21.1 dB. Off under environment fog. Setting `rendering/pathtracing/radiance_cache` (#110).
- README: side-by-side comparisons with NVIDIA-RTX/godot (many lights, glass shadows, texture filtering) and the test scenes that render them, in `docs/media` (#106).
- Path tracer: emissive surfaces as lights. Opaque StandardMaterial3D surfaces that emit (up to 1024 a frame, the strongest at the camera) get a light sample per shaded point: one picked by estimated power, then a triangle and a point on it, with the emission texture read there; bounce rays that hit them are weighed by multiple importance sampling (power heuristic). Sides follow the material's culling. Test street lit by signs at 4 spp: 21.2 to 24.0 dB, brightness within 0.1% of before at 1024 spp (#105).
- Path tracer: light visibility memory. Each part of the scene remembers which lights lit it lately; light selection draws half its candidates from those and weights the rest by how often they were blocked, so hidden lamps and a roofed-over sun stop taking samples. Test city street at 4 spp: 19.0 to 25.3 dB (direct light alone 21.8 to 33.0 dB); open street 17.8 to 20.1 dB. Off: `rendering/pathtracing/light_visibility_memory` (#104).
- Research note `docs/ideas/physx.md`: research pass (Arkham PhysX/APEX, Unreal Chaos destruction and cloth, heterogeneous volumes, volume path tracing papers, DLSS RR and volumes), which module features work with Jolt, and the design for path-traced smoke, cloth and props (#103).
- Research note `docs/ideas/witcher3-remaster.md`: what The Witcher 3 Remastered's path tracer suggests for the fork (ReSTIR reuse for many lights, DLSS 4.5 RR, hair fallback, exposure in dark scenes, streaming ahead of the camera) (#99).
- Research note `docs/ideas/other-engines.md`: what Unreal's MegaLights, Assassin's Creed Shadows, Doom: The Dark Ages, Alan Wake 2 and Star Wars Outlaws suggest for the fork (visibility-aware light selection, camera-following radiance cache, open-world BLAS streaming, foliage opacity test) (#100).
- Research note `docs/ideas/water.md`: rivers, waterfalls, lakes and ocean in Crimson Desert, Unreal's Water plugin, Fluid Flux and Horizon Forbidden West, and what the fork could take (flow-mapped spline rivers, FFT ocean, water medium in the path tracer, caustics) (#101).
- Research note `docs/ideas/weather.md`: clouds (Nubis), sky (Hillaire), wind, wetness, snow, fog and lightning for an open world, and how each fits the path tracer (#102).

### Changed
- Path tracer: the light grid follows the camera (256 m each way), so lights spread over a large world no longer coarsen its cells; lights reaching past it still light points outside it. A street of 460 lamps with 2000 more over 10 km: 11.7 dB to 17.8 dB at 4 spp, same as the street alone (#98).

### Fixed
- Path tracer: materials set to an anisotropic texture filter are now filtered anisotropically, and nearest-filter materials point sampled, as in raster. The filter never reached the path tracer, so grazing floors blurred in the middle distance (#108).
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
