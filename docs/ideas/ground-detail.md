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
- Fourth review passed (docs, 19 Nov), two polish notes: some rolls read as thick rounded lips like small bracket
  fungi, and the bands stacked in one column up the main stem like a ladder. Now plies are about 1 mm with a 4 to
  7 mm scroll, sheets at most 1.3 times their shape and never thickened when bent to the stem; each band's patches
  start a golden angle (137 degrees, jittered) round from the last band's and spread evenly round it; bands every
  4 to 10 cm.
- Cost (bench, 4 trunks, 1,045 pieces): main thread 0.15 ms mean, 3.4 ms worst an update with the meta (33 ms
  mean, 826 ms worst with the readback); 64 bytes a piece in instance buffers (67 KB), 32 shared shapes.

## Next round options (19 Nov)

Litter (vegetation), mud clods, snow clumps and close-up bark are done; the open items of the plan above are 2, 4 and
the far hand-over (proposal 6). Four candidates, put to Paulo as a decision card:

1. **Seamless far edge (recommended).** Past each class's reach the ground material takes the aggregate look of the
   grains it stops drawing, so the edge never shows. Technique: Meng et al. 2015, "Multi-scale modeling and
   rendering of granular materials" (SIGGRAPH): explicit grains near, a volumetric/BSDF aggregate far, the switch by
   projected grain size. A real-time cut of it: per class, tabulate once from the shape library (offline, from the
   rocks' own LODs) coverage, mean tilt, a self-shadowing term over sun elevation and the mean albedo and
   roughness of a lit grain bed; the terrain shader blends rock detail toward those values by distance with a
   dithered band one cell wide, and the far normal is the existing Worley rock detail. Cost: one small table and a
   few ALU in the terrain shader. Check by a raster/PT pair straddling the reach at 100% crop.
2. **Stones that react.** Wheels and feet already write the deform fields; a changed cell sinks or shoves its grains
   (displacement along the deform gradient), and grains under a contact patch become PhysX rigid bodies (the
   `break_stone` path), back to records once asleep. Only grains near a contact need bodies: tens at a time.
3. **River and stream beds.** Imbrication: river pebbles lie overlapping like roof tiles, the long axis across the
   flow and the upstream edge down, dip 10 to 30 degrees (standard sedimentology; the flow direction comes from the
   water's own flow map). Size from the stream's shear (coarse in riffles, fine and silted in pools); seen through
   clear water, so it needs Valence's water to trace them.
4. **More stones for the same cost.** Peytavie et al.'s tiles of piles (settled once by PhysX, Wang borders) and a
   compute pass writing TLAS instance records straight into a slot range (Unity 6
   `RayTracingAccelerationStructure.AddInstancesIndirect`, Unreal's GPU instance buffer path), with per-instance
   culling by projected size. Needs a cost run on the PC to prove the gain.

Sources: Meng, Papas, Habel, Dachsbacher, Marschner, Gross, Jarosz 2015
(https://cs.dartmouth.edu/~wjarosz/publications/meng15granular.html); Unity
`AddInstancesIndirect` (https://docs.unity3d.com/ScriptReference/Rendering.RayTracingAccelerationStructure.AddInstancesIndirect.html).

## Seamless far edge as built (19 Nov)

Picked first of the four (Paulo, 19 Nov: all of them, in order). The drawn classes stop at their reach (pebbles 32 m,
gravel 12 m, fine gravel 6 m at 720p), and past it the ground showed plain sand: a line across every beach and talus.

- **Cover maps (RockField3D).** Two camera-centred RGBA8 maps, 256 texels each, at 1 m (256 m) and 8 m (2 km) texels:
  R, G, B each class's cover of the ground (0 to 0.5), A the rock type. Filled from the same laws the detail cells
  place by (the shore's log-normal from the beach slope above the water line, the talus power law), on a worker when
  the camera has moved an eighth of a map; `build_area` fills them at once. `ground_detail_cover` (on by default),
  `get_ground_detail_cover_texture(level)`. A bench check (`--cover-check`) compares the map with the stones actually
  placed per 4 m square: no square with placed stones the map misses.
- **Aggregate (TerrainMaterial3D, `set_ground_cover`).** Meng et al. 2015's split, cut down to a few ALU: past each
  class's reach (blended over its cell size) the ground draws single hashed grains (domed normal, darker at the
  foot) while a pixel spans under a quarter of a grain, and their mean by the time it spans most of one. The mean is
  Poisson cover of grains standing about 0.6 of their width tall: the share of the view they fill grows with the
  tangent of the view angle (edge on they hide the bed), the bed they leave is in their shade by the tangent of the
  sun's, and edge on or against a low sun the camera sees their shaded sides. Colour: the rock type's linear albedo as
  the stones' shader mixes it (mineral, specks, rind), half dusted to the ground's. Grains hide the bed's sheen
  (specular off, roughness 0.72 after the wet sand's gloss). Patch noise at two octaves below the map's metre keeps it
  from an even smear (no visible repetition).
- **Calibration.** Against frames with every stone drawn (`--detail-pixels=0.75`), mean brightness of the hand-over
  area: at eye height 2074 before, 1639 every stone, 1711 aggregate; from 11 m up 141, 132, 126. Constants: view
  cover 0.8 cov (1 + 0.75 tan), bed shade 0.2 cov tan(sun). Polish after the docs check: the edge where cover thins is cut by two noise octaves (0.6 and 1.7 per m) so it breaks up, a third octave (5.3 per m) clusters the mean lighter and darker, and the grains stay single until a pixel spans more than one (footprint taken as the mean across and along the view).
- **Path tracing.** All of it is in the ground material, which the path tracer runs as is; the sun direction comes
  from `relief_sun_direction()` and the grain switch from the ray-cone footprint, so raster and PT pick the same
  grains.
- **Cost.** One 256 x 256 fill on a worker per eighth-of-a-map move, two texture reads and up
  to three hashed grain tests per ground pixel past the reach. No new instances.
- **Left.** Very sparse scattered stones (cover under 0.01) vanish in the aggregate where the drawn frame shows a few
  dark specks; any other ground layer (Terrain3D) needs the same uniforms wired.

## River and stream beds (19 Nov)

The river layer already lays cobbles by Shields' threshold and Sternberg's fining, imbricated; below `min_size` the
beds were bare ground, so a gravel-bed stream (most of them in a 2 km world: channels about a metre wide) showed no
stones at all. Ground detail now carries the river layer's bed on below it, as it does the shore's.

- **Technique.** The bed's median from the river layer (same formulas, shared per segment: `_river_segments`), then
  the pool-riffle sequence: riffles coarse and pools fine, about six channel widths apart (Leopold, Wolman and Miller
  1964; Keller and Melhorn 1978: 5 to 7 widths), median x (0.5 + 0.9 riffle), and finer on the bars at the sides than
  down the thalweg (x 0.65 at the banks). The pools' slack water drapes fines over the bed: their grains' cover drops
  by up to three quarters and they sit deeper. Sizes from a log-normal of sigma 0.6 (poorly sorted at the fine end,
  as the shore's), cover as the shore's (weight share, clast-supported 0.85).
- **Imbrication.** Every grain's long axis across the flow, its a-b plane dipping upstream 10 to 30 degrees (Rust
  1972; the stacked "roof tile" fabric of a flowing gravel bed), as the river layer's cobbles. Wet film on.
- **Rock.** 70% of grains carry the headwaters' rock, as the cobbles do.
- **Far cover.** The same beds go into the far hand-over's cover maps (rasterised per segment; a bed narrower than a
  texel is spread over it, thinner by as much), so a stream's gravel continues past the drawn reach.
- **Switch.** `RockField3D.ground_detail_rivers` (on). Bench: `--view=river --gravel-reach[=d]` picks an open
  gravel-bed reach whose median is nearest d (0.03 m); `--no-river-detail` for the before frame.
- **Through clear water.** The grains are ordinary detail instances, so the water's own transmission shows them; the
  check needs river water (PhysXRiver3D), which the cloud build here lacks (no PhysX SDK in this container): on the
  PC queue, or with a PhysX cloud build.

## Stones that react (19 Nov)

- **Technique.** An active zone round each contact, as in Servin, Berglund, Nordfeldt and Lacoursière's multiscale
  terrain for earthmoving (2021, https://www.algoryx.se/papers/terrain/): the bed is records, and only where a tool
  disturbs it does it turn into discrete particles, merged back once at rest. Here the tool is any `world_contact`
  body (`WorldContracts.describe_body`: feet, wheels, balls, the same list grass, snow and the ground read).
- **What a contact does.** Each physics frame, for each moving body (over 0.15 m/s; a body at rest leaves things as
  they are), the stones under its footprint with no collision of their own (ground detail, and the small shore,
  river and scree stones; never boulders over 0.3 m), each picked by a hash so it gets the same answer every frame:
  - 60% pressed into the bed (centre down to 0.15 of its size over the ground; idempotent);
  - 25% flicked: a rigid body on the physics server, a convex shape from the stone's own hull, mass from its size
    (2650 kg/m3, a third of its sphere), pushed with 0.6 of the body's speed, out of the footprint and a little up,
    spinning; ground collision only (layer 0: a vehicle is not shaken by gravel); up to `react_max_bodies` (32), and
    only from 1.2 cm across;
  - the rest (and flicks past the cap or too small) squeezed sideways to the footprint's edge, their place along the
    way kept, into a low berm.
  A first try that cleared every stone from the footprint read as a swept channel; a rolling contact mostly presses.
- **Prints and ruts (docs check, 19 Nov).** A ball in a straight line read as a painted stripe. Now: pressed in
  the middle of the print (across the body's line when it moves along, so a stone met at the leading edge still counts
  as in the middle), pushed out at the edge into a rim (a little further out for some, lying on the stones there,
  higher by up to most of their size). Pressed stones lie flat (yaw kept) and every disturbed stone shows a damp
  underside (water film 0.85) and drops its ground blend. Where a deformable ground lies under the stones
  (`WorldGround.height_at`, GroundSurface3D's sand or mud), a pressed stone sits in the dent that ground took: the
  print is a real depression. Bench `--tracks`: a walker (boots coming down every 0.45 s, alternating) and a
  two-wheeled cart on 6 and 8 m bends; `--tracks-verbose`.
- **Prints and ruts, second docs check (19 Nov).** Prints read as pale rings and ruts as pale sand strips, with fine
  stripes over the frame. The causes:
  - The terrain material opened a print to the bare soil colour *after* the stones' damp-soil tone
    (`rock_contact_map`) and gave it back its full sheen, so a print in gravel showed pale dry soil between the
    stones. The soil now opens before the damp tone, and its sheen keeps the ground cover's share
    (`0.5 * tm_gc_spec`).
  - The bench's sand kept its ruts between runs (`GroundSurface3D.keep_ruts` to `user://ground_ruts`), so before
    frames had old prints. The bench turns it off.
  - The stripes are the sand's wind ripples (`GroundSurface3D.wind_ripples`) beating against the pixel grid on a
    gravel-covered beach; gone with `--gs-prop=wind_ripples:false`. The sand's owner is told.
  - The bench's boots were weightless and never dented the sand. They now sink 2.5 cm, and the stones under a
    standing body keep following its print down (press only: no shove or flick at rest). A pressed stone sits
    0.1 of its size under the dent's surface (bedded, its top flat). The disturbed film is 0.4 (0.85 made a pebble
    on a boulder read as a hole).
- **Drawing.** A loose stone stays an instance of its own MultiMesh; each frame its instance transform is the body's,
  so raster and the path tracer draw it exactly as before (no node swap, no separate mesh). At rest (asleep, or
  under 5 cm/s for 0.4 s, or 8 s old, or through the ground) its body is freed and the record keeps where it lies.
- **Switch.** `RockField3D.stones_react` (on), `react_max_bodies`, `get_react_stats()`. Bench `--roll[=m/s]`: a 0.3 m,
  40 kg ball rolled 8 m across the spot (terrain collision on), taken away before the still; `--no-react`.
- **Not yet.** Stones that leave their cell's bounds stay in that cell (drawn, but streamed out with it).

## More stones for the same cost: what is left to gain (19 Nov)

Before building tiles of piles or GPU-written TLAS records, what the fork already does:

- **TLAS records already persist on the GPU.** Ray tracing keeps persistent instance slots
  (`render_raytracing_persistent.h`) and keyed TLAS builds (`RD::TLASBuild::keyed`): records live in a GPU buffer
  between builds and only changed ones are written. A static grain costs no CPU per frame after its first; a moved one
  (stones that react) one record. A compute pass writing records (Unity 6 `AddInstancesIndirect`, Unreal's GPU
  instance path) would save the first write of a streamed cell and the CPU expansion of a MultiMesh into rows. That
  is worth having only if the PC shows cell streaming spikes in the RT update, which nothing here has shown yet.
- **Tiles of piles (Peytavie, Galin, Grosjean and Mérillou 2009, aperiodic rock-pile tiles; corner-coloured Wang
  tiles, Lagae and Dutré 2006).** One BLAS per tile of settled grains, instanced: TLAS entries drop by the grains per
  tile (a 0.5 m fine-gravel tile at full cover holds about 700 grains), but each tile BLAS is its grains' triangles
  baked in (700 x 64 = 45 k triangles; 16 corner tiles x 2 rock types x 3 cover levels about 4 M triangles, some
  250 MB of BLAS). The top-level traversal cost grows with log N, so 100 k entries against 150 is a few levels of the
  tree, and a TLAS rebuild of 100 k instances is about a millisecond on current GPUs. Against that: the grains of a
  tile can no longer react one by one (a tile under a contact must be split back into instances), cover becomes three
  levels instead of continuous, and the tiles' repeats must be hidden (Paulo's rule: no visible repetition).
- **So:** measure first. The PC cost bench (rocks bench, shore eye view, path traced, RENDER_INFO_RT_TLAS_INSTANCES and
  GPU frame time with ground detail on and off) tells whether the TLAS is worth shrinking at all. If the detail's
  share of the frame is small, item 4 is not worth its memory and repetition risk.

## Tiles of piles: the build (Paulo, 19 Nov: "build the tiles then")

Latest practice, checked before building:

- **Moana on the GPU (Maven Kim, Nov 2025).** About 23 M singly instanced pebbles, flowers and sand grains: a TLAS over
  them would not fit 8 GB. Small instances go into partitions of under 1024 and whole partitions are switched off by
  screen size (partitioned TLAS).
- **Clustered geometry and partitioned TLAS in DXR (Microsoft, Oct 2026 preview; NVIDIA RTX Mega Geometry).** Cluster
  BLAS share cluster pieces; instances still reference whole BLAS. Their sample: AS memory 7.5 GB to 0.8 GB and
  trace 72.6 to 9.3 ms, "do not assume games will see this". Trace speed is a little worse on clusters (Remedy: about
  0.5 ms in Alan Wake 2), so static geometry stays on triangle BLAS. Not in the fork (no Vulkan cluster path yet).
- **Unreal 5.7+ Nanite assemblies.** A mesh of instanced parts, one level deep, up to 65 k parts, "a large array of
  matrices". Off Nanite they are merged into one mesh. Variation comes from authoring, not from the runtime.

So in the fork: a tile is one BLAS of baked grains, shared by every placement (the merged MultiMesh BLAS is already
cached per MultiMesh, `_shared_merged_mm_handle`, and each RS instance over it is one TLAS entry). What it takes:

- **Tiles.** Per detail class and rock library, a set of tiles of settled grains (one MultiMesh per shape in the tile,
  the same randoms as the cells), several cover levels. Corner-coloured Wang tiles so neighbours never repeat an
  edge; each placement turned by a quarter turn and mirrored at random (eight looks per tile); per-stone tone and size
  stay per grain.
- **Placement.** Each tile placement sits on the ground's plane at its centre (tilted to it). Where the ground bends
  more than a grain's height over the tile (scree, gullies, the shore's step) and on cover between levels, the cell
  keeps single grains, so the tiles never float or sink and the cover stays continuous; a noise mask decides which
  tiles show.
- **Shading.** The rock shader eases each grain into the ground with its own ground height (INSTANCE_CUSTOM.x, world)
  and slope; a tile's grains share one record set, so a tile placement gives the ground as a plane per placement
  (an instance uniform). Path tracing reads instance uniforms per TLAS entry, so both renderers agree.
- **Path tracer, merged MultiMeshes.** In a merged BLAS the hit shader's MODEL_MATRIX is the node's, not the grain's
  (raster's is node times grain), so per-stone origin uses (uphill fines band, rivulets) differ from raster for every
  merged MultiMesh today. The hit shader reads the grain's transform from the instance record and runs vertex() in
  the grain's own space, as raster does.
- **Contacts.** A tile under a contact is split back into single grains for that cell before stones react.

### As built (19 Nov)

- `RockField3D.ground_detail_tiles` (on since the rock shader took `rock_tile_plane`, rocks 029e0566). Each detail cell is
  cut into 4 x 4 squares. A square takes a tile when it has 8 or more grains on the ground, no hole, no coarse stone
  (over four times its grains) reaching in, and ground within 0.35 of its grains' scale of a plane. Up to four tiles
  are cut per cell from its own squares (in a hashed order), and a square takes the first tile matching its rocks,
  grain count (0.85 to 1.18), mean log size (0.12) and wetness (0.15), at most six uses, a random quarter turn. A
  tile used once is dropped (its square keeps its grains). Grains lifted onto a coarser grain stay single. No Wang
  edges are needed: grain centres are independent of the square's edges and grains cross them freely, so the seams
  have no gap; the variety is the cell's own grains, cut afresh per cell.
- The placed square's grains are replaced by the tile's (moved onto its plane), so the cell's tiled groups (no
  MultiMesh) hold what is drawn, for the finer layers' seeds and for contacts. A contact within the foot plus 0.3 m
  of a placement splits it into single grains (one rt-instanced MultiMesh per shape), then reacts as before.
- Path tracer: merged MultiMeshes now shade with the instance's own transform (`RT_GEOM_FLAG_INSTANCE_XFORM`,
  `fetch_instance_transform`), so vertex() sees what raster does (MODEL_MATRIX = node x instance, VERTEX in the
  mesh's space). This was off for every merged MultiMesh before (per-stone origin read as the node's).
- Shore view, cloud, raster: 2,320 squares; 1,469 too sparse, 154 by coarse stones, 164 bent, 58 unmatched; 412
  placements of 118 tiles draw 68,819 of 116,200 grains with 2,467 rendering instances; 2.6 M triangles baked in tile
  BLAS. Top-down path traced (before the tolerance went 0.2 to 0.35): TLAS 73,866 to 59,319 (most tiled grains are
  past the near ranges the TLAS holds). Top and eye-level frames match the single-grain ones (mean colour within
  0.4 of 255), no seams or repeats seen. The walker and the cart split 90 placements.
- Left: tiles shared across cells (one set per class and rock), which would cut the baked triangles; the PC cost run
  (GPU trace time with and without, `--tiles` / `--no-tiles`).
- On (19 Nov, with `rock_tile_plane`): shore top view path traced at 640, TLAS 73,848 without tiles to 47,324 with
  (36 % fewer), mean colour within 1 of 255 per channel; raster eye level 1.5 % brighter low in the frame (the tile
  plane's blend band where the plane sits a little over the ground).

## Tiles shared across cells (round 4, 19 Nov)

Paulo picked three follow-ups in order: share tiles across cells, tiles on bumpy ground, drop sub-pixel stones in ray
tracing. This is the first.

**Today.** Each detail cell cuts up to four tiles from its own squares and drops any tile used only once. On the
shore view that is about 115 tiles for about 400 placements: 2.5 M triangles baked into tile BLAS (about 150 MB), and
roughly a third of the eligible squares stay single because their cell had no matching cut left.

**How others do it.**
- Peytavie, Galin, Grosjean, Merillou 2009, *Procedural Generation of Rock Piles using Aperiodic Tiling*: a small
  precomputed set of pile tiles reused over the whole terrain. The tile set is built once per pile type, never per
  area, and variety comes from the set size and the aperiodic choice.
- Lagae and Dutre (tile-based methods, Wang and corner tiles): a few tiles per class, chosen per square by a hash, so
  the choice depends only on where the square is.
- Instancing in ray tracing (Moana on the GPU, Kim 2025; DXR clustered geometry and partitioned TLAS preview, Oct
  2026; UE Nanite assemblies; NVIDIA vk_partitioned_tlas sample): build each template's BLAS once and keep many
  instances of it. Memory goes with the number of templates, not the number of uses.

**As built (19 Nov).** The first design (one anchor cell per 4 x 4 block) gave no saving: the anchor often sat off
the shore band (no pebble tiles at all), and each block's tiles went unshared outside the few cells in view. What
works:
- One set per block of 16 x 16 detail cells (128, 64 and 32 m for pebble, gravel and fine).
- Stratified sampling: the block in 16 x 16 parts, one square from each, the first of eight hashed tries there with
  grains to tile, each placed on its own without the coarse stones round it. A pure function of the block, the seed and
  the ground, so streaming order never changes the tiles and raster and path-traced runs match.
- Thinned to a cover: a sample is kept only while fewer than three kept ones match it, so each kind of ground has a
  few tiles.
- Matching uses what a square already varies by chance: grain count within 1.5 Poisson standard errors (never under
  15%), mean log size within 1.5 standard errors of its spread (never under 0.12), rock shares within 1.5 binomial
  errors (never under 0.08), wetness within 0.15.
- A square picks by its hash from its first two matches, past them only when a neighbour has one, at most six times a
  cell; squares with no match still cut their own (now 3 to 6 tiles in view instead of about 115).
- Each candidate's MultiMesh and BLAS are built when a placement first takes it.

Shore view, 1280 x 720 top: before 115 tiles, 2.5 M triangles baked, 66 to 69 k grains drawn by tiles. Now about 70
tiles (66 shared, 3 own), 1.83 M triangles, 82 k grains drawn by tiles (71% of the field), 515 placements. Tile BLAS
memory down about 27% while a fifth more ground is tiled; per tiled grain about 40% less. The colour and density
match the untiled field closely (a rock-share test keeps mixed squares' colours).

**Design (first version, superseded above).**
- One tile set per block of 4 x 4 detail cells (32, 16 and 8 m for pebble, gravel and fine). It is cut from one anchor
  cell, picked by a hash of the block, so it carries the block's own rocks, sizes and wetness.
- The anchor is placed without the coarse stones round it (they only shape grains near those stones, and tile squares
  stay clear of them). That makes the set a pure function of the block, the seed and the ground, the same whatever
  order cells stream in, so raster and path-traced runs show the same tiles.
- Every eligible anchor square becomes a candidate (up to 16). A square of any cell in the block takes a matching
  candidate (same rocks, count within 0.85 to 1.18, log size within 0.12, wetness within 0.15) chosen by its own hash,
  never the same one as its left or upper neighbour, at most four times per cell, turned by a random quarter turn.
- Squares that match no shared candidate fall back to today's own cuts.
- A candidate's MultiMesh and BLAS are built only when a placement first uses it, and freed with the set when the last
  cell of the block leaves.
- The first cell to need a block's set computes it on its worker; the block's other cells wait for it rather than
  repeat it.

**Expected.** Up to 16 shared tiles per block instead of up to 64 own cuts, and each cell has up to 16 candidates
instead of 4, so more squares take a tile and each cell shows more different tiles than today.

## Tiles that follow bumpy ground (round 4, 19 Nov)

**Measured.** Scree view: of about 3,200 squares looked at, 1,000 are skipped for a coarse stone inside them and 500
for bent ground, and no square was tiled at all. River view: 1,650 of 3,900 skipped as bent, 50 placements. The shore
(flat) tiles 71% of its grains.

**How others do it.**
- Chunked LOD and quadtree terrain (Ulrich 2002, *Rendering Massive Terrains using Chunked Level of Detail Control*;
  Losasso and Hoppe 2004 geometry clipmaps): a patch is split while its geometric error (how far the real surface
  is from the flat patch) is over a tolerance. The error of a flat patch on a curved surface falls with the square of
  its size, so one split cuts it four times.
- Recursive Wang tiles (Kopf, Cohen-Or, Deussen, Lischinski 2006, *Recursive Wang Tiles for Real-Time Blue Noise*):
  a tile set at several scales, each level's tiles drawn from the same distribution, so a split region keeps the same
  statistics.
- Peytavie et al. 2009 place their pile tiles over the terrain as rigid tiles; their scenes stay on gentle slopes.
- A shared tile cannot bend per placement under path tracing (its BLAS is shared and rigid), so a bent square has to be
  covered by smaller flat tiles, not a warped one.

**Design.**
- A square skipped for bent ground or for a coarse stone splits once into 2 x 2 half squares (0.25, 0.5 and 1 m for
  fine, gravel and pebble), each tested again with its own plane, flatness and coarse-stone check, with at least 8
  grains. The flatness tolerance is per grain size as before, but the error over a half square is about a quarter.
- Half squares have their own tiles: each shared-set sample also gives its four halves as candidates (no extra
  placement), thinned per level the same way. A half square only matches half-square tiles.
- Neighbours are any squares or half squares that touch, so the never-a-neighbour's-tile rule holds across levels.

**As built (20 Nov): tried, measured, not kept.** Splitting bent or crowded squares into halves with their own tile
sets worked as designed (scree 50 to 266 placements, 6.5 k to 14 k grains drawn by tiles), but the halves rarely
repeat: talus and river beds change size and mix every few metres, so each half tile was used about 2.5 times. A tile's
BLAS is a copy of every grain's triangles, so on scree it added 0.8 M baked triangles (about 50 MB) to save about 7.6 k
TLAS entries (under 1 MB of instance data), and on the shore it added 0.4 M triangles for 1 k more tiled grains. The
split is taken out; what stays from this step:
- A shared tile is built only from its third placement on (TILE_MIN_USES); the first two draw as their single grains
  (the same stones in the same place, so nothing changes on screen). The tiles used once or twice were most of the
  scree's cost.
- The square bookkeeping keeps each square's rectangle and level, and neighbours are any squares that touch.
Bumpy ground stays single grains, instanced per grain over the shared shape BLAS, which is already the cheapest way
to trace stones that do not repeat.

## Sub-pixel stones in ray tracing (round 4, 20 Nov)

**Where the small stones in the TLAS are.** Inside the view nothing is sub-pixel by design: each detail class stops
where its mean stone is 3 px (the far-edge aggregate takes over past it), and the shader already fades a grain under
3 px to its mean colour. What the TLAS holds besides is everything else round the camera: the path tracer takes every
instance in the camera's ray-tracing box that passes its visibility range (renderer_scene_cull.cpp, "RT: collect ALL
instances inside the camera AABB"), so the grains beside and behind the camera are there too, for shadows and bounce.
A detail cell's instance is its whole MultiMesh (2 to 8 m), so the existing off-frustum solid-angle cull
(rendering/pathtracing/cull_min_size, off by default; the Assassin's Creed Shadows approach) can never drop a grain:
the cell's box is never small.

**How others do it.**
- Moana on the GPU (Kim 2025): instance groups below a screen-size threshold leave the acceleration structure; the
  film camera is fixed, so the set is chosen once.
- Assassin's Creed Shadows (Ubisoft, GDC 2025) and UE's ray tracing culling (r.RayTracing.Culling, by projected
  angle and distance behind the camera): small objects outside the view leave the ray tracing scene, kept when big or
  near enough to shadow what is seen.
- DXR partitioned TLAS (Oct 2026 preview) and NVIDIA's vk_partitioned_tlas: rebuild only the partitions that change,
  so a culled set that changes with the camera stays cheap.

**Design.**
- Per record, not per MultiMesh: when a ground detail MultiMesh (rt_instanced) is expanded into TLAS entries, a grain
  outside the view (outside the frustum widened by a guard band) is left out when its angular size is under a
  threshold, unless it is close enough to the frustum to throw a shadow into it (within about ten of its own sizes:
  a 2 cm grain's shadow at 20 degrees of sun is under 6 cm).
- Hysteresis so the persistent TLAS slots stay put: the guard band is wide (15 degrees), and a MultiMesh's culled set
  is only redone when the camera has turned or moved past half the band since it was last done.
- Inside the view nothing changes, so the camera's own image is the same; shadows cast into the view keep their
  casters. Raster is untouched.
- Off by default in the engine (a project setting, rendering/pathtracing/cull_offscreen_records_size), switched on by
  RockField3D for its detail MultiMeshes only, through a per-MultiMesh flag next to multimesh_set_rt_instanced.
- Check: TLAS count and PT frame (eye level and top) before and after, 100% crops; then the PC GPU frame time.

**As built.** RenderingServer.multimesh_set_rt_offscreen_cull(multimesh, degrees) on rt_instanced MultiMeshes; the
TLAS builder drops a record when it is outside the view widened by 15 degrees a side, more than max(10 r, r + 1 m)
past that band, and smaller than the angle seen from the camera. The band is redone when the camera turns 7.5 degrees
or moves 1 m, which bumps the culling instances' generations so persistent slots rebuild. RockField3D sets 0.5 degrees
on its detail grains. rendering/pathtracing/offscreen_record_cull (on) switches it off to compare. Shore, PT, 640x360:
eye level 34.1 k to 29.4 k TLAS instances, top view 34.3 k to 24.9 k; both frames match pixel for pixel within noise
(mean difference under 0.001 of a level, max 8 of 255 on a handful of pixels). GPU time on the PC queue.

## Stones at distance vs the vegetation far reach (assessment, 20 Nov)

Paulo asked (vegetation thread, 19 Nov) whether vegetation's far approach applies to rocks and other ground stuff:
full density over the inner half of the range, then gentle thinning by (r/2 / d)^2 with the survivors widened up to
1.6x (Ghost of Tsushima's grass, Sucker Punch GDC 2021), and a draped canopy surface coloured like the drawn plants out
to 4 km.

**Today.**
- Ground detail (pebble, gravel, fine): each class drawn to where its mean stone is 3 px (32, 12 and 6 m at the
  bench), then the far-edge aggregate (two cover maps, 1 m and 8 m texels, 2 km across; Meng 2015) shades the ground
  with each class's colour and roughness. No bald line, no thinning band: the class stops where it is about to alias.
- Coarse stone layers stop at a fixed visibility range: scree 60 m, shore 100 m, river beds 120 m, glacial 300 m,
  rockfall 600 m, cliffs 500 m, tors, coast forms and landforms 900 m. Past their range nothing stands in for them:
  the cover maps carry only the three detail classes. On a talus slope the scree stones stop at 60 m on a line.
- Tiles and the far edge are independent of this.

**Where the vegetation approach fits.**
1. Coarse layers' far thinning (scree, shore, river, glacial): hold full density over the inner half of the range,
   then keep a hashed share (r/2 / d)^2 of the stones, the survivors scaled up to keep the covered area (capped at
   1.6x and never past the class's largest stone), and reach two to four times further for about the same instances.
   The stones themselves stay real geometry; only how many are drawn changes with distance.
2. Coarse cover in the far aggregate: a fourth class in the cover maps for the scree and shore stones (their cover
   and rock colour), so the ground past their range takes their tone as it does for the detail classes, and a third
   cover level (32 m texels) so it reaches 4 km like the canopy surface.
3. Detail classes: they already hand over without a bald band. A thinning band before the 3 px stop would only add
   aliasing (stones under 3 px shimmer in raster), so not recommended there.
- Trees are the trees thread's; big landforms (cliffs, tors, coast) already reach 500 to 900 m.

**Outcome (20 Nov).** Paulo picked option 2. The rocks thread had built thinning, the 4 km big-stone tier and a 32 m
rock cover map to 4 km that morning (4ce85981); this thread added widened survivors, thinning in stills and river
beds in the far map (docs/ideas/rocks.md, "Far rocks", added 20 Nov). Scree's far look was already the terrain's own
parallax blocks past the scree radius.

## Game trails and boar rooting in the stones (20 Nov)

Contracts on main (371c030c): `WorldStand.trail_at(pos)` returns (passes per year, tread share),
`get_trails()` lists the lines, `rooting_age_at(pos)` returns years since rooting or -1, and
`stand_changed(rect)` says where to rebuild. Vegetation already flattens litter and turf on the
tread (547664ee); terrain owns the bare-soil colour. Ground detail owns the stones.

**What the field studies say.**
- Trail wear (Marion and Leung, USGS trail-degradation reviews; Leung and Marion 1996, "Trail
  degradation as influenced by environmental factors"): traffic first removes litter and the fines,
  then the soil itself. What is left on the tread is an **armour of coarse, angular stones and
  exposed roots**, sitting proud of the surface instead of sunk in. Fines and small gravel are
  kicked and washed **to the edges**, where a low berm of loose material builds up. Wear rises
  fast for the first few hundred passes per year and then levels off.
- Wild boar rooting (Forests 13(8) 1234, 2022; Eur. J. Wildl. Res., Springer s10344-025-01994-5):
  rooting turns the top 5 to 15 cm in patches, leaving **loose clods and upturned stones** lying on
  top at random angles, with fines on top of them. The surface settles and grasses back over in
  one to two years; stones stay loose for the first season.

**Technique.** Placement-time modulation of the existing detail sources (the same approach as the
river-bed and scree layers): no new pass, no new shader. Inputs are sampled on the main thread into a
0.25 m grid per detail job (the stand calls are not thread safe, and a deer trail's tread is narrower
than the 1 m sub-cells the other sources use), read by `_place` on the worker where each stone falls.
- Wear `w = tread * (1 - exp(-passes / 300))`; shoulders `4 tread (1 - tread)` of that.
- Tread: an armour source of angular stones (scree shapes, local lithology), the coarse end of each
  class, sunk little and tilted up to 25°. Cover at full wear: 15 % pebbles, 10 % gravel, 5 % fines,
  times the soil's stoniness (0.5 to 1 with the rock exposure). Existing gravel and fines thin by
  `(1 - 0.8 w)`; the coarsest class stays.
- Shoulders: the kicked fine end of each class, 1 % / 8 % / 12 % cover.
- Rooting `r = 1 - smoothstep(0.25, 2, age)` (the terrain material's fade): stones of any size turned up loose, tilted up to 60°,
  barely sunk: 6 % / 5 % / 6 % cover at r = 1, fading as the patch settles.
- Worn squares are never cut into shared tiles (their grains stay single), and the shared tiles are
  cut from unworn ground, so a trail never repeats into a field.
- `stand_changed` rebuilds only the detail cells inside the changed rectangle (`ground_wear`, on).
- Turned soil clods: `GroundClods3D.rooting` (on). Round the camera, hashed per 0.5 m cell, about 90
  clods per m² of fresh rooting, log-normal round 7 cm (3 to 16 cm), loose on top at any angle; fewer,
  smaller, flatter and sunk deeper as the patch settles. Colour is the terrain material's turned humus
  (3199eeb8), greying the same way; the overturned sods' turf stays the terrain's shading, not geometry.
  Rebuilt when the camera moves a quarter of the radius or the stand changes. Ground bench:
  `--view=rooting` (fresh, 1 and 1.8 years, left to right), `--no-rooting` for the before frame.

Bench: `--trail[=passes] --rooting[=years]` (rocks bench), `--no-wear` for the before frame. Stats
`wear_armour`, `wear_shoulder`, `wear_rooting` in `get_ground_detail_tile_stats()`.
