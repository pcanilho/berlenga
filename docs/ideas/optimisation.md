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
- With water present, dry ground inside the square also lost the cache (every point the map held counted). Now
  only points where the map's ratio differs from exactly 1 (bent and straight photons unequal: real lines) keep
  the cache off. Proof: `--medium` frame byte-identical to the previous build with the cache off; with it on,
  same mean brightness (137.63 vs 137.62 of 255), differences are the cache's run-to-run noise.

### Dune and drift wind shear (CPU)

- `DuneShear::rows()` (dune sea and snow drifts) rounded and wrapped four indices per cell per reach step
  (32 by default: 128 `lround` and modulo pairs per cell). The offsets along the wind do not depend on the cell, so
  they are worked out once per call (and the row offsets once per row), templated on wrap; reach steps where the
  offset sits at a rounding tie keep the direct form. A standalone harness over 4 grid sizes, 6 wind directions
  (ties included) and both edge modes gives bit-identical output, 3.5 times faster (818 ms to 235 ms). Snow and
  ground `--test` pass as before.

## Design: path tracer cost that no setting removes (next round)

The PC numbers for one indoor scene: the path tracer pass grew from about 18 ms to 27 ms over a week of
features. The settings explain under half of that, and the rest is spread over many commits. Most features
are gated by runtime branches that the scene never takes. Runtime-gated code is not free on a GPU, for two
reasons:

- **Live state across `traceRayEXT`.** Every value the raygen keeps across a trace call is spilled to the
  ray tracing stack and reloaded afterwards. NVIDIA's path tracing work on a 2024 AAA title (Bavoil, NVIDIA
  developer blog, May 2025) cut spilled live state from 222 to 84 bytes and the main trace pass by 15%. It did
  this by turning a rarely-repeating loop into a branch (72 bytes gone), keeping radiance and throughput in
  fp16, packing ray directions into 32 bits and bounce counters into 16 bits, and moving demodulation out of
  raygen. Their tool is the Nsight Graphics GPU Trace "Ray Tracing Live State" tab: it lists, per call site,
  each spilled GLSL variable with its bytes.
- **Register allocation for the worst path.** A branch the scene never takes still reserves its registers,
  which lowers occupancy for every pixel (Laine, Karras and Aila, "Megakernels Considered Harmful", HPG 2013).
  The usual answer is permutations: compile the code out where the scene cannot use it (Pettineo, "The Shader
  Permutation Problem", 2018, on the trade with compile time; the engine already does this for fog, decals,
  SER and ray query shadows through `RT_FLAG_*` specialization constants).

Plan, after the bisect fixes:

1. On the PC, one Nsight GPU Trace of the indoor scene. Note the raygen's spilled live state per trace site,
   its register count and active threads per warp. The cloud's software GPU cannot report any of these.
2. Live-state fixes that leave the image identical: values kept across a trace only because of a feature the
   path did not take, loop-carried structs, 32-bit counters. Each is proven byte-identical on rt_memory.
3. Scene-feature specialization constants for the largest blocks the scene does not use. Candidates are
   media and caustics (the media flag already exists), snow, glaze and wetness, hair, and subsurface. Each one
   doubles the pipeline variants it touches, so only blocks Nsight shows as costly get one. A variant
   switch must not make surfaces vanish while it compiles: the engine already traces with the default
   material while a hit group compiles (b06913a5), and a feature variant can keep the full shader until its
   own is ready.

## Indoor scene regression: suspects in 73a3e160..50273c52 (read before the split builds report)

The first and largest step (+3.3 ms on the indoor scene) spans clearcoat (d029458f), back lighting (fbb60149),
thin glass (effa664d), layer textures and glass shadows (381dda05) and refraction with media (50273c52). Read for
costs every pixel pays whether or not the scene uses the feature, ranked by how likely they are to matter:

1. **Shadow any-hit for every candidate.** Shadow rays trace with `gl_RayFlagsNoOpaqueEXT`, so the any-hit runs for
   every surface a shadow ray meets, opaque ones included. Before 381dda05 it left at once for an opaque surface;
   since then it first loaded the entry's whole material (now 176 bytes) and the geometry record, then decided.
   Fix (lossless, done): decide from the flags word, and read the geometry and material only past the early out.
   Larger follow-up: build truly opaque surfaces as opaque geometry and drop `NoOpaque` from shadow rays, so the
   hardware never calls the any-hit for them. Blocked today by transparent entries in the main TLAS, which camera
   rays must keep seeing as opaque; needs its own instance mask or TLAS for shadow-only casters.
