# Persistent ray tracing instances and partitioned TLAS

Research note, 8 Nov 2026. Step 3 of [open-world-geometry.md](open-world-geometry.md):
how to stop paying for every instance every frame. Written before any code, from the
4090 measurements and what other engines, NVIDIA, Microsoft and Valence do.

## What we measured

The open-world bench (`docs/bench/open_world`, 4090, 1080p, 1 spp, 2 bounces):

| Objects (instances) | Frame ms | GPU ms | AS build ms | Trace ms | Buffers |
|---|---|---|---|---|---|
| 10k (14k) | 5.8 | 1.7 | 0.3 | 1.9 | 70 MB |
| 30k (42k) | 10.1 | 2.5 | 0.6 | 1.8 | 94 MB |
| 100k (140k) | 31.1 | 7.0 | 2.2 | 4.2 | 152 MB |
| 100k, raster | 10.9 | 1.9 | | | 52 MB |
| 100k, cull 0.1 deg / 500 m | 16.4 | 3.9 | 0.8 | 2.8 | 99 MB |

The frame is CPU-bound: 31 ms of frame for 7 ms of GPU, against 11 ms in raster. The
20 ms difference is the path tracer's per-instance work on the render thread. A CPU
profile on the cloud renderer (42k instances) shows it spread over the TLAS builder's
per-instance chain rather than one hot spot: `_apply_surface_group`, `_plan_captures`,
`gather_instances`, `_prepare_tlas`, `_instance_kind`, `_reuse_mesh_plan`,
`_plan_mesh_record`, `_instance_masks`, `_push_tlas_entry`, `_push_tlas_instance`, then
the RenderingDevice instance validation and write. Every instance in range walks that
chain every frame, changed or not. `RenderingLightCuller::cull_directional_light` also
shows up: the directional shadow cull for raster still runs under the path tracer.

So the cost to remove is per-instance CPU work for instances that did not change. The
GPU TLAS build (2.2 ms at 140k) is the second target.

## What others do

- **Valence** (Godot path tracer, D94a to D94c, 2 Oct 2026, ffb98e75e3, 1d8854cf02,
  7708bc4d94):
  - Each RT surface keeps a persistent slot. A slot shadow holds what was last written
    (BLAS, transform relative to the origin, flags, mask, SBT offset, geometry and
    material row generations), and the slot is written only when that differs.
  - The walk skips instances it proves unchanged. Later (P2a, 24cdc67af2) it skips even
    the transform compare when the instance's transform was not set since the last
    build. Per-instance rows (geometry, material, motion) live in slot-indexed buffers,
    and a compute pass scatters the changed ones.
  - Instances that left the list are swept and disabled in the same build.
  - Partitions are 128 m columns of up to 8,192 enabled slots. A slot rewritten while
    enabled moves to the global partition and returns after 16 unwritten builds.
  - Partition translation (D94c) stores slots relative to their cell, so an
    origin shift writes one translation per partition and no slots.
  - Result: on 302,801 foliage instances, CPU p50 went from 23.87 to 0.15 ms and GPU p50
    from 10.64 to 9.69 ms.
  - Costs: about 2,700 lines, and only with `VK_NV_partitioned_acceleration_structure`.
    Without it Valence keeps the plain per-frame path.
  - Follow-ups measured walk costs that came back at crowd scale: a quadratic
    slot-range scan (4d3436e8eb) and cache misses on first touches, fixed with
    prefetch (208278701d).
- **NVIDIA vk_partitioned_tlas**:
  - 1.2 M static objects on a uniform 2D grid of partitions, with 170,000 toppling
    dominoes in a global partition.
  - Instance writes are made on the GPU into an indirect op buffer, with an atomic
    count. Only partitions with changed instances rebuild.
