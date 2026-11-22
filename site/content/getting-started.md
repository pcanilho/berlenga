---
title: "Getting started"
weight: 10
description: "Download Berlenga, turn on path tracing and find the settings."
---

## Download

[Latest release](https://github.com/pcanilho/berlenga/releases/latest), named by commit:

- **Windows**: for Ray Reconstruction, add NVIDIA's DLSS, Streamline and Reflex DLLs next to the exe (not shipped).
- **Linux** x86_64 and arm64: no Ray Reconstruction.
- **macOS**: raster only.

Projects open as in Godot 4. Without path tracing, Berlenga renders like Godot.

## Turn on path tracing

Path tracing needs a Vulkan GPU with hardware ray tracing. It is set per `Environment`:

- `pathtracing_enabled`: on.
- `pathtracing_samples_per_pixel` (1) and `pathtracing_max_bounces` (3).
- `pathtracing_denoiser`: Ray Reconstruction runs inside DLSS, so set the viewport's `scaling_3d_mode` to DLSS (`scaling_3d_scale` 1.0 gives DLAA). With any other scaling mode the path tracer runs without a denoiser.
- `pathtracing_sky_shader_enabled`: camera rays see the sky shader exactly; needed for path-traced clouds.

```gdscript
var env := $WorldEnvironment.environment
env.pathtracing_enabled = true
env.pathtracing_samples_per_pixel = 1
get_viewport().scaling_3d_mode = Viewport.SCALING_3D_MODE_DLSS
get_viewport().scaling_3d_scale = 1.0
```

`Viewport.use_pathtracing = false` keeps a small render-to-texture viewport rasterized.

## Settings

Path tracer settings live under **Project Settings > Rendering > Pathtracing** (`rendering/pathtracing/...`). Each feature page names its setting or node property. [Open worlds]({{< relref "features/open-worlds" >}}) lists settings for large scenes. Every setting with its default: [Settings]({{< relref "settings" >}}).

## Physics

Choose `PhysX` or `Jolt Physics` in **Project Settings > Physics > 3D > Physics Engine**. See [Physics]({{< relref "features/physics" >}}).

## Test scenes

Each area has a bench project under [`docs/bench`](https://github.com/pcanilho/berlenga/tree/main/docs/bench):

```sh
godot --path docs/bench/landscape
godot --path docs/bench/open_world --gpu-profile -- --count=100000
```

## Build from source

As Godot, for example `scons platform=linuxbsd target=editor`. Builds without the PhysX SDKs skip that module ([modules/godot_physx/README.md](https://github.com/pcanilho/berlenga/blob/main/modules/godot_physx/README.md)). Release flags: [release.yml](https://github.com/pcanilho/berlenga/blob/main/.github/workflows/release.yml).
