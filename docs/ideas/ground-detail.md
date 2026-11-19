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

## Stage 1 as built (18 Nov)

Built as an experiment, assessed by eye on raster and path-traced pairs, then switched on by default (stage 2).

- `RockField3D.ground_detail`: three more layers, `LAYER_DETAIL_PEBBLE` (4 cm to `min_size`, on talus to
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
  BLAS per shape and one TLAS entry per stone (the expanded MultiMesh path, forced by
  `RenderingServer.multimesh_set_rt_instanced`: a few hundred grains of one shape fall under the merged-BLAS cap,
  which baked a copy of every grain and ran the 285k-stone scree out of memory).
- Stones under 50 cm had a ground-blend band sized for a 50 cm stone, which covers a pebble whole. With
  `ground_detail` on, `small_linear` shrinks the band in proportion below 50 cm (unchanged above), so the pebbles and
  the grains share one dusted foot. Off, the shader is as before.

Bench: `docs/bench/rocks --world --view=shore|scree [--no-ground-detail] [--detail-ref=1080 --detail-pixels=3]`.

## Stage 2 (18 Nov): on by default, placed on workers

Measured on the cloud's software Vulkan, so relative only (GPU milliseconds are on the PC queue):

| | without | with |
|---|---|---|
| Shore, path traced, memory | 5.13 GB | 5.29 GB (+101k stones) |
| Scree, path traced, memory | 5.47 GB | 5.63 GB (+228k stones; before the instanced fix it ran out of 15 GB) |
| Shore raster frame | 7.5 s | 12.7 s |
| Shore path-traced frame (16 spp) | 34.5 s | 49.7 s |
| Scree path-traced frame (16 spp) | 25.7 s | 88.9 s |

A detail cell took 15 to 58 ms on the main thread (placement two thirds, packing the records a third). Stage 2
follows the vegetation streaming: the main thread snapshots what a cell reads (the shape libraries and the coarser
stones its grains rest on), a worker places the grains and packs each shape's MultiMesh records, and the main thread
adds the cell with one buffer upload per shape (`ground_detail_threads`). Walking 6 m over the shingle beach
(`--walk=6`), the ground detail's main-thread share per frame went from mean 13.9 / worst 165 ms to mean 0.2 /
worst 1.4 ms, with the same stones. Detail cells are streamed ahead of the coarse layers' far cells (they starved
behind them before), each once the coarse cells round it are down. A GPU compute fill remains possible but no longer
buys main-thread time; the remaining walking spikes (p95 18 ms, worst 0.85 s) are the coarse layers' own cells, with or
without ground detail.

Next: leaf litter and twigs (vegetation), mud clods and snow clumps on the same pattern.

## Snow clumps (18 Nov)

