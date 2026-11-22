# Optimisation without visual loss

Goal: make the fork cheaper in CPU time, memory and GPU time across every system, while every picture
stays identical or better. This note names the established techniques first, then lists what fits the
fork, how each change is proven, and what has to wait for a GPU.

## Rules every change follows

- **Same picture.** Each change is checked with a raster and a path-traced frame on the same camera,
  seed and settings, before and after, waiting for `RENDER_INFO_RT_PENDING_SURFACES == 0`. The pass bar
  is a byte-identical frame where the change is exact by construction (memory layout, compaction,
  scheduling), and a per-pixel difference under the run-to-run noise floor where the old path was
  already non-repeatable (Flow, PT accumulation). PT pairs are also compared at 100% crop for fine
  detail. Flow is not bit-repeatable, so its pairs come from one run.
- **No regressions.** Every existing `--test` bench passes before and after (trees, stands, links,
  ground, oasis, rocks, snow, vegetation, grass, terrain, weather, sky, rain, mist, plants, bark,
  gpu_multimesh, terrain_data, terrain_deform, terrain_gen).
- **Features stay on.** Nothing is switched off to save time. Tests may switch off what they do not
  need; fast iteration paths stay.
- **Measured, not assumed.** Each change reports a before/after number from a tool, not a guess.

## How the cloud can measure

The cloud has no GPU (lavapipe), so GPU time there is meaningless. It is good for:

| Quantity | Tool |
|---|---|
| CPU hot spots per frame | `perf record -g` (sampling profiler; works in the container) on a bench run |
| Allocation churn and peak heap | `heaptrack` (installed) |
| GPU memory by kind | RenderingDevice counters (`get_memory_usage`, buffer/texture totals) and `RENDER_INFO_*` |
| Driver/compile memory | `PIPELINE_MEM_LOG=1` (prints each pipeline compile with ms and RSS) |
| Draw calls, instances, pipelines | `RENDER_INFO_TOTAL_DRAW_CALLS_IN_FRAME`, pipeline cache counts |
| Load/generation time | wall-clock around each generator in the benches |

GPU timings go on the PC bench list at the end.

## Established techniques

### Acceleration structure memory (PT)

