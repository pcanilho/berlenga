# Open-world terrain: real geometry for the path tracer, PhysX collision, region streaming

Research note, 9 Nov 2026. Written before any code. The question: should the fork have its own
terrain, and if so, how, given that rivers (water.md, Step 4) want to carve it later and that the
fork is pitched for large open worlds.

## What a terrain has to do here

1. **Be seen by the path tracer as it is.** Primary rays, shadows, reflections and GI all hit the
   BLAS, so the BLAS must hold the terrain at the detail the camera sees, near and far, without
   per-frame rebuilds of hundreds of thousands of triangles.
2. **Collide in PhysX** with the same surface the renderer draws (feet, wheels, buoyancy probes,
   floaters and debris should not float or sink).
3. **Stream** at open-world scale: tens of km, only a ring of regions resident, a cheap far field
   to the horizon, precision far from the origin.
4. **Be editable by water**: a river's channel written into the ground, collision and BLAS
   updated for the touched tiles only.
5. **Shade with layers** (rock, grass, mud, snow) blended by a control map, and with fine detail
   that survives path tracing at 100% crop (project rule: path tracing must not remove detail).

## Where the fork stands

- **No terrain in core.** Godot has `HeightMapShape3D` for collision only.
- **PhysX heightfields already work.** `godot_physx_shape_3d.cpp` turns `SHAPE_HEIGHTMAP` into a
  `PxHeightField`: 16-bit samples mapped onto each shape's own height range (error = range / 65535,
  3 cm for a 2 km range), NaN samples become holes (Jolt's rule). Every cell uses material 0; the
  triangle diagonal is PhysX's default. Contacts and ray queries handle heightfields
  (`godot_physx_space_3d.cpp`).
- **Vertex-displaced surfaces are traced from a capture.** The raster pass runs `vertex()` and the
  BLAS is built over the moved vertices (displaced-hit-varyings.md, #182). The capture is redone
  whenever one of its stamps changes: camera, model transform, material uniforms, global uniforms.
- **Mesh LOD in the tracer** (`rendering/pathtracing/mesh_lod`), **persistent instances**,
  **off-screen culling** and **camera-relative tracing** (1024 m grid) are merged
  (open-world-geometry.md, README "Open worlds"). Partitioned TLAS and cluster AS are planned there.

## How the two Godot terrain addons work (code read, 9 Nov)

### Terrain3D (TokisanGames, MIT, C++ GDExtension, commit 852b6c9 of 2 Nov 2026)

- **Data.** Regions of 64 to 2048 vertices a side on a 32 x 32 region map (`REGION_MAP_SIZE`),
  so up to 65 km at 1 m spacing. Per region: height (`FORMAT_RF`, 32-bit float), control
  (`FORMAT_RF` holding a bit-packed uint: base and overlay texture ids, blend, angle, scale, hole,
  auto flag) and color (`RGBA8`). Region files are loaded with `load_region()`; no threaded
  streaming.
- **Geometry.** A geometry clipmap (Losasso and Hoppe 2004) centred on the camera: tiles, edges,
  trims and fills per LOD ring (`terrain_3d_mesher.cpp`). Each time the camera moves one vertex
  spacing the rings are snapped and their transforms set with `instance_teleport`. Heights come
  from `texelFetch` on a texture array in `vertex()`; `VERTEX` is written through `VIEW_MATRIX`.
  Optional tessellation levels and a displacement buffer (a canvas pass that renders displacement
  to a texture read in `vertex()`).
- **Collision.** `Terrain3DCollision`: dynamic mode builds `HeightMapShape3D` tiles (16 m) within a
  64 m radius of the camera; full mode builds a shape per region. Both go through the physics
  server, so they already become PhysX heightfields here.
- **Under our path tracer.** The clipmap is a displaced surface, so it is traced from a capture.
  Its `vertex()` depends on the camera (`VIEW_MATRIX`), on `_target_pos` and on the ring
  transforms, so it is recaptured on every frame the camera moves. #182 made its varyings correct.
  Two things keep the cost bounded: the clipmap's vertex count never changes (fixed rings), so the
  capture scheduler can refit its BLASes and rebuild only periodically (`render_raytracing.cpp`,
  `RT_SCHEDULER_REFIT`), unlike Unreal's rebuild on every LOD change; and the clipmap is centred on
  the camera with 7 rings by default, so off-screen rays (shadows from a ridge behind the camera,
  reflections) still find ground, coarser with distance. Not measured yet (no GPU in the cloud):
  the open number is capture plus refit ms per frame while flying, for the default mesh size 48.
- **Shading quality.** Its material (height-blended texture layers, auto shader by slope, macro
  variation, triplanar projection, per-vertex texture painting) runs in `fragment()`, which the
  hit shader runs too, so its look carries into path tracing as it is.
- **Also in the box.** Editor sculpt and paint tools, a foliage instancer (MultiMesh per region),
  an ocean mesh, importers, holes, navigation baking. Years of editor work we would otherwise redo.
- **Carrying cost.** Builds only as a godot-cpp GDExtension (one `GODOT_MODULE` stub, no module
  build); needs Godot 4.6 or newer, so it loads into the fork (4.8-dev) unchanged. Active: 1.1.0
  stable is the latest release, 122 commits since May 2026. Vendoring it as a fork module means
  porting its godot-cpp includes and merging that cadence forever; loading it as an addon costs
  nothing in the fork.

### HTerrain (Zylann, MIT, GDScript)

- One map up to 4097 x 4097 (`MAX_RESOLUTION`), chunked LOD on a quadtree (`quad_tree_lod.gd`),
  heights displaced in the shader. Maintenance mode. Same capture problem under path tracing,
  smaller scale. Not an open-world option.

### Takeaway

Both build their geometry in `vertex()`, which a ray tracer must capture. For HTerrain that ends
it (scale, GDScript). For Terrain3D it is a cost to measure, not a blocker: its rings keep a
fixed vertex count, so they refit.

## What other engines do

- **Unreal Engine 5 Landscape.** Components and sections of a heightmap, LOD per section with a
  fractional morph in the vertex factory (`CalcLOD`). For hardware ray tracing the landscape is
  dynamic geometry: vertices are generated from the heightmap, the BLAS is refitted while only
  the morph changes and fully rebuilt when the integer LOD changes (Epic support answer, forum
  thread 2720550, March 2026). Epic calls the cost "a known issue" and offers
  `r.RayTracing.Geometry.Landscape.LODsUpdateEveryFrame=-1` as a workaround at the risk of dark
  splotches (thread 2745488, July 2026). Nanite landscapes build a Nanite mesh per proxy (rebuilt
  after sculpting, at cook time if stale), with skirts to hide proxy seams and optional
  displacement through Nanite tessellation; both Nanite and non-Nanite data stay resident, and
  hardware RT still uses the classic landscape, since Nanite ray tracing is "not considered game
  ready in 5.8". Lesson: tie the RT geometry to LOD changes only, and make LOD changes rare.
- **Unity.** Heightmap terrain up to 4097 per tile, quadtree patches chosen by pixel error.
  `Terrain.enableHeightmapRayTracing` (since 2022.2) adds the heightmap geometry to the ray
  tracing acceleration structure as a separate, fixed representation.
- **Frostbite (Battlefield 3, Widmark, GDC 2012).** Quadtree of heightfield tiles streamed on
  demand, procedural virtual texturing for the shading layers, CPU-side LOD selection with GPU
  tessellation for close detail. The reference for "modern, complete and scalable".
- **Far Cry 5 (Moore, GDC 2018).** Public abstract only: a GPU compute pipeline for LOD,
  culling, stitching and rendering the heightfield, plus procedural cliffs and displacement
  geometry. The slides are members-only; nothing here depends on unread details.
- **CryEngine.** Heightmap terrain in sectors with per-sector LOD, plus voxel objects for
  overhangs and caves. Same split we would make: heightfield for the ground, meshes for the rest.
- **Classic LOD schemes.** Chunked LOD (Ulrich 2002, skirts), geometry clipmaps (Losasso and
  Hoppe 2004; GPU Gems 2 ch. 2), CDLOD (Strugar 2009, morph in the vertex shader). The first keeps
  vertices still between LOD switches; the other two move them every frame, which is cheap in
  raster and costly for a BVH.
- **NVIDIA displacement for ray tracing.** `heightmap_rtx` (Apache-2.0) displaced RT triangles
  with displacement micromaps; it is archived (Feb 2025) and `VK_NV_displacement_micromap` is no
  longer available. Its README points to RTX Mega Geometry: `vk_tessellated_clusters`
  (Apache-2.0) tessellates and displaces per frame into cluster acceleration structures
  (`VK_NV_cluster_acceleration_structure`, driver 572.16 or newer, present on our 4090). That is
  the route for detail below the heightmap spacing, later and NVIDIA-only.

## Options

**A. Use Terrain3D as an addon, called by name (never bundled).** Projects install it; the fork
makes sure it traces well and stays independent of it. Fork-side work is generic, not
Terrain3D-specific: measure capture plus refit cost on the 4090, tune the capture scheduler for
fixed-topology displaced meshes, and if needed a per-material hint that a `vertex()` does not
depend on the camera, so a clipmap ring is recaptured only when it snaps. Rivers already plan to
carve it by name (water.md Step 4). Gaps at open-world scale: region loading is synchronous
(`load_region()`), collision is dynamic heightmap tiles (already PhysX heightfields here, but one
material), no edit layers. Those can be contributed upstream (MIT) rather than forked.

**A2. Vendor Terrain3D as a fork module.** Same features, built in. Rejected: a godot-cpp to core
port, an active upstream to merge, and a dependency Paulo does not want shipped with the engine.

**B. Own heightfield terrain, real geometry.** A core node, working name
`Landscape3D` (no clash with Terrain3D's class names), built for the tracer first:

- **Data.** Regions (default 512 m) of 16-bit heights with a per-region range (as the PhysX path
  already quantizes), a control map compatible with Terrain3D's bit layout (so its data imports),
  holes as a control bit. One file per region; spacing per terrain (1 m default, 0.5 m allowed).
- **Render geometry.** Static chunk meshes (64 m) with a precomputed LOD chain (2x per level,
  restricted to one level between neighbours) and stitched edge index variants (16 per LOD) so
  there are no T-junction cracks or skirts for light to leak through. Vertices never move in
  `vertex()`; the material shades only. Each (chunk, LOD, stitch) gets a BLAS that is built once
  and kept by the persistent-instances path; an LOD switch swaps the instance to another BLAS.
  LOD picked by projected error with hysteresis, the same rule for raster and the tracer, so the
  tracer sees exactly what raster draws. Chunks behind the camera keep their distance LOD, so off-
  screen shadows and reflections see real terrain. Measured against Unreal's refit-and-rebuild:
  zero per-frame BLAS work while the camera moves; only LOD switches build.
- **Far field.** Beyond the resident ring, one coarse proxy (e.g. 16 m spacing) per region,
  always resident, in raster and the TLAS to the horizon (Unreal's HLOD idea).
- **Shading.** Splat layers from texture arrays with height blending, triplanar on steep slopes,
  macro variation; normal maps carry detail below the vertex spacing. A runtime virtual texture
  (Frostbite, Far Cry) is a later optimisation, not a start.
- **Collision.** One `PxHeightField` per 128 m tile through the existing heightmap path, extended
  with per-cell material indices (friction, footsteps; PhysX gives 7 bits per triangle) and the
  diagonal set to match the render mesh at LOD 0 so contact sits on the drawn surface. Edits use
  `PxHeightField::modifySamples` instead of recooking. Far from the origin, `PxScene::shiftOrigin`
  in step with the relative-origin grid.
- **Streaming.** Regions load on worker threads around streaming sources (camera, player);
  chunk meshes are generated from heights at load (not stored); BLAS builds go through the
  existing per-frame build budget; one partition per region once the partitioned TLAS lands.
- **Edits and rivers.** A layer stack (base heights plus brush layers, as Unreal's edit layers)
  so a `PhysXRiver3D` channel is a layer, not a destructive edit: changing the river re-bakes only
  the touched chunks, BLASes and PhysX tiles.
- **Authoring.** Import first (16-bit PNG, EXR, RAW from Gaea, World Machine, Houdini; Terrain3D
  region files), sculpt tools later. Editor brushes are the largest single cost of a terrain
  system and can wait.

**C. Voxel terrain (godot_voxel, MIT).** Overhangs and caves, but a large module, mesh rebuilds on
every edit, transvoxel seams, and no heightfield collision. Wrong cost for open ground; caves and
cliffs stay meshes on top of A or B.

What B buys over A, honestly: zero per-frame BLAS work while flying (persistent BLAS per chunk),
PhysX tiles with per-cell materials, threaded streaming and edit layers designed in, and no
outside dependency. What it costs: a terrain system and, eventually, its editor tools.

## Recommendation

A first, B only if the numbers say so.

Terrain3D's quality is in its material and tools, and both carry into path tracing now that #182
fixed displaced varyings. What it costs us under path tracing is a recapture and refit of the
clipmap each frame the camera moves; that may well be a millisecond or two on the 4090, which
would not justify writing a terrain system. So:

1. Generic test scene with Terrain3D (by name, not in the repo): raster vs path traced at 100%
   crop, and capture plus refit ms while flying, on the 4090 when the desktop is back.
2. If the cost is small: document Terrain3D as the recommended terrain (README line via the
   coordinator), keep the river carving adapter, contribute threaded region loading upstream if
   mortal-plane needs it.
3. If the cost is large or the BVH quality after refits hurts trace time: first the camera-
   independence hint above (small, generic); B only if that is not enough.

B's design stays here so the decision is quick if step 1 goes badly.

## Open questions

- Region and chunk sizes against the partitioned TLAS cell (128 m columns in Valence, 1024 m
  relative-origin grid here).
- Whether stitched variants or per-chunk BLAS at a fixed LOD ring (Unity-style, simpler, more
  triangles) win on the 4090. Step 5 decides.
- Memory per km²: 1 m spacing, 16-bit heights = 2 MB; LOD 0 chunk geometry and BLAS only for the
  resident ring.

## Sources

- Terrain3D source, https://github.com/TokisanGames/Terrain3D (MIT): `terrain_3d_mesher.cpp`,
  `terrain_3d_collision.h`, `terrain_3d_data.h`, `terrain_3d_region.h`, `shaders/main.glsl`.
- HTerrain source, https://github.com/Zylann/godot_heightmap_plugin (MIT): `hterrain_data.gd`,
  `util/quad_tree_lod.gd`.
- Epic, Using Nanite with Landscapes:
  https://dev.epicgames.com/documentation/unreal-engine/using-nanite-with-landscapes-in-unreal-engine
- Epic forums, Nanite landscape ray tracing proxy design (2720550) and Landscape in hardware ray
  tracing (2745488): https://forums.unrealengine.com/t/questions-on-nanite-landscape-ray-tracing-proxy-design-and-static-mesh-generation/2720550,
  https://forums.unrealengine.com/t/landscape-in-hardware-ray-tracing/2745488
- Unity, Terrain.enableHeightmapRayTracing:
  https://docs.unity3d.com/ScriptReference/Terrain-enableHeightmapRayTracing.html
- Widmark, Terrain in Battlefield 3: A Modern, Complete and Scalable System, GDC 2012:
  https://media.contentapi.ea.com/content/dam/eacom/frostbite/files/gdc12-terrain-in-battlefield3.pdf
- Moore, Terrain Rendering in Far Cry 5, GDC 2018 (abstract):
  https://gdcvault.com/play/1025261/Terrain-Rendering-in-Far-Cry
- Losasso and Hoppe, Geometry Clipmaps, SIGGRAPH 2004; Asirvatham and Hoppe, GPU Gems 2 ch. 2.
- Ulrich, Rendering Massive Terrains using Chunked Level of Detail Control, SIGGRAPH 2002 course.
- Strugar, Continuous Distance-Dependent Level of Detail for Rendering Heightmaps (CDLOD), JGT 2009.
- NVIDIA heightmap_rtx (archived): https://github.com/NVIDIAGameWorks/heightmap_rtx
- NVIDIA vk_tessellated_clusters: https://github.com/nvpro-samples/vk_tessellated_clusters
- PhysX PxHeightField API (modifySamples):
  https://nvidia-omniverse.github.io/PhysX/physx/5.5.1/_api_build/classPxHeightField.html