`SnowClumps3D` (modules/snow), made by every `SnowSurface3D` (`clumps`, on by default). Where the print field's rim
grows, a share of the pushed volume (`break_share` 0.25) breaks into lumps if the pack is cohesive: settled
(density above about 150 kg/m3) or moist snow sinters into lumps (Colbeck 1982), cold new powder sprays and leaves
none (the trail bench's default 110 kg/m3 fresh snow gives almost none; `--settled` lays 300 kg/m3). Sizes
log-normal, median 2.2 cm plus 3 cm times wetness, sigma 0.5, 1 to 10 cm (avalanche debris granules, Bartelt and
McArdell 2009). Each is thrown up to 12 cm away from the print (down the dent's slope), sits 15% into the snow,
shrinks by degree days in a thaw (4 mm w.e./day/C), is buried by new snow and crushed by a later print. Eight
shared lumpy icospheres (320 triangles) with a fracture face, each clump stretched its own way (0.75 to 1.2 per
axis) so the shapes do not read as copies; one rt_instanced MultiMesh per shape.

Cost (lavapipe, trail bench, `--settled`, about 5,000 clumps lying): main thread 1.5 ms mean, 2.1 ms worst per
update at 6 Hz (about 0.25 ms a frame); 48 bytes per clump. Bench: docs/bench/snow
`--view=trail --settled --clumps-close [--no-clumps]`. Follow-ups: the scan on a worker; cornice debris
(`SnowCover3D.cornice_failed`) as large blocks.

## Mud clods (18 Nov)

`GroundClods3D` (modules/ground), made by every `GroundSurface3D` (`clods`, on by default). Two sources:

- Thrown clay that lands (`track_mud` with no print, from MudSpray's MPM grains): the surface keeps a per-frame drop
  list (`take_clod_drops`, dropped after two physics frames when nobody takes it); a landing holding a lump's
  volume stands up as one to three clods instead of lying only as the film the field draws.
- Lumps torn out of a rim where a tyre or a boot heaves sticky soil: the print field's rim is scanned round the camera
  at 6 Hz as snow clumps do; the share torn out (`break_share` 0.2) follows the soil's stickiness (above 0.3, as
  MudSpray's "mud"): plastic clay holds together, sand crumbles and slurry flows.

Sizes log-normal, median 2.5 cm, sigma 0.55, 0.8 to 9 cm (soil aggregates, Gardner 1956). Eight angular shapes (an
icosphere with two or three fracture planes: clay breaks along planes, Dexter 1988), each clod stretched its own way
and in a shade of its own of the clay's colour (`get_mud_color_at`, the surface's `mud_color` where the map holds
none), sent as the instance colour (linear). Crushed by a later print, slaked by water standing over it (Emerson
1967). One rt_instanced MultiMesh per shape.

Cost (lavapipe, ground trail bench on sticky soil, about 2,500 clods lying): main thread 1.4 ms mean, 3 to 5 ms worst
per update at 6 Hz (about 0.25 ms a frame); 64 bytes per clod. The ground bench needs the PhysX build; the cloud
check ran its trail view with moisture 0.4 soil (`--ground=clay` locally) and `--prop=clods:false` for the A/B.
Follow-ups: the scan on a worker (shared with snow clumps); clods kept with kept ruts (a GroundCellStore file);
drying (lighter, crumbling) and rain slaking them over hours.

## Lumps: shared scan and drawing (18 Nov)

Snow clumps and mud clods share `GroundLumpScan` (modules/ground/ground_lump_scan.h: the print field's rim and dent
in a window round the camera, read from the field's texels directly; which cells grew a rim, which changed at all,
so the crush check samples only lumps under a changed cell) and `GroundLumpDraw` (one MultiMesh per shape with
room by doubling and `visible_instance_count`, so a lump more or less re-uploads a buffer instead of reallocating
the MultiMesh and its ray tracing entries). Main thread per update at 6 Hz (lavapipe, same benches, images
identical): clumps 1.5 to 0.9 ms mean, clods 1.4 to 0.8 ms mean and 7.7 to 1.6 ms worst.

## Lumps kept while away (18 Nov)

A lump that leaves the deformed area round the camera (4 m past its edge) is parked in `GroundLumpStore`
(modules/ground/ground_lump_store.h: lumps per 16 m tile, position, basis, size, colour, shape) instead of dropped,
and taken back when its tile is near again, after a check that the ground under it still has the relief it lay on.
Clods also go to disk beside the kept ruts (`lumps_clods_x_z.lmp`, zstd) when `GroundSurface3D` keeps ruts, so they
outlive the area streaming out like the ruts do. The scan skips cells outside the field's rect and gives cells newly
inside it a few looks to settle, so a returning field is not read as fresh rims. Snow bench `--away` (field taken
300 m away and back): 4,172 lying, 4,172 kept while away, 4,129 lying again where they were (the rest crushed by
the relief check), no new ones broken off. Cost unchanged: clumps 0.78 ms mean, clods 0.88 ms mean per update.

## Coarse rock cells on workers (19 Nov)

`RockField3D.cell_threads` (on): while streaming, the coarse layers' cells (field stones, scree, river and shore stones,
landforms) are placed on worker threads as the detail cells are. A worker places the cell and packs each shape's
MultiMesh records, looking shapes up under a lock; a shape not built yet sends the cell back to be placed once it is
in. The main thread takes out stones on holes and broken ones, uploads one buffer per shape and adds the collision.
Landform cells stay on the main thread with `cave_mouths` on (they open terrain holes). Rocks bench `--world --walk=120
--walk-step=0.2` (lavapipe, drawing off), main thread per frame, `--cell-sync` then on: mean 5.2 / 0.6 ms, p95 13.6 /
0.3 ms, worst 1,020 / 359 ms; the same 120,430 stones once settled (cells land about 8 s after a 12 m/s walk stops on
this machine, nearest first).

## Close-up bark (plan, 19 Nov)

LOD 0 trunks already carry bark relief as displaced geometry (TreeGenerator `bark_relief`, ten `BarkKind` families), so
the furrows are only as sharp as the tube's rings and segments, and nothing stands proud of the surface: no loose
plate edges, lifting flakes or curls of paper bark. Close up those are what reads as bark.

Established technique:
- Bark as elements on a surface (Pixar, "Elemental Bark", Hery et al.): plates, scales and strips modelled as
  separate pieces laid over the trunk, not one displacement, so they can overlap, lift and cast their own shadows.
- Bark synthesis from fracture of a growing skin (Lefebvre and Neyret 2002, "Synthesizing Bark"; Wang et al. 2003,
  Tsinghua): the plate layout comes from the trunk's own pattern, which here is the BarkSet height image.
- Game practice: SpeedTree and UE5 trunks pair a displaced mesh (Nanite tessellation) with decal or mesh pieces for
  peeling bark; the pieces are where close shots spend geometry.

Plan, in `modules/vegetation/tree_bark_detail.*` (BarkDetail3D), the ground detail pattern on trunks:
- Trunks near the camera from `VegetationField3D.get_trunks_near(center, radius, height)` (c027d1ed): each trunk's
  capsules up to a few metres, its preset and seed.
- Per bark kind (`TreeGenerator.bark_kind_for_preset`, 16f1f982), elements placed where `bark_height` says the
  surface stands: plated and pinaster/pinea plates on plate tops (0 height), their edges lifted over the furrow;
  flaking: thin flakes peeling at one edge; papery: curls of birch skin rolled outward; scaly: small overlapping
  scales; furrowed and cork keep their geometry (already deep), with a few broken ridge fragments; smooth and
  lenticelled get none.
- Each element one of 8 shapes per kind with its own random bend, size and tint (no visible repetition), drawn as
  rt_instanced MultiMeshes so the path tracer sees each piece over a shared shape; reach by the pixel rule (3 px).
- Placed on workers like the detail cells; cost reported on and off (memory, walking main thread, raster and PT frame).

## Close-up bark as built (19 Nov)

`BarkDetail3D` (modules/vegetation/tree_bark_detail.*), bench `docs/bench/bark` (`--test`, `--raster`, `--look`).
- Pieces per bark family: plates (Scots, stone and maritime pine), scales (holm oak), flakes (flaking), torn
  curls (paper birch, and short ones on lenticelled bark); smooth, furrowed and cork shed none. 8 shapes per family,
  each piece its own size, spin, tone and texture offset.
- Where they lie: placing pieces on the capsule radius plus `bark_height` put them inside the trunk (the LOD 0 tube
  stands proud of its colliders), so each trunk mesh gets a profile: its bark surface binned 96 round by 2.5 cm
  along its capsule chain, the outermost bark vertex and mean vertex colour per cell. Pieces sit at that radius
  and take that colour (the stem's vertex colour is its species tint; without it pieces rendered white). Plates and
  scales only go where the cell is in the top third of the bark round it (plate tops).
- The profile from the mesh's own arrays needs a GPU readback (`mesh_get_surface`), 200 ms or more a mesh on the
  cloud renderer. `BarkDetail3D.make_bark_profile` packs it for a `tree_bark_profile` mesh meta (about 110 KB a
  mesh, the first 4.2 m) that TreeGenerator stores while it still has the arrays; without the meta the readback
  is the fallback and the binning runs on the worker.
- Review round (docs, 19 Nov): birch pieces read as flat tan stickers and pine plates as orange cards at the
  silhouette. Now: birch strips are torn horizontal bands (ragged edges, narrowing ends) whose middle stays on the
  stem and whose ends stand up off it and roll back into scrolls, white side out, the peeled plies cream-salmon;
  pine plates are twice as thick (1.6 to 3 cm) and sunk into the relief, and broken edges are the outer bark in
  section (darkened, a trace of inner bark) instead of inner bark, which now takes the stem's hue.
- Second review (docs, 19 Nov): birch read as evenly scattered confetti, two pieces floated at a fork, pieces
  piled up; pine pieces lay across the furrows like shards and stood out of the silhouette like a saw edge. Now:
  - Pine and scales: the trunk's own plates are found in its profile (cells above the bark round them, eroded once
    so furrows split them, flood-filled), and a share of them shed their outer layer: a piece fitted inside that
    plate's bounds (scaled round and up to it), lying on it and lifting at its lower edge, so none crosses a furrow.
  - Birch: bands along the lenticels every 3 to 12 cm, each a torn bared patch of inner bark (bent to the stem's
    own radius) with one to five strips side by side at its ends: long strips peeling at both ends, wide sheets,
    loose flaps lifting like a page, scrolls; sizes log-normal.
  - The profile drops cells far proud of their row (another stem at a fork), and every piece claims its cells, so
    none lands on another.
- Third review (docs, 19 Nov), birch: pieces read as flat tan tape, stuck out past the silhouette as tabs, and sat
  on one side of one stem. Now: every stem of a forked trunk gets a profile (each further collider chain that
  leaves the main stem below the cap, 6 cm thick or more and heading up), each stem's scan keeps only its own bark
  (a vertex nearer another stem, in that stem's radii, is that stem's) and drops cells far off its row, and no
  piece goes nearer another stem than its own. Peel is white outside (no salmon tint on the outer face), plies
  1.4 mm thick, lifting steeply into a 1 cm scroll that hugs the stem; bared patches show the next ply (half
  white, half salmon), at most a quarter of the way round and bent to the stem's own radius.
- Fourth review (docs, 19 Nov), birch: a flat white sheet stood across the crotch, tabs poked past the silhouette,
  pieces ran up the leaning stem as dark sticks, and thin vertical brown slivers read as cracks. Now:
  - Sheets are turned a quarter round so their roll lies round the stem along the band (it tilts with a leaning
    stem); their texture is turned back with them, so the lenticels on a sheet run round the stem like the stem's
    own (the slivers were the texture's lenticels standing upright). Most of a sheet lifts (its attached part is
    the stem's own bark, so little is drawn over it), at 45 to 60 degrees rather than upright.
  - Bands every 5 to 14 cm, each with one to three bared patches round it whose torn top and bottom edges are
    rolled sheets; some bands only have sheets lifting.
  - A fork's further stems: the capsules follow a leaning stem only roughly (its mesh sat up to a radius off its
    chain, so its bark was binned lopsided and half of it read as hollow, few pieces landed there). Each of their
    capsules is moved to the centre of its own bark before binning (the shift is stored in the meta's header);
    the main stem keeps its chain, since its branches would pull it. Cells still empty take the nearest filled
    rows of their column.
  - Sheets and patches lie at the median radius of the cells they cover, bent round to the stem's radius (the
    shape's 12 cm bend rescaled), and skip spots too uneven to lie on (a collar, a fork's swell), so none stands
    off past the silhouette.
- Cost (bench, 4 trunks, 1,045 pieces): main thread 0.15 ms mean, 3.4 ms worst an update with the meta (33 ms
  mean, 826 ms worst with the readback); 64 bytes a piece in instance buffers (67 KB), 32 shared shapes.
