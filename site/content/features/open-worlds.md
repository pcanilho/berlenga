---
title: "Open worlds"
weight: 100
group: "Engine"
summary: "Camera-relative tracing, persistent instances, culling, mesh LOD."
---

Target: tens of thousands of objects over kilometres, mostly static.

- **Camera-relative tracing** (`rendering/pathtracing/relative_origin_grid`): the trace origin follows the camera on a grid, so shadows, reflections and thin refractive detail near the camera stay exact far from the world origin.
- **Persistent instances** (`rendering/pathtracing/persistent_instances`): unchanged objects and MultiMeshes cost no per-frame work.
- **Off-screen culling** (`rendering/pathtracing/cull_max_distance`, `cull_min_size`): small or distant unseen objects leave the trace.
- **Opacity micromaps** (`rendering/pathtracing/opacity_micromaps`): cut-out foliage traced without any-hit shaders.
- **Mesh LOD in the path tracer** (`rendering/pathtracing/mesh_lod`).
- **Compacted ray tracing structures** (`rendering/rendering_device/acceleration_structure/compaction`): static acceleration structures shrink in video memory after they are built.

Settings for a large world, in `project.godot`:

```ini
[rendering]
pathtracing/persistent_instances=true
pathtracing/cull_max_distance=500.0
pathtracing/cull_min_size=0.1
pathtracing/opacity_micromaps=true
```

Test scene (2 km, prints timing and memory):

```sh
godot --path docs/bench/open_world --gpu-profile -- --count=100000 --set=rendering/pathtracing/persistent_instances:1
```

Options: `--movers=N`, `--churn=N`, `--recolor`, `--fly=S`, `--multimesh=M` (MultiMesh tiles of M meters), `--mmmove=N`, `--raster`, `--out=frame.png`. Design and measurements: [open-world geometry](https://github.com/pcanilho/berlenga/blob/main/docs/ideas/open-world-geometry.md), [persistent instances](https://github.com/pcanilho/berlenga/blob/main/docs/ideas/persistent-instances.md), [instanced forests](https://github.com/pcanilho/berlenga/blob/main/docs/ideas/instanced-forests.md), [opacity micromaps](https://github.com/pcanilho/berlenga/blob/main/docs/ideas/opacity-micromaps.md).