- **Microsoft DXR** (preview, 28 Oct 2026):
  - Partitioned TLAS and clustered geometry are now in D3D12. They are previewed on all
    NVIDIA RTX GPUs, AMD RX 9000, Intel Arc B-series and Core Ultra 200V.
  - The ops are the same three as NVIDIA's: write instance, update instance (swap the
    BLAS, for LOD) and translate partition. They are GPU-driven through
    `ExecuteIndirectRTASOperations`.
  - Their domino sample: full TLAS rebuild 2.88 ms against 0.79 ms partitioned, with
    slightly slower trace.
  - No Vulkan EXT or KHR is announced yet. A cross-vendor Vulkan version is likely to
    follow, so the design should not tie itself to NV-only data layouts.
- **Unreal Engine 5**:
  - Rebuilds the TLAS every frame. Its render-thread gather (GatherRayTracingWorldInstances)
    scales with instance count, and Epic's budget is about 100,000 instances after culling
    for 30 fps on consoles.
  - Culls by distance and projected angle (`r.RayTracing.Culling` 3, Radius 100 m by
    default, Angle 1 degree).
  - Compacts the active instance list on the GPU
    (`r.RayTracing.Scene.CompactInstances`).
  - A Far Field of HLOD meshes covers what culling drops.
- **Moana on PTLAS** (Maven Kim, Nov 2025):
  - 36 M instances in SAH-built partitions of under 1,024 instances each.
  - Partitions whose screen size falls under a threshold are disabled on the GPU, and
    their slots go back to a free list.
  - Pitfalls: disabled instances still count toward a partition's capacity, and a
    duplicate instance index in one build is an error.

## What to build

Two stages, each behind a setting with today's path as the default:

**Stage A: persistent instance slots, any RT GPU** (`rendering/pathtracing/persistent_instances`)
- Each traced surface of a geometry instance gets a stable slot in a per-viewport slot
  table. The slot is freed when the instance is freed or the viewport goes idle.
- A per-instance RT generation is bumped by everything that changes what the trace
  sees: transform, material, mask and visibility layers, mesh or BLAS rebuild, LOD
  level, shadow group.
  - A slot whose generation and BLAS match its shadow is skipped before any planning.
  - This is Valence's slot shadow keyed by generation instead of by comparing
    fields, so the common case costs one load and one compare.
- Instances not in this frame's list are disabled (mask 0) by a sweep over the slots
  not stamped this frame. Instances in range keep their slot while out of view.
- Changed slots are written into a persistent instance buffer and per-slot row buffers
  by a scatter pass, like Valence's `ptlas_rows`. The plain TLAS is then built from the
  whole persistent buffer.
  - On any GPU this removes the CPU walk for unchanged instances. The GPU build stays a
    full build, 2.2 ms at 140k on the 4090.
  - The slot range is compacted when more than a quarter of it is disabled.
- A relative-origin shift (1024 m cells) rewrites every slot once. This is rare and costs
  the same as today's frame.
- Things that need care in this codebase:
  - Shadow-caster groups are assigned in instance order each frame. They must become
    stable per instance, or a group change must bump the generation.
  - Expanded MultiMesh ranges and particles stay on today's per-frame path at first.
  - Vertex captures and deformed BLASes keep their own dirty tracking.
  - Each viewport, including editor viewports, has its own slots.

**Stage B: partitioned TLAS where available** (`rendering/pathtracing/partitioned_tlas`)
- Use `VK_NV_partitioned_acceleration_structure`, which the 4090 has at rev 1. Keep the
  slot model from stage A, so the extension only changes how changed slots reach the
  structure (write and disable ops) and how much the GPU rebuilds.
- Partitions are square cells, with a global partition for slots rewritten while
  enabled. They return after N stable builds.
  - Start from Valence's tested values: 128 m cells, 8,192 slots, N = 16.
  - Measure 256 m too, since our relative-origin grid is 1024 m.
- Partition translation is set from each cell's origin minus `rt_origin`, so an origin
  shift writes one translation per partition. The global partition can't be translated,
  so its records carry their cell's offset.
- When the extension is missing, keep stage A's plain TLAS with one warning. When a
  cross-vendor Vulkan version appears, map the same ops onto it.

