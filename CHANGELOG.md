# Changelog

What changed in this fork, newest first. Upstream Godot's own changes are in its release notes.

Every pull request adds its entry as a file in `changelog.d/` (see [changelog.d/README.md](changelog.d/README.md)), in one of: Added, Changed, Fixed, Removed. Write what changes for someone using the engine, in one line, with the PR number. The release workflow publishes those entries as the release notes and moves them here under that release's heading.

## Unreleased

## build-50273c52 (2026-11-09)

### Added
- Transmission bounds a medium when `transmission_thin_walled` is off: `transmission_ior`, `transmission_attenuation_color` and `transmission_attenuation_distance` (glTF `KHR_materials_volume` and `KHR_materials_ior`, imported and exported). The path tracer refracts rays into and out of it with total internal reflection and absorbs every ray inside it (Beer-Lambert), shadow rays included, so water darkens and tints with depth; the rasterizer refracts the screen read by the IOR and tints it by the depth behind. Setting `rendering/pathtracing/refraction` (#159)
- Design note on refraction and a water medium in the path tracer (#158).

## build-40b63b1b (2026-11-09)

### Added
- Path tracing reads the clearcoat, back light and transmission textures, and shaders can write `TRANSMISSION`; shaders writing `TRANSMISSION` or `BACKLIGHT` pass light into their shadows (#155).

### Fixed
- Path-traced shadows of Transmission and Refraction materials let the tinted light through instead of blocking it (#155).

### Changed
- README: layer textures and shader TRANSMISSION in the material lines, with an image (#156)
- README: summary section and transmission, back lighting and clearcoat images (#154)
- README: transmission image retaken with glass shadows that pass light (#157)

## build-effa664d (2026-11-09)

### Added
- `PhysXCloth3D.tear_factor`: cloth tears by itself where it is stretched past this multiple of its rest length, splitting vertices so the rip runs across it; `torn` signal, texture coordinates on generated grids and source meshes, project setting `physics/physx_3d/cloth/tearing`.
- Design note on clearcoat and thin transmission in the path tracer (#150).
- Path-traced back lighting: BaseMaterial3D back light, bounce light through the surface, `rendering/pathtracing/back_lighting` (#152).
- Path-traced clearcoat matching the rasterizer, `rendering/pathtracing/clearcoat` (#151).
- Materials test scene, `docs/bench/materials` (#151).
- BaseMaterial3D Transmission section: thin glass in raster (screen read) and the path tracer, tinted shadows, glTF `KHR_materials_transmission`, `rendering/pathtracing/transmission` (#153).
- Path tracer: Refraction-enabled materials pass `1 - alpha` of the light through (#153).
- Path tracer: back-lit surfaces let coloured light into their shadows (#153).

### Fixed
- Path tracer: lights behind a back-lit surface no longer shadowed by the surface itself (#152).

### Changed
- README: the cloth tearing clip is collapsed, so one GIF plays by default (#149).
- README: cloth tearing line and clip in the Physics section (#148).

## build-73a3e160 (2026-11-08)

### Added
- Persistent instances also keep the per-record instances of large MultiMeshes until a record changes (#147).
- The open-world test scene can place its objects as MultiMesh tiles (`--multimesh=M`) and change records each frame (`--mmmove=N`) (#147).

## build-bd7ad4eb (2026-11-08)

### Changed
- The open-world test scene also prints the CPU render and frame setup times (#143).

### Added
- Design note on path tracing instanced forests: MultiMesh records in persistent slots, chunked forests, partitioned TLAS (#145).

### Fixed
- PhysXCloth3D: a generated grid now faces +Z as documented, and cloth normals follow the front face, so two-sided cloth is lit on the side the light falls on in raster and path tracing.

## build-35438dbd (2026-11-08)

### Changed
- Path tracer: motion vectors age out only on objects that moved, instead of visiting every listed object each frame (#139).
- Path tracer: the per-object check of persistent instances runs on the worker threads (#140).
- README: an Open worlds section with the large-world settings, a ready-to-paste project.godot block and the open-world test scene (#141).

## build-28640b78 (2026-11-08)

### Fixed
- Path tracer: `rt_vertex_dynamic` surfaces are recaptured every frame. PhysX skinned cloth no longer traces as a stale first capture (a coat left crumpled, or missing, while the raster image is right) (#138).

### Changed
- README: opacity micromaps listed under Large worlds (#137).

## build-61af2b42 (2026-11-08)

### Added
- Path tracer: opacity micromaps for alpha-scissor StandardMaterial3D surfaces (Vulkan `VK_EXT_opacity_micromap`), baked on the GPU on first trace so rays skip cut-out texels and accept solid ones without the any-hit shader; same image, opt-in with `rendering/pathtracing/opacity_micromaps` (off by default).

## build-e7f15277 (2026-11-08)

### Changed
- Path tracer: with persistent instances, the TLASes keep their instances on the GPU and each build writes only the changed ones (#135).
- Path tracer: persistent instances prefetch the instances and their slots ahead of the per-frame check, about a third less time in it (#136).
- Path tracer: the raster sun shadow casters are no longer culled while path tracing, which never draws those shadow maps (#134).

## build-8cfe788f (2026-11-08)

### Added
- Open-world bench project in `docs/bench/open_world`: rocks, trees and blocks over 2 km for path tracer timing and memory (#130).
- Research note `docs/ideas/persistent-instances.md`: persistent ray tracing instances and partitioned TLAS, from the 4090 measurements (#132).
- Path tracer: persistent instance slots (`rendering/pathtracing/persistent_instances`, off by default): unchanged objects skip the per-frame TLAS work (#133).
- README: PhysX clip (cloth, Blast destruction, rigid bodies, Flow smoke) and its test scene, `--shot=physx` in `docs/media/showcase` (#PR).
- Path tracer: simulated gas. The PhysX Flow volume nearest the camera is traced as a participating medium (delta tracking, ratio-tracked shadows, Henyey-Greenstein phase, temperature emission). Setting `rendering/pathtracing/simulated_gas` (#128).
- PhysXCloth3D.release_pins(): lets pinned vertices go without resetting the cloth, so an edge can tear free (#128).
- PhysXCloth3D.tear(): cuts the cloth along a line so it rips apart (CPU solver) (#128).
- PhysXFlowEmitter3D: an emitter with no smoke, fuel, burn or heat only pushes air, for wind on the gas; it no longer cools the gas it passes (#128).

### Changed
- README: about a third shorter; open-world summary folded into the feature list, one line per feature, shorter comparison captions (#129).
- README: trimmed to one short line per feature with its setting; papers moved to `docs/REFERENCES.md` (#131).

## build-384f62e7 (2026-11-08)

### Added
- Local exposure from a bilateral grid (`rendering/camera/auto_exposure/local_highlight_contrast`, `local_shadow_contrast`), off by default (#127).

## build-e071b5eb (2026-11-08)

### Added
- Histogram auto exposure metering (`rendering/camera/auto_exposure/metering`) and a dark-scene key (`rendering/camera/auto_exposure/dark_scene_luminance`), both off by default (#126).

### Fixed
- Auto exposure overshooting when a frame is longer than the adaptation time (#126).

## build-f7c2227f (2026-11-08)

### Added
- `rendering/pathtracing/cull_max_distance` and `rendering/pathtracing/cull_min_size`: optional distance and size limits for geometry outside the camera view in the path tracer (#125).

## build-ccfb311a (2026-11-08)

### Added
- Research note `docs/ideas/exposure.md`: auto exposure for dark scenes under path tracing (histogram metering, luminance-dependent key, exposure compensation curve, local exposure) and what the fork should take (#116).
- Project setting `rendering/pathtracing/emissive_lights` (default on) to turn off sampling emissive surfaces as lights (#115).
- Research note docs/ideas/open-world-geometry.md: RT LOD, Mega Geometry, partitioned TLAS and streaming, and the order to build them (#123).
- PhysX 5 physics engine (Windows x64, Linux x86_64), selectable per project next to Jolt, with cloth, skinned cloth, Blast destruction, Flow smoke and fire, ocean water and particle fluids; cloth, destruction, smoke and water also work with Jolt (#113).
- README: on and off comparisons for emissive lights, light visibility memory and the radiance cache, with their test scenes in `docs/media/light-selection` (#118).
- `rendering/pathtracing/mesh_lod`: static meshes trace at the LOD the rasterizer draws them at, cutting acceleration-structure memory for far geometry (#124).

### Changed
- Changelog entries are files in `changelog.d/`, one per pull request, gathered by the release, so a release made while a pull request is open no longer misplaces its entry; lines under Unreleased in CHANGELOG.md still work. The release's changelog commit retries when main moves meanwhile.
- COPYRIGHT.txt: notices for V-HACD and Boost, which ship inside the Blast authoring library in Windows and Linux x86_64 builds (#117).
- README: a note on the images: raw path tracer output, almost all CPU-rendered, without a denoiser or Ray Reconstruction (#122).
- README: what the fork does for open-world games and what comes next for them (#121).
- README: comparison captions explain what to look at, the emissive lights pair is a brightened close-up, and the many lights section shows its image (#120).

### Fixed
- Path tracing stays exact far from the world origin: it traces relative to the camera's cell on a 1024 m grid (`rendering/pathtracing/relative_origin_grid`, 0 = absolute), and ray origins clear surfaces by a few float steps of the largest coordinate, so shadows, reflections and contact points no longer break up kilometres out (#119).

## build-350c2073 (2026-11-08)

### Added
- Research note `docs/ideas/exposure.md`: auto exposure for dark scenes under path tracing (histogram metering, luminance-dependent key, exposure compensation curve, local exposure) and what the fork should take (#116).
- Project setting `rendering/pathtracing/emissive_lights` (default on) to turn off sampling emissive surfaces as lights (#115).
- PhysX 5 physics engine (Windows x64, Linux x86_64), selectable per project next to Jolt, with cloth, skinned cloth, Blast destruction, Flow smoke and fire, ocean water and particle fluids; cloth, destruction, smoke and water also work with Jolt (#113).
- README: on and off comparisons for emissive lights, light visibility memory and the radiance cache, with their test scenes in `docs/media/light-selection` (#118).

### Changed
- Changelog entries are files in `changelog.d/`, one per pull request, gathered by the release, so a release made while a pull request is open no longer misplaces its entry; lines under Unreleased in CHANGELOG.md still work. The release's changelog commit retries when main moves meanwhile.
- COPYRIGHT.txt: notices for V-HACD and Boost, which ship inside the Blast authoring library in Windows and Linux x86_64 builds (#117).

### Fixed
- Path tracing stays exact far from the world origin: it traces relative to the camera's cell on a 1024 m grid (`rendering/pathtracing/relative_origin_grid`, 0 = absolute), and ray origins clear surfaces by a few float steps of the largest coordinate, so shadows, reflections and contact points no longer break up kilometres out (#119).

## build-30bb7182 (2026-11-08)

### Added
- README: texture filtering comparison re-rendered on an RTX 4090 with the anisotropic filtering fix (#108).

### Changed
- Path tracer: the light visibility memory learns from about 65536 pixels a frame (at most one in four) instead of all of them. On an RTX 4090 at 1080p it cost 8 to 12 ms of a 17 ms frame, mostly contended compare-and-swap updates; quality at 480x270 is unchanged (26.05 vs 26.01 dB) (#114).

## build-5e22cf31 (2026-11-08)

### Changed
- README: DLSS Ray Reconstruction checked with the DLSS 4.5 DLLs (310.5) on an RTX 4090 (#112).

### Fixed
- Path tracer: color backgrounds (Background mode Custom Color or Clear Color) show behind the scene as in raster; camera rays that missed read the empty radiance map and came out black (#111).
- Release workflow: only commits on main are published as releases; a run on any other branch builds the zips as workflow artifacts only. build-3c75a9b9 was a test build of an unmerged branch published by mistake.

## build-3c75a9b9 (2026-11-08)

Test build of an unmerged branch, published by mistake and deleted. It was not a build of main.

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
