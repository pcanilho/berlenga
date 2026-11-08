# Open-world geometry: LOD, Mega Geometry, partitioned TLAS, streaming

Research note, 8 Nov 2026. What the path tracer needs to trace a large world, what NVIDIA's
samples and the Valence fork do, and the order to build it in. Step 0 (camera-relative
tracing, #119) is done.

## Where the fork stands

- Every mesh surface gets one BLAS at full detail. Godot's own mesh LODs (generated on import by
  default) are used by raster only.
- The TLAS holds every instance in a box of half-size `z_far` around the camera
  (`renderer_scene_cull.cpp`, `rt_aabb`), rebuilt from scratch each frame. No distance or size
  culling beyond that.
- Dynamic BLAS (skinned, particles, camera-facing MultiMesh) have per-frame build budgets.
- RenderingDevice has no cluster or partitioned acceleration structures.

So in a large world, BLAS memory grows with everything within `z_far` at full detail, and TLAS
build time grows with the instance count. Both limit view distance long before shading does.

## What others do

- Alan Wake 2 (Mega Geometry, patch 1.2.8) moved existing meshes to cluster acceleration
  structures: about 1 GB less VRAM and 13% faster at 4K on an RTX 4090 (other-engines.md).
- Assassin's Creed Shadows traces coarse RT LODs, streams instances around the camera, rebuilds
  the TLAS asynchronously and culls by solid angle (other-engines.md).
- NVIDIA vk_animated_clusters (Apache-2.0), RTX 6000 Ada, 8.43 M animated triangles: triangle
  BLAS with 10% rebuilds cost 5.22 ms build + 1.39 ms trace and 564 MB; clusters from templates
  0.80 + 1.52 ms and 271 MB. Needs driver 572.16 or newer.
- NVIDIA vk_lod_clusters (Apache-2.0): meshoptimizer `clusterlod` builds a DAG of cluster
  groups with quadric error; a compute traversal over a per-mesh sphere tree picks clusters
  whose error stays under one pixel; instances whose LOD range overlaps share one BLAS build
  (LEGO scene: 3.9 to 1.6 ms a frame, BLAS 62 to 9 MB); geometry streams by group with the
  coarsest level always resident; CLAS memory comes from a sparse allocator grown in 128 MiB
  chunks.
- NVIDIA vk_partitioned_tlas: the TLAS is split into app-defined partitions (a 2D grid there)
  and only partitions whose instances changed are rebuilt; a global partition takes fast-moving
  instances at some trace cost; optional per-partition translation. Demo: 170,000 moving
  dominoes on 1.2 M static objects.
- Valence (Godot path tracer on the same NVIDIA base) built all of it:
  - Cluster AS and cluster LOD (D79 to D92): about 8,100 lines over 37 files, including the
    RenderingDevice and Vulkan driver support, a clusterizer on mesh creation saved with the
    mesh, GPU LOD selection, BLAS sharing (400 rocks: 6 builds a frame instead of 400, BLAS
    arena 10.6 to 0.8 MB, GPU time flat), cached levels under a memory budget.
  - Partitioned TLAS (D94): about 2,700 lines. Slot-resident instances written only when they
    change, 128 m column partitions, a global partition for moving ones, falls back to a plain
    TLAS with one warning.
  - Streaming (D93): about 2,500 lines. Per-cluster blobs, residency, gathers on worker
    threads (render-thread load time 25 to 2-3 ms on their test world), a RAM fallback level.
  - Async builds on a compute queue with timeline semaphores (D97, D98).

Hardware: both NV extensions are present on our RTX 4090 (cluster AS rev 4, partitioned rev 1,
checked 6 Nov). Neither exists on AMD, Intel or macOS, so every step needs the current path as
the fallback.

## Order

Each step is a project setting, off means today's behaviour, and each gets a README entry and
a shot or numbers from a generic test scene.

1. **RT LOD from Godot's mesh LODs** (any RT GPU, small). Pick a LOD per instance by projected
   size, as raster does, and build one BLAS per (surface, LOD), shared by every instance at that
   LOD. Uses LODs projects already have, so it also works without the NV extensions and stays as
   the fallback for steps 3 and 4. Plus an RT draw distance and a minimum projected size, so
   tiny far objects leave the TLAS (as AC Shadows). Risk: popping in reflections when an
   instance changes LOD; hysteresis as raster's LOD bias.
2. **Measure on the 4090** with a generic open-world test scene (rocks, trees and blocks to
   2 km, 10,000 to 100,000 instances): BLAS memory, TLAS build ms, trace ms, at each step.
   This decides whether partitioned TLAS comes before clusters.
3. **Partitioned TLAS** (`VK_NV_partitioned_acceleration_structure`). Only changed partitions
   rebuild. Worth it once TLAS build time shows up in step 2's numbers; with relative origin
   the partition translation keeps far partitions precise.
4. **Cluster acceleration structures** (`VK_NV_cluster_acceleration_structure`), static meshes
   first: clusterize on import (meshoptimizer, 128 triangles), build CLAS and BLAS from them.
   The Alan Wake 2 memory win. Then cluster templates for skinned meshes (the animated-clusters
   numbers above), which matters for crowds and for PhysX cloth.
5. **Cluster LOD and BLAS sharing**: meshoptimizer `clusterlod`, GPU selection by pixel error,
   shared builds. This is the part that lets distant geometry stay in the trace at low cost.
6. **Streaming and async builds**: residency by cluster group, worker-thread gathers, a compute
   queue for builds. Ties into the game's world streaming (cells loaded and unloaded by the
   scene, not by the renderer).

## Open questions

- Whether RenderingDevice should expose cluster builds publicly or keep them internal to the
  path tracer (Valence added public RD methods; internal is less to maintain against upstream).
- Import cost: clusterizing and building the LOD DAG at import adds time and file size; Valence
  saves the tables with the mesh.
- Opacity micromaps (vegetation alpha test) interact with clusters; Valence supports both.
- Partition layout for a world with streaming cells: one partition per cell is the natural fit.

## Sources

- nvpro-samples/vk_lod_clusters README and docs (lod_generation, streaming, blas_sharing,
  clas_allocation), vk_animated_clusters README, vk_partitioned_tlas README.
- Rman887/valence-engine-rtx commit messages D79 to D98.
- docs/ideas/other-engines.md (Alan Wake 2, AC Shadows), valence-engine-rtx-takeaways.md.
