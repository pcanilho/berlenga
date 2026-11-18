# Ground detail as real geometry near the camera

Research note, 18 Nov 2026, written before any code. A shared GPU system that fills the ground near the
camera with small real geometry (grit, gravel, shingle, pebbles, leaf litter, twigs, mud clods, snow
clumps) for every procedural system, seen identically by raster and the path tracer.

## The gap

- `RockField3D.min_size` is 8 cm: "smaller grains stay in the ground material". Below that, scree, till,
  shore shingle and river gravel are `TerrainMaterial3D.rock_detail_enabled` (a Worley-cell normal).
- Leaf litter and twigs under 1 cm are cards placed by `VegetationField3D` (forest-floor.md).
- Mud, sand and snow have a 1.56 cm near field (`GroundSurface3D.near_field`, 8 m), traced from its
  capture, so the ground's shape is real geometry there; what sits on it (clods, clumps, grit) is not.

A normal map has no silhouette, no contact shadow, no parallax and no occlusion of the light under a
grazing sun. Under the path tracer that is what reads as "flat ground". Real geometry here adds detail to
both renderers and removes nothing.

## Hard constraint: the path tracer must see it

- **Mesh shaders do not reach the BLAS.** Geometry emitted by a task/mesh shader exists only in the raster
  pipeline. Godot has no mesh shaders either.
- So the geometry must live in buffers a BLAS can be built from, or be instances of shapes that have one.
  The fork already has three routes:
  1. **GPU-written MultiMesh records** (gpu-multimesh-records.md): a compute shader writes records,
     `multimesh_mark_buffer_rd_changed` hands them to the path tracer. GrassField3D's GPU fill uses it.
     Merged MultiMeshes (up to `multimesh_merged_blas_max_triangles`, 65,536) rebake one BLAS from the GPU
     buffer the same frame; expanded ones get one TLAS entry per record after an async readback (one or two
     frames late).
  2. **Captured vertex buffers** (displaced-hit-varyings.md, ground-deform.md): a surface whose `vertex()`
     moves `VERTEX` is captured and its BLAS built from the capture. The 115k-vertex ground patch already
     goes this way every frame.
  3. **Procedural AABB geometry** (`RTProceduralInstance3D`, intersection shaders): one AABB per element and
     an implicit surface hit in the shader. Raster would have to ray-march the same surface on a proxy.

## What others do

- **Raad et al. 2025**, "Real-time procedural resurfacing using GPU mesh shader" (Eurographics, CGF 44(2)).
  A base mesh is resurfaced per face and per vertex with parametric elements (torus, sphere, cone,
  B-spline cages such as scales); the task shader culls and picks LOD per base face, the mesh shader
  evaluates the element on a UV grid and emits triangles. Pebbles: a 4x4 B-spline cage built from each base
  face, extruded, rounded and perturbed by noise, subdivided adaptively. Code (Andarael/resurfacing) is
  CC BY-NC: technique only, no code. Raster only.
- **Gravel** (michaelsalton/Gravel, 2025-26), an extension of Raad: priority-sorted placement slots per face
  (elements appear and vanish at fixed points as density changes, so nothing swims), a pebble ground that
  follows the player, alpha-to-coverage fade as elements go sub-pixel, projected-space NDF filtering for
  specular aliasing (Tokuyoshi and Kaplanyan 2021) with an extra term for the element normals, and
  **proxy shading**: past a distance the base face is shaded with aggregate roughness, mean tilt,
  self-shadowed albedo and coverage of the elements it no longer draws. Also raster only.
- **Peytavie et al. 2009**, "Procedural generation of rock piles using aperiodic tiling" (CGF 28(7)).
  A few tiles of rocks are piled by a physical simulation once, with matching borders, and an aperiodic
  tiling repeats them over any area without visible pattern. The cost of a pile becomes the cost of a few
  tiles. Directly usable: PhysX settles the tiles at load.
