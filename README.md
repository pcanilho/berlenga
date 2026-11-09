# <img src="misc/dist/linux/berlenga.svg" width="32" height="32" alt=""> Berlenga

[![Release](https://github.com/pcanilho/berlenga/actions/workflows/release.yml/badge.svg)](https://github.com/pcanilho/berlenga/actions/workflows/release.yml) [![CodeQL](https://github.com/pcanilho/berlenga/actions/workflows/codeql.yml/badge.svg)](https://github.com/pcanilho/berlenga/actions/workflows/codeql.yml) [![Latest release](https://img.shields.io/github/v/release/pcanilho/berlenga?label=latest)](https://github.com/pcanilho/berlenga/releases/latest)

Berlenga is a Godot 4 fork with Vulkan path tracing and DLSS Ray Reconstruction, built for large open worlds. Based on [NVIDIA-RTX/godot](https://github.com/NVIDIA-RTX/godot) (2 November 2026).

**[Download](https://github.com/pcanilho/berlenga/releases/latest):** Windows (Ray Reconstruction: add NVIDIA's DLSS, Streamline and Reflex DLLs next to the exe), Linux x86_64/arm64 (no Ray Reconstruction), macOS (raster only). **Changes:** [CHANGELOG.md](CHANGELOG.md).

![1024 street lamps: NVIDIA-RTX/godot on the left keeps 64 of them, Berlenga on the right lights them all](docs/media/many-lights.jpg)

*1024 lights, 16 samples per pixel. Left: NVIDIA-RTX/godot. Right: Berlenga. Images here are raw path tracer output, mostly CPU-rendered, with no denoiser or Ray Reconstruction.*

## At a glance

- **Many lights**: 4096 path-traced lights (upstream 64), emissive surfaces count as lights.
- **Faster convergence**: light visibility memory and a radiance cache.
- **Materials**: clearcoat, back lighting and thin-glass transmission in the path tracer; textures filter like raster.
- **Water**: refraction, depth tint, murky water, caustics and a path-traced ocean.
- **Exposure**: histogram metering and local exposure that keep night scenes dark.
- **Physics**: PhysX 5 next to Jolt, with cloth, tearing, destruction and path-traced smoke.
- **Open worlds**: far-origin precision, persistent ray tracing instances, mesh LOD in the tracer.

## What Berlenga adds

Each feature that changes the look or the cost can be turned off with the setting shown.

### Lighting
- **4096 lights** (upstream 64): camera-following light grid with resampled importance sampling.
- **Emissive surfaces as lights** (`rendering/pathtracing/emissive_lights`).
- **Light visibility memory** (`rendering/pathtracing/light_visibility_memory`): hidden lights stop taking samples.
- **Radiance cache** (`rendering/pathtracing/radiance_cache`): bounces end on cached light.
- Shadow-only casters bounce light.
- Lit raster transparents and rain (`rendering/pathtracing/max_transparency_layers` 0), still traced in reflections.

### Materials
- **Back lighting** (`rendering/pathtracing/back_lighting`): light passes through leaves, paper and curtains, back light texture included, from lights and bounces, into coloured shadows.
- **Transmission** (new BaseMaterial3D section, `rendering/pathtracing/transmission`): thin glass passes light straight through, tinted and blurred by roughness, with tinted shadows; transmission texture; `TRANSMISSION` in shaders; glTF `KHR_materials_transmission`.
- **Clearcoat** (`rendering/pathtracing/clearcoat`): coated paint as raster shades it, coat texture included, with traced coat reflections.
- Shaders writing `BACKLIGHT` or `TRANSMISSION` pass light into their shadows.
- Textures filter like raster (ray cones, anisotropic).
- Even, tinted glass shadows.
- Back-face normals in `vertex()`; `IN_SHADOW_PASS` in custom shaders.

<details><summary>Images: transmission, back lighting, clearcoat, layer textures</summary>

![Green glass panes before coloured spheres: raster, path traced with transmission on (the spheres show through and the panes cast green-tinted light), and off (grey opaque panes, dark shadows)](docs/media/transmission.jpg)

![Back-lit leaf cards and a red curtain: green and red light on the floor with back lighting on, plain shadows with it off](docs/media/back-lighting.jpg)

![Red coated spheres: raster, path traced with clearcoat on (sharp coat highlights match raster), and off (dull)](docs/media/clearcoat.jpg)

![Checkered back light and transmission textures: raster casts plain shadows, the path tracer casts checkered coloured ones](docs/media/layer-textures.jpg)

Look at the floor under the glass and cards, the highlights on the spheres, and the checkered shadows.

</details>

### Water
![A pool with a checkered floor and coloured posts: with refraction off the water is clear and the posts run straight; with it on the posts shift where they enter the water and the floor turns blue and darker with depth](docs/media/water-refraction.jpg)

*Refraction off, then on: the posts shift where they enter the water and the floor darkens and tints with depth.*

- **Refraction** (`rendering/pathtracing/refraction`): water and glass blocks (Transmission, thin-walled off) bend light by their IOR, reflect it totally at grazing angles, and darken and tint with depth; glTF `KHR_materials_volume` and `KHR_materials_ior`.
- **Murky water** (same setting): Transmission media scatter light (strength, colour, anisotropy; glTF `KHR_materials_scatter` draft); shaders can write `IOR` and `ABSORPTION`.

<details><summary>Image: clear vs murky water</summary>

![The same pool path traced with clear water and with murky water: in the murky one a green glow fills the deep end and the posts fade below the surface](docs/media/murky-water.jpg)

Look at the green glow filling the deep end.

</details>

- **Caustics** (`rendering/pathtracing/caustics`): sunlight gathers into moving lines on the floor under rippled water.

<details><summary>Image: caustics off vs on</summary>

![A rippled pool path traced: with caustics off the floor is evenly lit; with them on bright wavy lines of sunlight cross the floor and posts](docs/media/caustics.jpg)

Look at the bright lines on the pool floor.

</details>

- **Path-traced ocean**: `PhysXWaterSurface3D` is real water under path tracing, bending light through the waves, tinting with depth and catching sun caustics on the seabed (material `pathtraced_absorption`, `pathtraced_roughness`; caustics via `rendering/pathtracing/caustics`).

<details><summary>Image: ocean shallows, raster vs path traced</summary>

![Looking down into shallow waves with floating crates: raster shows a flat grey-blue surface; path traced shows the sand through the water with caustic light on it](docs/media/ocean.jpg)

Look at the sand showing through the water and the caustic light on it.

</details>

### Exposure
- **Histogram metering** (`rendering/camera/auto_exposure/metering`): lamps in view no longer darken the scene.
- **Dark-scene key** (`dark_scene_luminance`): night stays night.
- **Local exposure** (`local_highlight_contrast`, `local_shadow_contrast`).

### DLSS Ray Reconstruction
- DLSS 4.5 (`rendering/streamline/dlss_ray_reconstruction_preset`).
- Continuous thin rain and correct blending over its output; frame generation fixes.

### Physics (PhysX 5)
![PhysX: cloth flag in wind, a Blast wall broken by a ball, rigid crates, Flow smoke](docs/media/physx.gif)

Windows x64 and Linux x86_64. Choose `PhysX` or `Jolt Physics` in **Project Settings > Physics > 3D > Physics Engine**.
- Either engine: tearable cloth, skinned cloth, Blast destruction, Flow smoke and fire, ocean buoyancy, compute-shader fluids.
- PhysX only: vehicles, CUDA fluids and deformables.
- **Path-traced smoke and fire** (`rendering/pathtracing/simulated_gas`).
- **Cloth tearing** (`PhysXCloth3D.tear_factor`, `physics/physx_3d/cloth/tearing`): overstretched edges split and the rip runs.

  <details><summary>Clip: a ribbon rips off its post and a flag frays</summary>

  ![Cloth tearing: a ribbon rips off its post and a flag frays in rising wind](docs/media/cloth-tearing.gif)

  </details>

- Builds without the SDKs skip the module (`modules/godot_physx/README.md`).

### Other
- Cached shadows, time-sliced reflection probes; `is_pathtracing_ready()`, video memory queries; leak, crash and device-fault fixes.

## Comparisons

Scenes: [docs/media/showcase](docs/media/showcase) and [docs/media/light-selection](docs/media/light-selection).

<details>
<summary>Glass shadows: NVIDIA-RTX/godot vs Berlenga</summary>

![Glass pane shadow: speckled on NVIDIA-RTX/godot, even and tinted on Berlenga](docs/media/glass-shadow.jpg)
</details>

<details>
<summary>Texture filtering: NVIDIA-RTX/godot, Berlenga, raster</summary>

![Checker floor: moire on NVIDIA-RTX/godot, filtered on Berlenga, raster for reference](docs/media/texture-filtering.jpg)
</details>

<details>
<summary>Emissive lights off vs on</summary>

![A pavement lit by a glowing window: dark and speckled with the setting off, evenly lit with it on](docs/media/emissive-lights.jpg)

Look at the pavement under the window.
</details>

<details>
<summary>Light visibility memory off vs on</summary>

![A street among buildings with lamps hidden inside them: noisy with the setting off, much cleaner with it on](docs/media/light-visibility-memory.jpg)
</details>

<details>
<summary>Radiance cache off vs on, equal time</summary>

![The same street at 4 bounces and equal render time: 4 samples per pixel with the cache off, 6 with it on](docs/media/radiance-cache.jpg)
</details>

<details>
<summary>PhysX scene path traced</summary>

![The PhysX scene path traced at 64 samples per pixel](docs/media/physx-pathtraced.webp)
</details>

<details>
<summary>65 km from the origin: absolute vs camera-relative</summary>

![A cube, a mirror ball and a lamp 65 km from the origin: speckled cube and a broken reflection with absolute positions, correct with camera-relative positions](docs/media/far-origin.jpg)
</details>

<details>
<summary>Exposure: average, histogram, dark-scene key</summary>

![A night street with lamp posts: dark with average metering, readable with histogram metering, darker again with the dark-scene key](docs/media/night-exposure.jpg)
</details>

<details>
<summary>Local exposure off vs on</summary>

![The night street: hot spots on lit walls and black facades without local exposure, detail kept with it](docs/media/local-exposure.jpg)
</details>

## Open worlds

Berlenga aims at large streamed worlds: tens of thousands of objects over kilometres, with most of them unchanged from one frame to the next.

- **Camera-relative tracing** (`rendering/pathtracing/relative_origin_grid`): exact shadows and reflections far from the origin.
- **Persistent instances** (`rendering/pathtracing/persistent_instances`): objects and large MultiMeshes that did not change keep their ray tracing instances on the GPU and cost no per-frame work.
- **Off-screen culling** (`rendering/pathtracing/cull_max_distance`, `cull_min_size`): small or distant objects the camera can't see leave the trace.
- **Opacity micromaps** (`rendering/pathtracing/opacity_micromaps`): cut-out foliage traced without any-hit shaders.
- **Mesh LOD in the path tracer** (`rendering/pathtracing/mesh_lod`).

Settings for a large world, in `project.godot`:

```ini
[rendering]
pathtracing/relative_origin_grid=1024.0
pathtracing/persistent_instances=true
pathtracing/cull_max_distance=500.0
pathtracing/cull_min_size=0.1
pathtracing/opacity_micromaps=true
```

Test scene: rocks, trees and blocks scattered over 2 km, with timing and memory printed at the end.

```sh
godot --path docs/bench/open_world --gpu-profile -- --count=100000 --set=rendering/pathtracing/persistent_instances:1
```

Options: `--movers=N`, `--churn=N`, `--recolor`, `--fly=S`, `--multimesh=M` (MultiMesh tiles of M meters), `--mmmove=N`, `--raster`, `--out=frame.png`. Design and measurements: [open-world geometry](docs/ideas/open-world-geometry.md), [persistent instances](docs/ideas/persistent-instances.md), [instanced forests](docs/ideas/instanced-forests.md), [opacity micromaps](docs/ideas/opacity-micromaps.md).

## References

Code: [NVIDIA-RTX/godot](https://github.com/NVIDIA-RTX/godot) (path tracer, DLSS); [uno1982/godot](https://github.com/uno1982/godot) `feature/physx5-module` (Wild Ox Studios, MIT); [NVIDIA-Omniverse/PhysX](https://github.com/NVIDIA-Omniverse/PhysX) (BSD-3-Clause); [valence-engine-rtx](https://github.com/Rman887/valence-engine-rtx) (camera-relative tracing, ray offset, persistent instance slots, opacity micromap bake). Papers: [docs/REFERENCES.md](docs/REFERENCES.md). Research notes: [docs/ideas](docs/ideas).

## License

MIT, like Godot. Third-party licenses in `thirdparty/` and `COPYRIGHT.txt`; NVIDIA binaries under NVIDIA's terms. Build and docs: [godotengine/godot](https://github.com/godotengine/godot), [docs.godotengine.org](https://docs.godotengine.org).

Berlenga is an independent fork and is not affiliated with or endorsed by the Godot Foundation.
