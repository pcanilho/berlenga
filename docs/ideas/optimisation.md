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

## PC GPU bench list (when the PC is back)

- GPU frame time raster and PT on the landscape showcase, before/after each merged change.
- BLAS compaction: AS memory and trace time (compacted BLASes can be slightly faster to trace).
- Any GPU culling or format change: per-pass GPU timestamps.

## Results

None yet.