- **Horizon Zero Dawn** (van Muijden, GDC 2017, "GPU-based run-time procedural placement"): placement
  runs on the GPU in tiles round the camera from density maps built by rules over the terrain data, and is
  regenerated as the camera moves; nothing is stored. Our GrassField3D GPU fill is the same idea.
- **Ghost of Tsushima** (Wohllaib, GDC 2021, procedural grass): blades generated per tile in compute each
  frame from a few parameters, LOD by distance with a blade-count hand-over. Raster only.
- **Unreal Engine 5.7.** Nanite Tessellation (`r.Nanite.AllowTessellation`, `r.Nanite.Tessellation`,
  material Displacement with pixel-size fade) is raster: hardware ray tracing uses the Fallback Mesh
  (`r.RayTracing.Nanite.Mode 1` is experimental). Nanite Foliage (assemblies of instanced parts, voxel bricks
  far away, bone wind) also traces its fallback mesh; users report polygonal RT shadows. PCG GPU with
  FastGeometry (`pcg.RuntimeGeneration.ISM.ComponentlessPrimitives 1`) scatters dense ground clutter as
  instances. So UE's own answer for traced ground debris is instances.
- **NVIDIA RTX Mega Geometry.** `vk_tessellated_clusters` path traces displaced geometry that is tessellated
  every frame: cluster templates (pre-built per tessellation pattern) are instantiated with compute-written
  vertex positions (`VK_NV_cluster_acceleration_structure`). `vk_animated_clusters` (RTX 6000 Ada, 8.43 M
  animated triangles): triangle BLAS with 10% rebuilds 5.22 ms build, 564 MB; templates 0.80 ms, 271 MB,
  slightly slower trace (1.52 against 1.39 ms). Displaced micro-meshes (`VK_NV_displacement_micromap`) are
  deprecated: NVIDIA archived `vk_raytrace_displacement` in Jan 2025, "Mega Geometry supersedes" them.
  NVIDIA only; not in the fork (Valence has cluster AS).
- **Displacement traced directly**: Thonat et al. 2021 (tessellation-free displacement for ray tracing),
  RMIP (oblong bounding), Hoetzlein 2025 (projective displacement). Heightfield-only: cannot make loose
  stones, leaves on leaves or overhangs.
- **AMD 2025**, tetrahedral cages for massive animated geometry: rays are moved into the rest cage instead
  of rebuilding BLAS. Relevant later for moving elements; not needed for static ground detail.
- **Kuth et al. 2024** (HPG, GPU work graphs) and **2025** (GPU tree generation): procedural geometry
  amplified on the GPU each frame, mesh nodes for raster. Same RT limitation.
- **Meng et al. 2015**, "Multi-scale modeling and rendering of granular materials" (SIGGRAPH): explicit
  grains near, then aggregate (volumetric then BSDF) appearance far. The far hand-over our proxy shading
  should follow.
- **Godot addons and itch.io.** ProtonScatter (MIT), godot-multimesh-scatter, Spatial Gardener: CPU
  MultiMesh scatter, editor-time. GodotGrass (2Retr0) and compute-shader-plus: compute-driven grass, raster
  only. itch.io offers pebble textures and Blender rock generators: nothing GPU-placed or ray traced. No
  addon justifies bundling.

The pattern everyone converges on for traced debris: **a small library of shared shapes, placed per
instance on the GPU, culled by size and distance**, with the far field handed to the ground material.

## How many elements, and what they cost