**Also, separately:** skip the raster directional shadow cull when the viewport is path
traced and nothing rasterizes shadows (done, see below).

## Stage A as built

- `RenderGeometryInstanceBase::rt_generation` is renewed by every setter the trace reads and by surface rebuilds.
- `RTPersistentInstances` (render_raytracing_persistent.h) keeps, per viewport and by `rt_index`, the instance's
  TLAS entries (already converted for RenderingDevice), geometry rows, motion indices and emissive candidates.
  Per-frame instances (MultiMesh, particles, procedural, deformed, captured, billboards, mesh LOD) follow them.
- A changed material rewrites only the slots whose rows use it, found by scanning the material cache when the
  global material version moves. Mesh, readiness, pipeline, camera layers, LOD, light caster masks and origin
  changes rewrite every slot once.
- Shadow-caster groups persist while the lights' caster masks stay the same set; the self-shadow group is always
  reserved.
- Disabled entries get mask 0, and RenderingDevice now leaves mask-0 instances out of every TLAS build. Lavapipe
  faults on mask-0 and null-BLAS instances, both legal in Vulkan.
- Still per frame: the plan arena reserve and one load per listed instance, and RenderingDevice's validation and
  write of all TLAS instances. The second is the next CPU target.
- Cloud (llvmpipe CPU), 30k objects (42k instances): `build_tlas` CPU 12.7 ms off, 2.7 ms on. Images match the
  per-frame path except for noise from the TLAS order.
- 4090, 100k objects, frame ms off and on (median; images match within noise):

  | Run | Off | On | GPU off | GPU on | AS build off | AS build on |
  |---|---|---|---|---|---|---|
  | Still | 25.3 | 6.7 | 7.1 | 2.5 | 2.1 | 0.86 |
  | Cull 0.1 deg / 500 m | 15.6 | 6.5 | 4.3 | 2.1 | 0.92 | 0.43 |
  | 500 movers | 25.9 | 7.4 | 6.8 | 2.6 | 2.1 | 0.87 |
  | Fly at 60 m/s | 29.7 | 7.4 | 5.2 | 2.6 | 1.2 | 0.90 |

  The GPU passes get faster too, most likely because the GPU no longer idles between CPU-bound frames. The frame
  is still CPU-bound (6.7 ms for 2.5 ms of GPU). Raster runs the same scene in 10.9 ms.
- The raster directional shadow caster cull is skipped while path tracing, as positional shadows already were:
  the path tracer never draws the shadow maps.

## Measuring

Each stage uses the same bench runs (W3 to W6: 100k objects, with and without cull)
plus a moving-objects case (`--movers=N`, to add) and an origin-crossing flight.
The numbers to watch:
- Render-thread CPU per frame.
- AS build GPU ms.
- Trace GPU ms. Partitions can slow tracing.
- Slot writes per frame.

Images must match the current path to within noise. Valence held bit identity on its
primary guides.

Expected, from Valence's numbers:
- Stage A: the CPU part of the 31 ms frame at 100k objects falls toward raster's 11 ms.
- Stage B: the 2.2 ms GPU build falls to well under 1 ms in a static scene.

## Sources

- Valence commits ffb98e75e3, 1d8854cf02, 7708bc4d94, 2a89cc5abf, f5c1c66595,
  24cdc67af2, 4d3436e8eb, 208278701d (github.com/Rman887/valence-engine-rtx).
- `VK_NV_partitioned_acceleration_structure` proposal, docs.vulkan.org.
- NVIDIA nvpro-samples/vk_partitioned_tlas README.
- Microsoft DirectX blog, *Clustered geometry and partitioned TLAS preview*,
  28 Oct 2026.
- NVIDIA, *UE 5.4 Raytracing Guideline* (2024); Epic, *Ray Tracing Performance Guide in
  Unreal Engine*.
- Maven Kim, *Interactive Moana, part 2*, 9 Nov 2025.