- **BLAS compaction.** After a BLAS is built with `ALLOW_COMPACTION`, query its compacted size and copy
  it into a right-sized buffer (`vkCmdCopyAccelerationStructureKHR`, mode `COMPACT`). The copy holds the
  same tree, so traversal and the image are unchanged. NVIDIA reports compaction typically saving
  around half of BLAS memory, and recommends it for every static BLAS
  ([NVIDIA, Tips: Acceleration Structure Compaction](https://developer.nvidia.com/blog/tips-acceleration-structure-compaction);
  [RTX Best Practices](https://developer.nvidia.com/blog/best-practices-for-using-nvidia-rtx-ray-tracing-updated/);
  [RTXMU](https://developer.nvidia.com/blog/reducing-acceleration-structure-memory-with-nvidia-rtxmu/);
  [nvpro Vulkan RT tutorial](https://nvpro-samples.github.io/vk_raytracing_tutorial_KHR/acceleration_structures.html)).
  Refitted (deforming) BLASes are left as they are: compaction rules out updates.
  **Fork today:** the flag exists in the driver enum but nothing uses it. Every static BLAS keeps its
  full build-size allocation.
- **Shared scratch.** One pooled scratch buffer for batched builds instead of one per structure.
  **Fork today:** done (`acceleration_structure_build_batch`, `max_scratch_buffer_size_mb`). Only the
  one decal box BLAS still uses the single-build path with its own scratch, which is negligible.
- **Persistent instances / partitioned TLAS.** Done as stage A; see [persistent-instances.md](persistent-instances.md)
  and [open-world-geometry.md](open-world-geometry.md).

### CPU frame time

- **Data-oriented culling and update loops.** Flat arrays, hot/cold split, SIMD, no virtual calls per
  element ([Collin, "Culling the Battlefield", GDC 2011](https://ea.com/frostbite/news/culling-the-battlefield-data-oriented-design-in-practice);
  [Decima visibility, Horizon Zero Dawn](https://www.guerrilla-games.com/media/News/Files/GCAP2017_DecimaVisibility.pdf)).
- **Job systems over long serial passes.** Split per-frame work into small jobs on the worker pool
  (Gyrling, "Parallelizing the Naughty Dog Engine Using Fibers", GDC 2015;
  [Efficient rendering in The Division 2, GDC 2019](https://gpuopen.com/gdc-presentations/2019/gdc-2019-agtd1-efficient-rendering-in-the-division-2.pdf)).
  The fork already uses `WorkerThreadPool` in about 26 module files and plans PT instances in parallel.
- **No per-frame heap allocation.** Frame (linear) allocators and reused scratch arrays; heap churn
  costs lock contention and cache misses (same Naughty Dog talk; Frostbite and Activision engine
  architecture talks, [REAC 2021](https://enginearchitecture.org/downloads/reac2021_rendering_engine_architecture_at_activision.pdf)).
  In Godot this mostly means `Vector` copy-on-write copies, `String` building and `Array`/`Dictionary`
  in hot loops.
- **Dirty flags and time-slicing.** Update only what changed; spread slow updates (far vegetation,
  maps) over frames with a budget. Already used in the PT scheduler (`RT_SWEEP_BUDGET`).

### GPU memory and bandwidth

- **Transient resource aliasing via a frame graph.** Render targets that never live at the same time
  share memory ([O'Donnell, "FrameGraph", GDC 2017](https://www.gdcvault.com/play/1024612);
  [Filament frame graph notes](https://google.github.io/filament/notes/framegraph.html)). Big change in
  Godot; only worth it if PT/DLSS RR buffers turn out large and disjoint.
- **Right-sized formats where exact.** Only where the stored values fit the smaller format exactly
  (masks, indices, values already quantised); anything that would change a pixel is out.

### GPU-driven rendering

- **GPU culling and indirect draws** for instanced fields
  ([Haar and Aaltonen, "GPU-Driven Rendering Pipelines", SIGGRAPH 2015](https://advances.realtimerendering.com/s2015/aaltonenhaar_siggraph2015_combined_final_footer_220dpi.pdf);
  Wihlidal, "Optimizing the Graphics Pipeline with Compute", GDC 2016). Partly in place
  ([gpu-multimesh-records.md](gpu-multimesh-records.md)); timings need the PC.

### Shaders and pipelines

- **Fewer, earlier pipeline compiles.** Count shader variants per system, drop dead specialisations,
  and precompile the pipelines a scene will need at load (Unreal 5's PSO precaching; Godot 4.4's
  ubershader + background specialisation). Cloud-measurable as compile count, ms and RSS.

## Latest work (2025–2026) and what it means here

- **Cluster acceleration structures and partitioned TLAS**, now in both Vulkan (`VK_NV_cluster_acceleration_structure`,
  `VK_NV_partitioned_acceleration_structure`) and a DirectX preview (Oct 2026) on all three desktop GPU vendors.
  Microsoft's sample: animated AS memory 6.8 GB to 0.24 GB, per-frame build 788 ms to 3 ms; a partitioned TLAS
  rebuild 2.9 ms to 0.8 ms ([DirectX blog](https://devblogs.microsoft.com/directx/clas-ptlas-preview/);
  [Vulkan proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_NV_partitioned_acceleration_structure.html)).
  This is the Mega Geometry route already listed in [open-world-geometry.md](open-world-geometry.md); it needs
  hardware and drivers the cloud lacks, so it is a PC-stage item. Compaction is the step that works everywhere now.
- **Assassin's Creed Shadows (SIGGRAPH 2025 Advances).** A coarse ray-tracing world: low LODs, no skinning, small
  geometry culled, TLAS rebuilt each frame with time-sliced instance culling; about 2,000 BLASes and 30k instances
  at about 300 MB. Probe updates budgeted per frame
  ([slides](https://advances.realtimerendering.com/s2025/content/Advances%202025%20-%20Raytracing%20the%20world%20of%20Assassin's%20Creed%20Shadows.pdf)).
  Coarser trace geometry would change the PT image here, so it is out under our rule; budgeted updates are in.
- **Space Marine 2 (REAC 2025).** Static instance data packed to about 4 bytes per instance; GPU culling of 300k
  instances in about 0.5 ms; pipeline precache limited to the about 2k pipelines actually used out of about 200k
  permutations ([slides](https://enginearchitecture.org/downloads/REAC_2025_Saber.pdf)). The pipeline point
  matches our lavapipe finding that driver compile memory, not engine data, drives cloud peaks.
- **Unreal Engine 5.6.** Instance culling in chunks of 64 instances (including generated ones), distance-driven
  surface-cache updates, a shared per-frame budget for streaming and loading
  ([summary by Tom Looman](https://tomlooman.com/unreal-engine-5-6-performance-highlights/)).
- **BVH research.** Quantised wide BVH nodes and triangles cut memory traffic to about 18% of the baseline in
  simulation ([Grauer et al. 2025](https://arxiv.org/html/2505.24653v1)); merged-node BVHs 48% smaller and 32%
  faster ([Haydel et al., HPG 2026](https://graphics.cs.utah.edu/research/projects/bvh-merged-nodes/)); oriented
  boxes 18–32% faster traversal ([DOBB-BVH, HPG 2025](https://arxiv.org/html/2506.22849v1)). These live inside the
  GPU driver's BVH, so the engine cannot use them directly; they explain why compaction ratios differ by vendor.
- **Godot itself.** 4.5's shader baker precompiles shaders at export (load times 20x shorter on Metal and D3D12),
  on top of 4.4's ubershaders ([release notes](https://godotengine.org/releases/4.5/)). Godot's 2026 CPU-side work
  found per-frame vertex array creation and freeing as a top cost
  ([GameDev.net](https://gamedev.net/news/5741-optimizing-cpu-side-rendering-code/)), the same per-frame
  allocation pattern the profiling pass will look for.
- **Indie and itch.io.** Mostly instancing, HLOD merging and culling by fog distance, all already in the fork's
  vegetation and rock systems ([Maze Madness devlog](https://itch.io/devlog/898004/reasonably-seamless-and-scalable-wall-and-ground-rendering.amp):
  occlusion culling over 50%, distance culling 30–40%; [Sons of Ryke](https://alpaca-sponge-wsff.squarespace.com/sons-of-ryke/optimizing-an-open-world-1):
  separate low-poly shadow casters, HLOD). Nothing new for us beyond confirming the order: measure, cull, batch.

## Candidates for the fork

Ordered by expected win for the risk. Each is a separate commit with its own proof.

1. **BLAS compaction for static geometry.** Expected: large cut in PT acceleration-structure memory
   (half is typical), unchanged image by construction. Cost: one extra copy per static BLAS, once.
   Proof: byte-identical PT frame, AS memory before/after on the landscape and trees benches.
2. **CPU profile pass on the benches.** `perf` + `heaptrack` on landscape, trees, links, ground and
   oasis; fix the top hot spots and per-frame allocations, each with a measured gain and identical
   frames. Fixes touching another thread's files go through the coordinator.
3. **Shader and pipeline audit.** Variant counts, compile ms, RSS per system; remove dead variants,
   precompile at load. Identical frames by construction.

## Design: BLAS compaction

Follows NVIDIA's recipe (build with `ALLOW_COMPACTION`, query `COMPACTED_SIZE` after the build,
copy with mode `COMPACT`, free the original once the GPU is done with it).

- **Which BLASes.** Only static fast-trace ones: no `ALLOW_UPDATE`, no `PREFER_FAST_BUILD`. That is the
  surface cache, LOD entries, merged mesh groups and OMM surfaces. Deformed, swept-sphere and procedural
  BLASes refit every frame and stay as they are.
- **Where.** Inside `RenderingDevice`, so the RID never changes and no system above it needs to know.
  `acceleration_structure_build_batch` records a compacted-size query after the batch's builds. When
  that frame's fence has passed (the same point timestamps are read), the sizes are read without a
  wait, a right-sized structure is created, and the next batch records the `COMPACT` copies before its
  own builds. The RD record then points at the new driver structure; the old one goes on the frame
  disposal list.
- **TLAS records.** The compacted copy has a new device address. A relocation serial bumps with each
  swap; keyed TLASes whose cached records predate it rewrite all records once, and the PT pass rebuilds
  its TLAS that frame. Swaps are batched so this happens at most every few frames.
- **Rebuilds.** A compacted BLAS has no room for a fresh build, so a rebuild request reallocates it at
  full size first (and it is compacted again afterwards).
- **Proof.** A PT frame before and after must be byte-identical (same tree, same traversal). Memory is
  read from the RD buffer counters. Lavapipe may report little saving since its BVH layout differs from
  NVIDIA's; the real saving is measured on the PC.
- **Switches.** `rendering/rendering_device/acceleration_structure/compaction` (on by default). Env
  `RD_BLAS_COMPACTION_MOVE_ALL=1` moves every static BLAS even when the driver reports no saving, so the copy
  path runs on lavapipe. `--verbose` logs each step (`BLAS compaction: ...`).

## PC GPU bench list (when the PC is back)

- GPU frame time raster and PT on the landscape showcase, before/after each merged change.
- BLAS compaction: AS memory and trace time (compacted BLASes can be slightly faster to trace). Run
  `docs/bench/rt_memory` and the landscape showcase with `--verbose` and compare "MB to MB" in the log and
  the video memory line with the setting on and off.
- Any GPU culling or format change: per-pass GPU timestamps.

## Results

### BLAS compaction (cloud, lavapipe)

- New bench `docs/bench/rt_memory`: 24 static meshes (sphere, torus, subdivided box, fine relief) on a
  plane, path traced at 640x360, 4 spp. Lavapipe reports compacted size equal to the build size (12.69 MB
  of 12.69 MB), so it saves nothing here; with `RD_BLAS_COMPACTION_MOVE_ALL=1` all 6 BLASes moved through
  the compacting copy and the frame is **byte-identical** to the previous build (max difference 0), with
  persistent instances off and on (keyed TLAS rewrite path).
- Memory saving on real GPUs: PC bench list.

### Media skip for caustics and the camera probe

- Caustic photons ran every frame whenever caustics and refraction were on, one photon per pixel over three
  cascades, even in a scene with no water at all. Only a surface that bounds a medium can bend a photon
  (`RT_MAT_FLAG_VOLUME`: Transmission that is not thin-walled, or a shader writing TRANSMISSION and IOR), so the
  renderer now scans the material cache for one (again only when a material changes or a cache entry is rebuilt)
  and skips the photons and the per-pixel camera medium probe without one (`RT_MATERIAL_FEATURE_MEDIA`).
- Side effect found on the way: every dry point inside the caustics square read as "covered" by the map, which
  switched the radiance cache off there, so in a scene without water the cache was off around the camera.
  Without media the cache is back, as it was before caustics existed.
- Proof (rt_memory bench, PT 640x360 4 spp): with the water slab (`--medium`) the frame is byte-identical to the
  previous build. Without media, byte-identical to the previous build with caustics off when the radiance cache
  is off too (`--no-radiance-cache`; the cache is not bit-repeatable between runs, two runs of one build differ by
  up to 6/255). With the cache on, the frame equals "caustics off" within that run-to-run noise.
- On the PC, in an indoor scene with no water, caustics off saved 2.8 ms of 27.2 ms per
  frame; this change gives that back with caustics left on.
- Next: a dry point beside water (bent and straight photons equal, ratio exactly 1) still turns the cache off.