Assumptions: an element is drawn while it is at least 3 px tall (60 degree vertical field of view),
single-layer cover at 80% packing, the view frustum plus a margin (31% of the disc round the camera; the
path tracer keeps a ring behind for shadows), LOD by projected size (12 to 300 triangles). Script:
area-weighted over the disc. BLAS about 67 B per triangle (NVIDIA's reservation above), vertices and
indices about 24 B per triangle, a TLAS entry with its geometry row about 192 B.

| size class | drawn out to (1080p / 2160p) | per m2 at full cover | in view at full cover (1080p / 2160p) |
|---|---|---|---|
| grit 0.5 to 1 cm | 2.6 / 5.2 m | 18,100 | 117k / 469k |
| fine gravel 1 to 2 cm | 5.2 / 10.3 m | 4,500 | 117k / 469k |
| gravel 2 to 4 cm | 10.3 / 20.6 m | 1,130 | 117k / 469k |
| pebbles 4 to 8 cm | 20.6 / 41.3 m | 280 | 117k / 469k |

Density falls with size squared and range grows with size, so every class costs about the same count.
All four at full cover: 469k elements and 8.5 M triangles at 1080p; 1.9 M elements and 35 M triangles at
2160p. A real place mixes classes (a shingle beach is mostly two of them; a forest floor is leaves and
twigs), so a busy view is about a quarter to a half of that.

| approach | 1080p, quarter cover | 1080p, full cover | 2160p, full cover |
|---|---|---|---|
| A. instances of a shared library (32 shapes per class, 3 LODs) | 27 MB, 117k TLAS entries | 95 MB, 469k | 365 MB, 1.9 M |
| A2. A with pre-settled tiles of 64 elements (Peytavie) | about 60 MB, 2k entries | about 70 MB, 7k | about 100 MB, 30k |
| B. unique geometry per element (resurfacing in compute) | 190 MB, 2.1 M tris | 765 MB, 8.5 M tris | 3.2 GB, 35 M tris |

- **B** also rebuilds BLAS for every tile that streams in or changes: at the published fast-build rate
  (about 0.85 M triangles in 5 ms including the rest of the frame's builds) a full 8.5 M rebuild is tens of
  ms, so it must be spread over frames, and a moving camera always has tiles to build.
- **A** builds no BLAS at run time (the library is built once at load). Its cost is TLAS size: the TLAS is
  rebuilt from scratch every frame (open-world-geometry.md), and expanded MultiMesh records are walked on
  the CPU each frame (instanced-forests.md). Hundreds of thousands of entries need either A2 or GPU-written
  TLAS entries.
- **A2** groups elements into tiles that were settled together by PhysX (stones resting on stones, leaves
  over leaves), so a tile is one BLAS shared by every copy and one TLAS entry. The tile's BLAS is bigger,
  but there are few variants. Where the ground is deformed (ruts, prints) or an element must move, the tile
  breaks into single instances.
- **Grit** below about 1 cm is the near field's job (1.56 cm displacement, already traced). Geometry
  starts at the fine gravel class.
- Raster: 2 to 9 M small triangles near the camera. Triangles of a few pixels shade inefficiently
  (quad overdraw), so LOD must keep them at about 8 px or more; alpha-to-coverage fade at the far edge
  (Gravel) hides the hand-over. Expected 1 to 3 ms on the RTX 4090 at 1080p: PC queue to measure.
- Path tracer: more instances under every ground hit deepen traversal. Measure trace ms with the feature
  off and on in the same view (PC queue). Shading is unchanged (the same ground material on the elements).

Cloud measurements possible now: element counts, triangles, library and tile sizes, and the frame-time
split on lavapipe as a relative check. Real GPU timings (trace, TLAS build, raster) go on the PC queue.

## Pros and cons

| | A / A2: shared instanced shapes, GPU placed | B: unique geometry per element |
|---|---|---|
| Raster and PT match | Exactly: same records, same meshes | Exactly, if the BLAS is built from the same buffer |
| Memory | Tens of MB | Hundreds of MB to GBs |
| Run-time BLAS builds | None (TLAS only) | Every streamed or changed tile |
| Variety | 32+ shapes per class, random rotation, non-uniform scale, tint; tiles pile them naturally | Every element unique |
| Contact and piling | Tiles settled by PhysX; single instances sunk by a third | Per element, by rule |
| Works on AMD, Intel, macOS | Yes | Yes, but memory-bound |
| Engine work | GPU-written TLAS entries (scale), per-instance culling | Large per-tile BLAS scheduling |
| Interaction | Elements become PhysX bodies when touched, then return | Rebuild the tile |

Variety is the only thing B wins, and at 1 to 8 cm 32 shapes per class under random rotation and scale
do not repeat visibly (RockField3D already shows this at larger sizes).

## Proposal: GroundDetail3D

A node in `modules/ground` with one GPU pipeline shared by all systems:

1. **Families and libraries.** Each family is a shape library built once at load by the system that owns
   its look: gravel and pebbles by `RockGenerator` (rock type, rounding by transport distance, the same
   sizes `RockField3D` uses so the size curve continues below `min_size`), leaves and twigs by
   `PlantGenerator`/`TreeGenerator` (the stand's species and season, real curled leaves instead of cards),
   mud clods and snow clumps by `GroundSurface3D` (SDF lumps meshed by surface nets). LODs by meshoptimizer.
2. **Deposit maps.** Each owner writes a per-tile density and size map per family that the fill reads:
   scree, till, shore and river bands from `RockField3D` (the grain size curves it already has), litter and
   twig amount from `VegetationField3D.get_litter_at`, clods on rut rims and clumps on snow from
   `GroundSurface3D`'s fields, wetness and snow cover from the weather map.
3. **GPU fill.** Square tiles round the camera per size class (each class to its own range above). A
   compute shader per tile places elements with blue-noise slots ranked by priority (Gravel), so density
   changes add or drop elements in place; positions sit on the ground's height including the near field,
   sunk by a third of their thickness; orientation from the ground normal and the deposit (imbricated in
   rivers, flat on beaches). Records go to MultiMeshes per (shape, LOD) through the existing GPU MultiMesh
   path. Nothing is stored: tiles are regenerated deterministically from the seed.
4. **Tiles of piles (Peytavie).** For undisturbed ground, 16 to 32 tiles per family are settled once by
   PhysX at load (elements dropped on a plane, Wang borders), instanced as whole tiles. Tiles on deformed
   ground break into single instances that follow the deformation.
5. **Engine: GPU-written TLAS entries.** A compute pass writes TLAS instance records for these MultiMeshes
   straight into a slot range (Unreal's `RayTracingInstanceBufferUtil`, Unity's `AddInstancesIndirect`),
   with per-instance culling by projected size; removes the readback lag and the CPU walk. Stage B of
   persistent-instances.md; partitioned TLAS later.
6. **Far hand-over.** Past each class's range the ground material takes the aggregate look (Gravel's proxy
   shading, Meng 2015): coverage, mean tilt, self-shadowed albedo, roughness of the elements it stops
   drawing, so the edge does not show. The existing rock detail normal becomes that far term.
7. **Interaction.** Wheels and feet already write the deform fields; elements in a changed tile sink or are
   shoved aside by the fill. Elements under a contact are promoted to PhysX rigid bodies (the PhysX module,
   as `break_stone` does) and dropped back into records when they sleep.

All on by default once built, with a fast path for tests (`ground_detail` off in benches that do not need
it).

## Order

1. Gravel and pebbles on shingle beaches and scree feet with per-element instances (existing GPU MultiMesh
   path). Raster and PT before/after pairs at 100% crop: the website pair.
2. Tiles of piles and GPU-written TLAS entries, so counts scale to 2160p.
3. Leaves and twigs (replacing near litter cards), mud clods, snow clumps.
4. Interaction and PhysX promotion.
5. Only if cluster acceleration structures land in the fork: unique per-element geometry as an NVIDIA-only
   option for hero spots.

## Sources

- Raad et al. 2025, CGF 44(2), doi:10.1111/cgf.70075; github.com/Andarael/resurfacing (CC BY-NC).
- github.com/michaelsalton/Gravel (README, 2026).
- Peytavie, Galin, Grosjean, Merillou 2009, CGF 28(7), "Procedural generation of rock piles using aperiodic tiling".
- van Muijden 2017, GDC, "GPU-based run-time procedural placement in Horizon Zero Dawn".
- Wohllaib 2021, GDC, "Procedural grass in Ghost of Tsushima".
- Epic, Nanite docs (5.7 and later), Nanite Foliage docs; Tom Looman, "Unreal Engine 5.7 performance highlights".
- nvpro-samples vk_tessellated_clusters, vk_animated_clusters, vk_raytrace_displacement (archived); NVIDIA blog,
  "RTX Mega Geometry now available with new Vulkan samples" (Feb 2025).
- Thonat et al. 2021, "Tessellation-free displacement mapping for ray tracing"; Hoetzlein 2025, CGF, doi:10.1111/cgf.70235.
- AMD GPUOpen 2025, "Ray tracing massive amounts of animated geometry using tetrahedral cages".
- Kuth et al. 2024, "Real-time procedural generation with GPU work graphs" (HPG).
- Meng, Papas, et al. 2015, "Multi-scale modeling and rendering of granular materials" (SIGGRAPH).
- Tokuyoshi and Kaplanyan 2021, "Stable geometric specular antialiasing with projected-space NDF filtering" (JCGT).
- In the fork: gpu-multimesh-records.md, instanced-forests.md, persistent-instances.md, open-world-geometry.md,
  displaced-hit-varyings.md, rocks.md, forest-floor.md, mud.md.

## Stage 1 as built (18 Nov): an experiment, off by default

Paulo picked the instanced approach as an experiment to assess by eye before anything reaches main
(branch `claude/project-thread-tmrbzz`).

- `RockField3D.ground_detail` (off): three more layers, `LAYER_DETAIL_PEBBLE` (4 cm to `min_size`, on talus to
  the scree layer's own smallest), `LAYER_DETAIL_GRAVEL` (2 to 4 cm), `LAYER_DETAIL_FINE` (1 to 2 cm), in 8, 4 and
  2 m cells round the camera.
- Amounts continue the big layers' own size laws below `min_size`: the shore's log-normal (sigma 0.35 round the
  beach-slope median, Bujan et al. 2019) and the talus's power law (exponent 2.5), so a coarse beach has few fine
  grains and a sand beach none. Each class's cover is held under about half the ground.
- Placement reuses the field's sequential deposition (Visscher and Bolsterli 1972) on a finer grid, seeded with the
  stones of the coarser layers already down, so grains rest against and around the big stones and never inside one.
  Cells are built coarse to fine for that.
- Reach: each class is drawn out to where its mean stone is `ground_detail_pixels` (3) tall, from the camera's field
  of view and `ground_detail_reference_height` (or the viewport's height). At 1080p: pebbles 32 m (capped by
  `ground_detail_max_radius`), gravel 11.7 m, fine gravel 5.8 m.
- Shapes are the field's own libraries (pebble and scree families), drawn from one of their LODs (320, 140 and 64
  triangles): a MultiMesh takes one LOD for the whole cell, so the grains carry theirs. The path tracer builds one
  BLAS per shape and one TLAS entry per stone (the expanded MultiMesh path); nothing is built per stone.
- Stones under 50 cm had a ground-blend band sized for a 50 cm stone, which covers a pebble whole. With
  `ground_detail` on, `small_linear` shrinks the band in proportion below 50 cm (unchanged above), so the pebbles and
  the grains share one dusted foot. Off, the shader is as before.
- GPU fill (the compute pass) is not in this stage: placement runs with the field's cells on the CPU, the same
  deterministic way. It comes with the GPU-written TLAS entries if the experiment is kept.

Bench: `docs/bench/rocks --world --view=shore|scree --ground-detail [--detail-ref=1080 --detail-pixels=3]`.