2. **Material record growth**, 128 to 144 bytes in this range (176 now). Every closest hit and every
   transmissive or scissored shadow candidate copies `materials[i]`; a 144 or 176-byte stride also puts most
   records across two 128-byte cache lines. Fix (lossless): a hot record of 128 bytes (textures, colours, flags,
   UV transforms) and a cold one for coat, hair, subsurface, medium and layer textures, read under their flags.
3. **Clearcoat in every light evaluation.** `MaterialProperties` carries five more values through
   `evalCombinedBRDFSeparate()` for every light; `materialSpecularF0()` and the coat's early out are cheap, but
   the bigger struct adds registers to the whole closest hit. Fix (lossless): a hit-group variant compiled
   without the coat for custom shaders that never write CLEARCOAT (as raster's LIGHT_CLEARCOAT_USED).
4. **Glass now passes shadow rays** (381dda05): rays that stopped at the first window now continue, call the
   any-hit with texture reads, and pay the rest of the traversal. That is the feature working (light through
   windows), not waste; a cost to measure, not remove.
5. **Medium tracking in raygen** (50273c52): a branch per segment while no medium is entered, plus the camera
   probe, already skipped without media (81753d3a).

The four split builds (d029458f, fbb60149, effa664d, 381dda05) say which step holds the time; 1 and 2 are the
first fixes to try on the one that does.

## Research round 2 (22 Nov 2026): beyond the regression

What shipped path tracers and recent papers did for speed, and where the fork stands.

| Technique | Source | Gain reported | Fork today | Image |
|---|---|---|---|---|
| Live-state cuts around trace calls (loop to branch, packed counters, fp16 radiance) | Indiana Jones PT, NVIDIA blog May 2025 | trace pass -15% (222 to 84 spilled bytes) | not measured yet (Nsight on the PC) | packing exact; fp16 needs a 100% crop check |
| SER with coherence hints | Indiana Jones (SER -11% before live-state work), Doom: The Dark Ages ("biggest optimizations"), SER whitepaper | 11-24% | SER on where supported, but `reorderThreadEXT(hitObject)` gets no hint bits | exact |
| Async compute: denoiser inputs and post of frame N-1 overlap tracing of frame N | RTXPT 1.9 (up to 20% with CPU and OMM memory work) | up to 20% total | serial | exact |
| CPU cost of instance-heavy scenes | RTXPT 1.9 | large CPU frame-time cuts | persistent slots, keyed TLAS | exact |
| Opacity micromaps | Doom: The Dark Ages, RTXPT | alpha-tested foliage | in (render_raytracing_omm.cpp) | exact |
| BVH refit instead of rebuild for deforming meshes | Doom: The Dark Ages (refit up to 10x faster) | build time | per-surface `rt_blas_update` modes | exact traversal results, refit trees trace slower when poses drift; keep the rebuild cadence |
| ReSTIR PT Enhanced (reciprocal neighbour selection halves spatial reuse cost, footprint reconnection, duplication maps, direct and indirect in one reservoir) | Lin, Kettunen, Wyman, I3D 2026 best paper | 2-3x faster than ReSTIR PT at lower error | ReSTIR DI (off by default) and indirect reuse | a different estimator: same converged image, different noise; a quality-and-speed feature, not a pure optimisation |
| Wavefront instead of megakernel | Laine et al. HPG 2013; recent comparisons | scene-dependent | megakernel raygen with SER | exact, large rewrite; SER gets most of the coherence win without it |

Order for the fork, after the bisect fixes (each proven identical on rt_memory and the showcase, raster and PT at
100% crop):

1. SER coherence hints: a few bits from the hit (material class: opaque diffuse, transmissive, hair, sky miss) passed to
   `reorderThreadEXT(hitObject, hint, bits)`. Small change, exact image; measure on the PC.
2. Live-state packing that keeps every bit: bounce counters and flags to 16 bits, ray directions octahedral-packed only
   where the value is rebuilt exactly, medium state kept as its 32-bit code rather than seven floats. fp16 radiance only
   if the crop check shows nothing.
3. Async compute overlap of the denoiser inputs and post with the next trace.
4. Then the scene-feature variants (design above), for what Nsight still shows.
ReSTIR PT Enhanced is a separate decision: it changes the noise, so it belongs on a card with shots.

Progress: (1) shipped, setting `rendering/pathtracing/ser_coherence_hints`, two hint bits (light through the surface or
a medium boundary, hair). (2) started with the medium: only its code and the scatter word read on entry stay live across
the bounce's trace, and the extinction, albedo and phase are decoded after it (8 words down to 2). The scatter word
must be kept, not reread: a later subsurface hit reuses the pixel's `rt_medium_image` texel. Both proven
byte-identical on rt_memory (dry and `--medium`); the timing needs the PC.

Sources: [Indiana Jones PT](https://developer.nvidia.com/blog/path-tracing-optimization-in-indiana-jones-shader-execution-reordering-and-live-state-reductions/),
[Doom: The Dark Ages](https://developer.nvidia.com/blog/how-id-software-used-neural-rendering-and-path-tracing-in-doom-the-dark-ages/),
[RTXPT 1.9](https://www.tweaktown.com/news/114290/nvidia-updates-rtx-path-tracing-for-developers-up-to-20-more-performance-and-windows-on-arm-support/index.html),
[ReSTIR PT Enhanced](https://research.nvidia.com/labs/rtr/publication/lin2026restirptenhanced/),
[SER whitepaper](https://developer.nvidia.cn/sites/default/files/akamai/gameworks/ser-whitepaper.pdf),
[Megakernels Considered Harmful](https://research.nvidia.com/node/2737).

## Path tracer warm-up across launches: the pipeline cache never settled

Seen on the PC: the Vulkan pipeline cache file grew on every launch (780, 793, 808 MB), and the path tracer could still
be waiting 30 s after loading on the second launch of a build (236 surfaces waiting), though ready by the third.

Both pipeline steps already go through the cache: each material's hit-group library and the final link
(`rendering_device_driver_vulkan.cpp`, `vkCreateRayTracingPipelinesKHR` with `pipelines_cache.vk_cache`). The SPIR-V
is cached on disk too (`RaytracingSPIRV`). What changed between launches was the key of the link. Hit-group slots were
numbered in the order materials registered, and that follows threaded loading, so the link's group order (and its
cache key) differed every launch: a miss, and one more entry in the file.

Fix (exact image: a slot index only picks the shader binding table record, the shading is the same):
`scene_shader_raytracing.cpp` keeps the slot order on disk (`RaytracingSPIRV/hit_group_slots.txt`, this launch's
materials in slot order, written once nothing is building and at exit). At start-up those slots are held for their
materials, and a material takes its held slot when it registers. A launch into the same scenes then links the same
groups in the same order, and the link hits the cache. Held slots that no material takes this launch are left empty and
dropped from the next file, so the list does not grow without bound.

Proof (lavapipe, a copy of rt_memory with five shader materials added in a chosen order, radiance cache off): old build
forward and reversed order, new build with no file, with last launch's file and the materials arriving reversed, and
reversed from cold: all byte-identical, and the file kept last launch's order when they arrived reversed. Same with
async compilation on (old and new identical to each other). Pre-existing, cloud only: with async compilation on,
lavapipe aborts at exit and the frame differs from the synchronous one, on the old build as well.

How the fork engine's cost of a project's scripts was found, for any project: time each call in the project's main
`_process` and in what it calls (a wrapper around each statement with `Time.get_ticks_usec()`), headless, on the scene
that the PC's frame profile flags. A once-per-frame layout of thousands of `MultiMesh.set_instance_transform` calls,
redone even when nothing moved, cost about 8 ms on the PC. Laid only when its inputs change, and handed over as one
`MultiMesh.buffer` per MultiMesh, it fell to well under 1 ms with identical instance data. Engine candidate from it:
none needed (`MultiMesh.buffer` is the bulk path); worth a line in the docs that per-instance calls in a loop are the
slow path.

## Main-thread CPU: the render command queue woke the render thread for every command

Profiled (perf, cloud, headless bench of four scenes, an unstripped build): with the render thread separate, every
`CommandQueueMT` push called `WorkerThreadPool::notify_yield_over` on the render thread's pump task, a mutex and a futex
syscall each. The render thread then ran one command and went back to sleep, and the next push woke it again. Skinned
characters push one command per bone per frame, so this ran hundreds of times a frame: 4-5% of all samples were the
wake-ups alone. Upstream Godot does the same.

Fix (`core/templates/command_queue_mt.h`): the pump is woken only when the queue was empty since its last flush. A flush
runs until the queue is empty, so commands pushed meanwhile are run by it; the wake is latched (`yield_is_over`), so a
push just before the pump yields is not lost. Same commands, same order. Cloud headless frame p50: office 7.9 to 6.9 ms,
roof 13.5 to 12.3, metro 14.2 to 12.8, station 15.8 to 13.8. Frames byte-identical (rt_memory, raster and PT, render
thread separate); the gameplay test passes.

Next in the same profiles: per-bone `skeleton_bone_set_transform` calls (one per bone, even for characters hidden in
other parts of the level), the cloth solver on the main thread, and the project's own skeleton modifiers.

## Scene-feature elision: snow, glaze and ground code out of the hit shaders

The split builds put most of the indoor scene's path tracer growth between eb40e5d1 and 03f1cfaa, and 1.2 ms more
between bb187ac2 and 8db26ca5. Both ranges add BaseMaterial3D features to the shared closest-hit (snow and its
sparkle, glaze, ground deform with its sand grain glints, exact snow sparkle). Every BaseMaterial3D surface uses
that one hit shader, and it reads a material's switches from its record at run time, so a scene with no snowy
material still pays the registers of the snow block on every hit (Laine, Karras and Aila 2013, above). This is
step 3 of "path tracer cost that no setting removes", for the three blocks the bisect points at.

- Three `RT_FLAGS` bits (`RT_FLAG_SCENE_NO_SNOW`, `_NO_GLAZE`, `_NO_GROUND`, bits 11 to 13). The accessors in
  `raytracing_data_inc.glsl` return "off" while their bit is set, so the driver folds the specialization
  constant and drops the block. The SPIR-V does not change (the compile-context key leaves these bits out), only
  the pipeline.
- The material storage counts, for each bit of `RTBaseMaterialState`, how many materials hold it (set in
  `material_set_param`, cleared in `material_free`). A material created this frame is counted before the frame
  picks its variant, whether or not it is drawn yet, so the count never lags the records.
- The frame never traces a variant that leaves out a feature the scene holds. While the wanted variant builds,
  it traces with the variant that leaves nothing out when that one is ready, else with the last variant when its
  left-out set is within the wanted one's, else raster (as any variant switch). Once the elided variant is in
  use, the full one is kept built, so a snowy area streamed in later is traced at once.
- `rendering/pathtracing/scene_feature_elision` (default on) turns it off, for A/B timing on the same build.

Stage 2 adds four more bits on the same rules (bits 14 to 17): `RT_FLAG_SCENE_NO_MUD` (a vehicle's body mud),
`_NO_CANOPY_WET` (a leaf's intercepted water and beads), `_NO_MOVING_WET` (slipstream and wiper) and `_NO_HAIR`
(BaseMaterial3D Hair, its mask, melanin and strand frame; a custom shader's `HAIR` keeps the fibre lobe). The
Nsight trace backs the method: occupancy is set by the register file (76% allocated, 14 to 15 of 48 warps).

Not elided: wetness itself (rain is on in most scenes), subsurface, clear coat and the base specular's own
normal (`SPECULAR_NORMAL`, which custom shaders can write). Each is a candidate once the PC trace shows its cost.

Proof on the cloud's software GPU, against the build before it: the memory scene (radiance cache off) traces the
variant that leaves all three out and saves the same frame byte for byte, with the setting on and off. A copy of
it with snow from the start (only glaze and ground left out), with all three features on (nothing left out), and
with snow switched on mid-run (six frames saved across the switch, traced at once with the full variant) all
match byte for byte. The snow bench's own frame is not repeatable between two runs of one build, so it is not a
check; its `--test` passes.

## Memory: re-spirv kept a parsed copy of every material shader

A heap profile (heaptrack) of the landscape bench's farm still put 3.3 GB of its 8.5 GB heap peak in
`RenderingDeviceDriverVulkan::shader_create_from_container`. For every shader with specialization constants (2,353
material shader variants in that scene), the driver kept a parsed `respv::Shader` per stage. That parsed copy holds
the SPIR-V plus its instruction, block, result and decoration tables, about 1.4 MB a shader. It exists so that
re-spirv (upstream Godot 4.5) can specialize the SPIR-V when a pipeline is made.

The driver now keeps only the SPIR-V bytes. It parses them again when a pipeline is made, keeping the last parsed
stage per thread (a shader's variants are made one after another). The parse is deterministic, so the
specialized SPIR-V and the pipelines are the same: the memory scene's raster and path traced frames match byte for
byte. Farm still, resident after `load_all_heights`, same session: 7.8 GB, from 10.6 GB.

## Curve3D.get_closest_offset: bounded blocks instead of every segment

A main-thread sample of the landscape meadow view (lavapipe, plants on) spent about 40% of its time in
`Curve3D::get_closest_offset`, called from `PhysXRiver3D` for `WorldWater.high_water_at` (each grass layer's
flood map, 64 x 64 points every 5 s) and `WorldWater.height_at` (weather spray sources). Godot's version tests
every baked segment; a 4 km river baked at 0.2 m has about 43,000. One flood map took 0.94 s of CPU; it now takes 29 ms (33 times less) in the same micro-bench.

This is the bounding volume hierarchy method for nearest-point queries (two levels here): the bake also keeps
the bounds of each run of 16 segments and of each 16 runs. A query tests the run nearest in bounds first, then
only the runs whose bounds come within the best distance found, plus a margin far above float rounding
(1e-4 of the coordinate scale). Each segment is tested with the same arithmetic as before, and equal distances
go to the lowest segment, as the old loop's first strict minimum did, so the offset is the same to the bit.
Short curves (under 64 segments), curves with a zero-length interval and non-finite points keep the old loop.

Proof: 24,000 queries over eight curves (open, closed, a straight line with ties, far from the origin, on
baked points) hash the same as the previous build.

## Sky light: the panorama read back without stalling the GPU

The same meadow sample caught `TerrainGenerator3D::_sky_light_poll` waiting in `RenderingDevice::_stall_for_frame`:
`RenderingServer.sky_bake_panorama` copies the radiance map out and reads it with `texture_get_data`, which flushes
and waits for every frame in flight. The terrain asks for its 64 x 32 sky panorama every 2 s while it runs, so the
game lost a frame's worth of GPU overlap each time (and the main thread waited on the render thread as well).

`RenderingServer.sky_bake_panorama_async` does the same copy and reads it with `texture_get_data_async` (the
staging-buffer readback Godot already uses for `RenderingDevice.texture_get_data_async`); the Image reaches the
callback on the main thread a few frames later. The terrain keeps one request in flight and runs the same update
when it lands. Renderers without that readback (Compatibility, dummy) bake at once and hand the Image over the
same way. Proof: a sky baked both ways gives the same bytes, radiance and irradiance (3 to 4 frames later on
the cloud's software GPU). The sky light reaches the terrain those few frames later; its interval stays 2 s.

The water module's two periodic sync readbacks of the same kind (the raster reflections' 128 x 64 sky panorama
and the underwater sky radiance, each every 240 frames per water body) now go through the same call:
`WaterSkyReadback` keeps one request in flight and the owner runs the same update when it lands. The water
surface bench (raster, beach, 60 frames at a fixed 30 fps) looks the same; its frames differ from run to run
by the simulation alone (0.06% of pixels between two runs of the old build, 0.12% against the new one, all
foam and floating boxes).

## Memory: static surface BLASes were never freed

A path traced still of the rocks world (gully, 640 x 360, cloud) peaked at 13.5 GB resident, 9.1 GB of it GPU
memory, and 6.1 GB of that acceleration structures (`RenderingDevice.get_memory_usage(MEMORY_ACCELERATION_STRUCTURES)`,
new for this; `docs/bench/rocks --mem-report` prints it). One 1.08 M triangle mesh was there twice: once plain, once
deformed. A captured surface traces its bare mesh until its first capture lands, and the bare mesh's BLAS then
stayed for the rest of the session. The static surface cache had no eviction at all (the deformed, MultiMesh and
mesh LOD caches already free what goes unused), so the same held for meshes that left the view or were freed.

`RenderRaytracing::_evict_surfaces` now frees a static surface no instance traced for 120 frames, as
`_evict_surface_lods` does for LOD levels, checking 64 cache chunks a frame. A surface traced again is built again
from the same mesh, in the frame it comes back. BLASes that persistent instance slots still trace (they skip the
cache while a slot is unchanged) stay.

Proof: `docs/bench/rt_memory --evict` hides every mesh for 150 frames, then shows them again. The new build frees
their acceleration structures (12.7 MB to 0) and the saved frame matches the previous build byte for byte, with
and without `--persistent`; so do the plain frame and a 200 frame `--persistent` run.

That alone left the gully still as it was (it is saved well within 120 frames of the captures landing). So a
surface traced through a capture also records its cache key, and the sweep frees its bare BLAS as soon as no
instance traced it in the last frame (one that still traces the bare mesh, or a persistent slot holding it, keeps
it). Gully still, same view: acceleration structures 6,080 to 4,505 MB, GPU memory 8,866 to 7,291 MB, peak
resident 12.9 to 12.1 GB. That still is not bit-repeatable between runs (wind, capture timing), so its proof is
the rt_memory runs above plus a side by side look; the freed and rebuilt path is the same one.

## Water: one GPU step per drawn frame by default

A slow frame runs several physics ticks (up to `physics/common/max_physics_steps_per_frame`, 8), and with
`PhysXWaterSurface3D.step_mode` per tick every tick ran the surface's whole GPU step. The late-frost meadow had 20
water solvers (surfaces and their cascades): 160 steps and about 1,860 compute passes a frame, which made the frame
slower still (on the cloud's software GPU, frames of 100 s that kept the tick count at 8).

`STEP_MODE_PER_FRAME` (already in the module) records each tick and runs them in one step per frame: the ripple
layer still takes one sub-step per tick with that tick's bodies, while the FFT ocean (evaluated at the summed
time), foam, swash, blits and readbacks run once. It is now the default. Where a frame holds one tick (a game at or
above its physics rate) nothing changes; frames with several ticks now cost one step. Look test: the water surface
bench, beach and surf views (raster, fixed 30 fps, two ticks a frame), per tick against per frame: the same water,
differing only by the run-to-run simulation noise.

## Path tracing live state: the cut-out retry's copy of the path state

The camera cut-out loop kept a copy of the whole `PathState` (60 bytes) across every bounce's trace, to repack the
payload for a further layer. A surface that opens in the cut answers with `hit_t` alone, so the retry now resets
`hit_t` in the payload as it is. Byte-identical: rt_memory and the links bench's cut-out path traced frame. The
rest of what the raygen keeps across traces waits for the PC's Nsight "Ray Tracing Live State" table, since the
driver may already recompute the values that come from the scene data (camera matrix and ray).

## Setup time of the full landscape (all features on), and a world cache

Cloud profile of `docs/bench/landscape` at its defaults (main thread and every worker sampled with gdb): setup
takes 6.3 min. Rocks placing all 1.17 M stones of the 4 km world as plant obstacles 148 s (half of it in the
terrain's height query, `TerrainErosionFilter::offset`); terrain generation 85 s (hydraulic erosion); dunes 58 s
(`TerrainDunes::_carry`, whose sand transport is order-dependent within a tile, and tiles already run in
parallel); cover 37 s; snow climate 31 s. The steady frames there are bound by the software GPU, so their cost
waits for a PC trace.

A bit-exact speed-up of the erosion filter (sin and cos from one range reduction, skipping cells whose pivots are
all out of reach) measured 0%: the compiler already shares the reduction, and the rest is the octaves' own
arithmetic. The setup cost is the simulations themselves.

The lossless lever is not to run them twice. Terrain tools bake their output once and load it (World Machine and
Gaea export baked heightmaps; Unreal's World Partition and Houdini Engine cook procedural content at edit time and
stream the result), and the generator here is already deterministic (TerrainDMath) so its outputs are a pure
function of its inputs. A world cache would store each stage's outputs (heights and sediment after erosion, sand
depth after dunes, snow climate, rock obstacle cells) under a hash of every input that stage reads (its
properties, its inputs' hashes, the engine commit), load them when the hash matches and regenerate otherwise. The
loaded data is the same bytes, so frames are unchanged; a second launch skips minutes.

Built (terrain stage). `modules/terrain_deform/world_cache.{h,cpp}` keeps each piece on disk, zstd-packed, under
the first 16 bytes of a SHA-256 of everything that piece reads plus the stage name and the engine binary's
modified time and size (so any rebuild starts clean). Pieces are per tile within each stage: an erosion tile's key
is its `TerrainHydraulic::Params`, input heights and cover; a dune tile's is `TerrainDunes::hash_inputs` (params,
winds, base, masks, supply, initial sand, steps). A stale or short file is ignored and recomputed; writes go to a
temporary name and are renamed in place. Settings: `world/cache/enabled` (on) and `world/cache/path` (empty: the
user cache folder, `berlenga/world`).

Proof on the landscape bench, empty cache then a second launch: the erosion outputs (delta, flow, deposit) and the
dune outputs (sand, plants) hash the same both times, as do the logged sand volume and height change. 82 pieces,
57 MB. Generation 79.6 s to 13.5 s, dunes 53.3 s to 1.3 s: about 2 min off each launch after the first.

Rock obstacle cells (built): `RockField3D.get_obstacles` keeps its cells by 512 m zone per layer. The key holds
this field's and the generator's stored properties (marshalled as a scene stores them), the generator's result
(`TerrainGenerator3D::hash_result`: ground, water, rivers, sediment, sand, touch-up masks, holes) and each relief
provider's properties and heights (`WorldReliefProvider::contract_relief_object/state`, Landscape3D). A script ground
stub or a relief provider without a key turns the cache off for that query. A terrain change misses every rock zone,
since stones sample the ground everywhere. Proof: the obstacles' hash (1,167,958 stones) and the rest of the scene's
log match cold and warm; 129 s to 0.5 s. The warm run builds fewer shape libraries during setup (only those the
drawn cells ask for), which only saves time and memory.

Snow climate (31 s): not cached. Its bake reads the fire, frozen ground, obstacle lee and salt contracts at every
step, so a sound key would need every one of those systems' state.

## Undrawn frames (viewport disable_3d)

The landscape bench places forests and cover over frames with the main viewport's 3D off. A GPU profile
(`--gpu-profile`) of those frames on the software device showed two passes whose only output is a drawn 3D frame:
the water caustics map (about 5 s a frame there) and the rain and snow shelter heightfield collider (about 9 s:
`RainFall3D` and `SnowFall3D` set the collider's size each frame, which re-renders the geometry under it).
`RendererViewport::has_3d_viewport()` says whether any active viewport draws 3D. While none does,
`RendererSceneCull::render_particle_colliders` keeps the colliders in its list and renders them on the first frame
that draws (before the particles update), and `WaterSolverGPU::rt_render_caustics` defers the map, which the
surface asks for again on the next frame without a water tick. Every drawn frame runs exactly as before. Undrawn
frames there went from about 15 s of GPU work to under 1.5 s.

The check is per world: a viewport counts only when it draws the collider's or the water's scenario. The far grass
colour bake draws its own world in a SubViewport for about 117 frames while the main view waits undrawn; those
frames no longer run the landscape's caustics and collider. The bake wait on the landscape bench went from 474 s at
100 frames to 258 s.


## Path tracer pipeline build memory: one call site, no standby variant on a CPU device

Building the landscape's path tracing pipeline on the software device passed 12.5 GB. Drivers turn each material's
hit group into one closest-hit program with its whole call tree inlined (SPIR-V to NIR inlines every function; the
software driver then keeps every stage's code and links the whole pipeline into one program), and every material's
closest hit carries the shared integrator, `shade_and_bounce()`. Counting instructions after inlining in the cached
SPIR-V (`spirv-opt --inline-entry-points-exhaustive`, and a call-site count per function) showed that integrator at
about 320k instructions per hit group, most of it copies: `shade_local_surface()` and
`sample_surface_continuation()` were each called from two places (the transparency peel and the opaque hit), and
`surface_continuation_pdf()` from four places in each continuation, so the compiled code held two copies of the
lighting, two of the bounce and ten of the density. This is ordinary inlining growth (code size is the sum over call
sites of each callee's size); the remedy is one call site per heavy function.

- `shade_and_bounce()` runs the peel and the opaque hit through one call of `shade_local_surface()` and one of
  `sample_surface_continuation()`; each path keeps its own steps before and after, in the same order, with the
  same random numbers.
- `sample_surface_continuation()` wraps `sample_surface_lobe()`, which picks the lobe and direction and says whether
  the density is owed; the wrapper takes it once, for `ps.next_ray_dir` under the same material.
- The default hit shader went from about 400k to 215k instructions after inlining, and a materials bench start from
  82 s to 19 s on the software device. The software device's output is not repeatable run to run (a few hundred
  pixels of a 480x270 frame at 4 spp move by up to about 30 levels between two runs of one build); old against new
  stays inside that on seven material scenes (coat, layer textures, transmission, back light, hair, subsurface,
  water).
- Scene-feature elision keeps a second, standby variant (nothing left out) built so a feature that streams in is
  traced at once. On a CPU device that doubles every hit group's memory and its link alone needs gigabytes, and a
  variant takes it minutes to build anyway, so a CPU device skips the standby. Traced frames use the scene's own
  variant either way.
- The water surface's hit and any-hit shaders were the largest (its `vertex()` and `fragment()` run in them): the
  path tracer's foam gradient and the per-vertex normal each took four central-difference samples through four
  calls, four copies of the foam or wave code. Each is now one call in a four-step loop (same arguments, same
  arithmetic after); a body that size is never unrolled. Water hit shader 916k to 656k instructions after inlining,
  any-hit 787k to 527k. Raster draws per-pixel normals by default, so it does not run the vertex samples.
- Where the rest goes on the landscape (software device, allocations of 32 MB and up caught in a debugger): the
  pipeline link itself is small now (about 50 MB); the last 4 GB before the peak are GPU allocations the path tracer
  makes for its first frames (256 MB blocks, which on a CPU device are system memory) and the raster material
  pipelines compiling on a worker at the same time (glibc arenas of 64 and 128 MB). `MALLOC_ARENA_MAX=2` trims a
  little. The landscape at 640 still peaks near 13 GB there; the next cut is in those allocations, not the shaders.
- Those allocations, on a CPU device only (a GPU keeps every path as before): MultiMesh instances are traced as
  TLAS instances of the mesh's own BLAS instead of being copied into merged per-tile BLASes (the merge cap,
  `rendering/pathtracing/multimesh_merged_blas_max_triangles`, is held at its floor of 256 there). Merging trades
  memory for trace speed on hardware traversal; a software device has the memory cost (merged vertex copies,
  per-tile BLASes and their build scratch, all system memory) and none of the gain. The same triangles are traced
  either way. And the free heap is handed back to the system (`malloc_trim`) at most every 5 s while a CPU device
  renders, since the pipeline compiles on worker threads leave glibc arenas full of freed pages that the next
  allocation spike stacks on. The landscape at 480x270 now saves its path traced frame on the software device, peak
  11.9 GB resident (it was killed at about 13.5 GB). A GPU-written MultiMesh scene and the grass meadow match the
  previous build inside the run-to-run noise.
- With the full art set (ground, stone, cave, floor and bark scans, the showcase props) the landscape went back over
  13 GB on the software device, and not every run: the peak swung between 12.4 and 13.5 GB. Memory maps at 10 GB:
  4.3 GB device memory, 4.3 GB main heap, 0.8 GB in worker arenas. A debugger on every mapping of 32 MB and up showed
  the last 2.8 GB is the driver compiling the path tracing pipeline: the software driver links every hit group into one
  compute program (Mesa's lavapipe, `lvp_compile_ray_tracing_pipeline`) and LLVM holds gigabytes while it optimises
  it, while raster pipelines compiled on other threads added theirs at the same moment. On a CPU device only:
  - a ray tracing pipeline compiles alone (`RenderingDevice`: raster and compute pipeline creation take a shared lock,
    ray tracing pipeline creation the exclusive one), so the two spikes no longer meet;
  - scans that systems load from files at run time (TerrainMaterial3D, RockField3D, CaveNetwork3D, VegetationField3D
    and TreeGenerator scan sets) are halved by 2x2 boxes to at most
    `rendering/textures/scans/cpu_device_max_size` (1024; 0 keeps them whole), the full texture's own next mip
    levels; the scans are 2K, so this is a quarter of their memory;
  - the vertex capture arena keeps no 50% headroom.
  The landscape with the full art set then saved its frame at a 12.3 GB peak. `RenderingServer.get_texture_debug_usage()`
  (new binding) lists every texture with its size and source, and the landscape bench's `--tex-report` prints the
  largest after each memory line: textures were about 1 GB of it, not the cause.
- Cliff wall views (rocks bench `--world --view=cliff`): placing the wall blocks and cliff kit pillars is under 4% of
  the CPU there; 82% is Landscape3D building its cliff relief meshes (`_build_cliff_surface`), most of it the same
  reads made again at one point. Now each edge's split test runs once (an edge is shared by two triangles) and keeps
  its middle's displacement for the vertex a split puts there, and the per-vertex pass reads the heightfield normal,
  face weights, fold scale and the two relief scale bounds (`_cliff_scale_limit`, `_bank_scale_limit`) once, shared
  by the displaced point and the CUSTOM channels (`ReliefFrame`). All 611 chunk meshes of the view hash the same
  (positions, normals, CUSTOM0-2, indices) and the frame is byte-identical; their build time fell by a third.
  Each chunk's build then keeps the grid normals it reads (`LandscapeNormalCache`, over the chunk and the face
  weights' reach, filled as read): the relief read the same grid normal tens of times. Again every mesh and the frame
  are the same, and the build time fell by another fifth (981 s summed over the view's chunks, then 648 s, now 511 s).
