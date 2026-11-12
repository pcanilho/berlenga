# Grass: procedural blades with wind, trampling and path tracing

Research note, 10 Nov 2026, written before any code. Prompted by a Godot grass addon shown on
r/godot (wind, blades that sway when something walks through them). The question: should the
engine have its own grass, and what should it be at open-world scale and under path tracing?

## What the engine has today

- No grass node. Grass would come in as a MultiMesh with a wind vertex shader.
- That already traces. A displacing `vertex()` is a capture (`displaced-hit-varyings.md`): the
  raster pass writes moved positions and the BLAS is built over them. A MultiMesh under
  `multimesh_merged_blas_max_triangles` (65,536) gets one merged BLAS with fast-build and
  allow-update flags, so wind costs a refit, not a rebuild.
- `RTDynamicScheduler` ranks captured items by visibility and spends a per-frame budget
  (1.5M refit triangles, 500k build triangles, 2000 operations). What misses the budget holds
  its last pose for up to 30 frames.
- Opacity micromaps (`opacity-micromaps.md`) make alpha-cut cards cheaper, but every hit on a
  card still has an alpha test. Real blade geometry needs none.
- Instancing: persistent slots and MultiMesh ranges (`instanced-forests.md`). Animated grass is
  left on the per-frame path there.
- Wind: `WeatherMap3D` publishes `weather_wind_map`, `weather_gusts`, `weather_map_rect` and
  `weather_wind_info` as global shader uniforms and `get_wind_at()` on the CPU. Cloth, hair and
  water already read it.
- Contact: the PhysX module knows every body; nothing turns that into a field grass can read.
- `RenderingServer.multimesh_get_buffer_rd_rid` and `multimesh_get_command_buffer_rd_rid` let a
  compute pass fill a MultiMesh and its indirect draw count on the GPU.

## Published methods

- **Ghost of Tsushima** (Wohllaib, GDC 2021). Blades are generated on the GPU each frame per
  tile from artist-painted grass type and density maps; nothing is stored per blade. Each blade
  is a cubic Bezier strip (15 vertices near, 7 far) with per-blade height, width, facing, bend
  and a clump id from Voronoi cells so neighbours lean together. Blades seen edge-on are widened
  in view space so the field never looks thin. Far away, density drops and blades widen to keep
  coverage. Wind is a scrolling 2D noise read per blade; the same wind drives trees, particles
  and clouds (Rockenbeck, GDC 2021). Shading: base-to-tip colour, a rounded normal across the
  width, height AO, specular and back-lit translucency.
- **Jahrmann and Wimmer, Responsive Real-Time Grass Rendering for General 3D Scenes**
  (I3D 2017). Each blade is a quadratic Bezier with three control points kept as state on the
  GPU. A compute pass applies gravity, a recovery force toward the rest shape (stiffness), wind,
  and collisions with spheres, then corrects the length. Trampled grass stays down and springs
  back over time. Culling by orientation, frustum, distance and occlusion; tessellation by
  distance.
- **Horizon Zero Dawn** (van Muijden, GDC 2017). GPU procedural placement: density comes from
  rules over terrain data (height, slope, wetness, roads), evaluated in tiles around the camera.
  This is the open-world answer to "where does grass grow" without storing instances.
- **Crysis** (Sousa, GPU Gems 3 ch. 16). Vertex-shader bending in layers (main bend, detail
  flutter) driven by vertex colours; CryEngine later added "touch bending" from physics contact.
- **Unreal Engine.** PCG or the foliage tool places instances; wind and interaction are World
  Position Offset. Under hardware ray tracing WPO needs "Evaluate WPO" and a per-frame dynamic
  geometry pass, the same capture we do. NVIDIA's RTX Mega Geometry foliage preview (GDC 2026)
  uses cluster BLASes and partitioned TLAS for millions of animated foliage elements.
- **Unity HDRP.** Terrain detail meshes with wind; the path tracer does not see vertex
  animation.

### Godot addons (code read)

- **SimpleGrassTextured** (IcterusGames, MIT). Alpha-cut cards in a MultiMesh painted onto a
  StaticBody3D. Wind is a vertex offset from a scrolling noise texture and global uniforms.
  Contact: an orthographic camera follows the player and renders "collider" meshes on render
  layer 17 into SubViewports (distance, motion, normal, two blur passes); the grass shader reads
  that displacement around the player only. Good result, but one player, viewport passes, cards
  (alpha test under path tracing), CPU placement.
- **GodotGrass** (2Retr0, MIT). A Ghost of Tsushima study: blade meshes in MultiMesh tiles,
  clumping from cellular noise, view-space widening, scrolling Perlin wind with domain warp,
  tile LOD by density and mesh swap. The README notes visible LOD pops and that CPU placement is
  the limit; it suggests compute placement.
- **Terrain3D** instancer. Hand-painted MultiMesh per region cell, transforms stored on disk.
  Fine for props, heavy for grass across kilometres.

None of the addons reads a shared wind field, handles many movers, scales past a level, or was
designed for a path tracer.

## Path tracing

Moving blades mean BLAS work every frame. Options, cheapest first:

1. **Capture and refit per tile** (exists). Tiles of at most 65k triangles become merged BLASes.
   Near tiles refit each frame inside the scheduler budget; far tiles hold their last pose. A
   field of 100k near blades at 13 triangles each is 1.3M refit triangles, about the whole
   current budget, so the near radius and blade LOD must be tuned on the PC.
2. **Geometry, not cards.** Blades as thin triangle strips need no any-hit shader and no
   micromap. This matters more than the refit cost: foliage alpha tests are the most expensive
   part of a trace.
3. **RTX Mega Geometry** (`VK_NV_cluster_acceleration_structure`, partitioned TLAS). Blade
   clusters built from templates each frame, built far faster than a refit. NVIDIA only, needs
   the PC; already queued as a separate item.
4. Linear swept spheres (RTX 50) suit round strands, not flat blades. Not for grass.

Detail rule: path tracing must show the same blades as raster. The traced geometry is the
captured raster output, so near grass matches exactly. Held far tiles lag in pose, not in
detail. Blades thinner than a pixel need DLSS RR to resolve them; this must be checked at 100%
crop against raster.

## Proposed design: `GrassField3D` (new module `modules/grass`)

1. **Generation on the GPU.** Tiles in rings around the camera. A compute pass fills each tile's
   MultiMesh buffer and indirect count from a density and type map: either its own painted
   texture, Terrain3D's control map read by name when installed (never bundled), or rules over
   height and slope (Horizon). `TerrainDeformer3D` carves and flattens remove grass where they
   cut. Blades are seeded by world position so a tile regenerates the same field and LOD changes
   thin it instead of reshuffling it (removes GodotGrass's pops).
2. **Blade shape and shading** as in Ghost of Tsushima: cubic Bezier strip, clumping, view-space
   widening, LOD by vertex count and density. Material uses the engine's back lighting and
   transmission so path traced grass glows against the sun the same way raster does. Wetness
   from the weather state darkens and adds a specular sheen.
3. **Wind** from `weather_wind_map` and `weather_gusts` only, with a per-blade flutter term, as
   cloth and hair already do. No second wind.
4. **Contact: a trample field.** A small texture around the camera (for example 64 m at 12.5 cm)
   holding bend direction and amount. Each frame a compute pass splats every PhysX body and
   character capsule near the camera into it, then relaxes it toward zero with a damped spring.
   That is Jahrmann and Wimmer's recovery force applied per texel instead of per blade, so
   blades stay stateless (they are regenerated) while trampled paths persist and spring back.
   Any number of movers, no SubViewports, no render layer.
5. **Path tracing** through the existing capture and refit path, tiles sized under the merged
   cap, with a `pt_animated_radius` past which tiles hold their rest pose. Mega Geometry later.

Stages, one PR each: (1) generation, blades, LOD, wind, raster; (2) trample field from PhysX;
(3) path tracing tuning plus a bench scene (`--grass`); (4) Mega Geometry when the PC is back.
GPU timings and the PT/raster crops need the PC; the cloud can build and run Lavapipe checks.

## What we built (stage 2.1)

`GrassField3D` in `modules/vegetation` (next to placement, which shares the rules), not a new
module. Two changes from the plan above, both measured:

- **Blades are generated on the CPU, per tile.** A tile of 8 m at 60 blades/m2 builds in about
  0.25 ms, and tiles are rebuilt only when the camera crosses into new ones (12 a frame at
  most), so a GPU pass buys nothing yet and the CPU path keeps every blade visible to the path
  tracer through the normal MultiMesh capture. The GPU fill (gap 1 made it visible to the path
  tracer) is the route if far denser fields need it.
- **Quadratic, not cubic, Bezier**: with four segments a quadratic already bends smoothly, and
  its length can be kept exactly by Jahrmann and Wimmer's correction, so blades do not stretch
  in strong wind.

What it does:

- Blades on a jittered world grid seeded by position hash, grouped into clumps
  (`clump_size`) that share height, facing, lean and dryness, as in Ghost of Tsushima. The same
  ground always grows the same blades.
- Rules from a `PlantSpecies` over a `TerrainDataMap3D` (slope, wetness, water depth, distance
  to water), the same rules as placement. No blades under water by default.
- LOD by rank: at 25, 50 and 75% of `radius` a tile keeps 1/2, 1/4 and 1/8 of its blades (the
  blades with the lowest rank, so thinning never reshuffles) and widens the kept ones by
  1/sqrt(keep) so coverage holds.
- Wind from `WeatherMap3D` (`weather_wind`) when a weather map is present, else
  `fallback_wind` with travelling gusts; a per-blade flutter on top. One wind for grass, cloth,
  hair and water.
- Shading: base to tip gradient, some dry tips (`dryness`), darker roots, back lighting, and the
  Lagarde wetness of gap 5 from the weather map (porosity 0.25: blades are waxy).

## Results (stage 2.1)

Bench `docs/bench/grass --test` (rolling hills, a pond, radius 40 m, tiles 8 m): 83,408 blades
in 97 tiles; near density 60/m2 within range, the far ring thinned by LOD, no blade under
water, the same field on a second build, and a camera move of one tile rebuilt 58 tiles in
14 ms (spread over frames by `max_tiles_per_frame`).

Raster and path traced frames of the field match at 100% crop (mean difference 10 of 255,
mostly the path traced soft shadows under the blades and the wind pose at capture); path
tracing keeps every blade, the dry tips and the clumps. Media: grass-field.jpg (raster and path
traced), grass-wind.gif (raster, gusts travelling downwind).

Cost: CPU build as above, nothing per frame on the CPU except the tile check; one MultiMesh
draw per tile. GPU timings and the path traced cost on hardware wait for the PC.

## Trample field (stage 2.2)

As planned in point 4 above: a bend field around the camera (`trample_size` 32 m,
`trample_resolution` 128, so 25 cm texels), world aligned so it scrolls with the camera without
smearing. Each texel holds a bend vector (direction and amount, 0 to 1) in world xz.

- **Movers**: every `Node3D` in the group `trample_group` (default `grass_trample`) whose
  underside is within the grass. Its radius and half height come from the meta
  `trample_radius`, else its first `CollisionShape3D` (sphere, capsule, cylinder or box), else
  `trample_radius`. So PhysX bodies, characters and props join by being in the group, with no
  extra setup. Scripts can also press it with `add_trample()` (footsteps, a landing).
- **Pressing**: full bend under the mover, fading over its rim; blades fall away from the
  mover and forward along its motion (Jahrmann and Wimmer 2017). A texel keeps the stronger of
  its current bend and the new one, so passing twice does not over-bend.
- **Recovery**: the bend relaxes with a half-life (`trample_recovery`, 6 s), Jahrmann and
  Wimmer's recovery applied per texel, so trails persist for a while and stand back up.
- **Blades** read the field at their root and lay down along it in place of the wind; the
  length-preserving bend keeps them from stretching. No per-blade state, so tiles can still be
  rebuilt at any time.

CPU, not compute: 16,384 texels relax and upload (half floats, 64 KB) in 0.13 ms a frame on the
cloud CPU, and only the group scan once every texel is upright. A compute pass is only worth it for
much larger or finer fields.

Bench `docs/bench/grass --test`: flat (bend 1.0) under a parked ball, bent away from it at its
rim, untouched 3 m away, back to 5% after 2 s at a 0.5 s half-life, and nothing pressed by a
ball held above the grass. `--trample` rolls a ball and walks a capsule through the field
(grass-trample.gif). The path traced frame keeps the trails: the bend is the same vertex
shader the path tracer captures.

## A full carpet (stage 2.3)

Paulo, after the first shots: too sparse, the blades too thin and tall to act as a carpet.
A real meadow holds hundreds to thousands of shoots a square metre; no game draws that. How
the published fields read as full:

- **Ghost of Tsushima** (Wohllaib, GDC 2021): blades seen edge-on are shifted in view space so
  they never thin to a line, and their normals are tilted outward so a flat blade shades as a
  rounded one; height and type vary per tile from textures; flowers and pampas grass are
  separate instanced meshes placed in the same tiles; far tiles fall back to a texture on the
  ground.
- **Horizon Zero Dawn** (van Muijden, GDC 2017) and **Far Cry 5** (Carrier, GDC 2018): ground
  cover is several layers placed in order, short dense plants under taller ones, each with its
  own density graph.
- **Unreal Landscape Grass Types** and the **Witcher 3** / **Red Dead Redemption 2** fields: the
  terrain's own layer under grass is painted the grass's colour, so the gaps read as thatch.

What we added:

- `GrassField3D.view_facing` (0.6): edge-on blades widen across the view.
- `GrassField3D.blade_segments`: a second field with one-segment blades, short and wide, at
  about 300 a square metre out to 14 m, is the undercoat (a quarter of the vertices per blade).
- Ground tint: the field publishes its coverage (the rules over the data map) and
  `ground_tint` as `grass_ground_*` globals; `GrassField3D.get_ground_shader_code()` gives the
  terrain shader `grass_ground(world_pos, albedo)`. Past the blades' range the ground keeps the
  grass colour, as in Ghost of Tsushima's far tiles.
- `PlantSpecies` patches (`patch_scale`, `patch_cover`, `patch_seed`, `patch_invert`): value
  noise keeps a species to patches. Tall grass grows in patches, clover in the exact complement,
  flowers in drifts, all through the existing placement. `PlantSpecies.visibility_range` drops
  small ground cover by distance.

Bench `docs/bench/grass` (layers on by default, `--layers=0` for the single field): tall grass
55 blades/m2 inside its patches, undercoat 286/m2 near, nothing under water, clover only
between the tall patches (347 of 55,163 inside one, on its soft edge), 7,499 flowers. Clover
and flowers place in 72 ms once. Frame time on the cloud CPU rasteriser, close view: 238 ms
against 217 ms for the single field (+10%), 123,078 blades against 79,992. GPU timings wait for
the PC. Media: grass-carpet.jpg (before above, after below), grass-carpet-pt.jpg.

## Blade detail (stage 2.4)

The wide meadow shot showed the carpet, not the plants on it: one flat green, wide single-triangle
blades and a pale soil between them. What the published grass does about that:

- **Colour at several scales.** Ghost of Tsushima varies colour per clump and over the field; Horizon
  Zero Dawn and CryEngine (Kingdom Come: Deliverance 2) tint vegetation from a large-scale colour map.
  GrassField3D now drifts each blade towards a yellower or a bluer green by two octaves of world noise
  (about 9 m and 2 m) plus a per-blade term, with a value jitter of about 20%. `color_variation`
  (default 0.5) scales it; 0 gives the old single colour.
- **Thatch.** Real swards carry dead blades from the root, not only dry tips. A third of the
  `dryness` fraction is now straw from the root.
- **Monocot leaf detail.** A lighter midrib and faint parallel veins across the blade, faded out by
  `fwidth` once a blade is under a few pixels wide, so far blades do not shimmer.
- **Blade outline.** Edges stay nearly parallel and taper near the tip (width `1 - 0.9 t^2`) instead of a
  linear wedge.
- **Bench carpet.** The undercoat in `docs/bench/grass` is now two-segment blades, 2 cm wide at
  640 /m² (was one triangle 4.5 cm wide at 320 /m²), over a darker soil tint: under a dense sward the
  soil sits in the blades' shade, so it should read darker than the blades, not lighter.

Cost on the CPU rasteriser (lavapipe, 1280x720, meadow with all species): 517 ms before, 610 ms after;
almost all of it is the doubled undercoat. The shader changes alone are a few ALU ops per fragment.

## Hiding the ground (stage 2.5)

Paulo, 10 Nov: in KCD2 and Star Citizen's Nyx the floor is never seen through the vegetation; ours
showed gaps. What the published work does is not more blades alone but making the ground the base
of the grass:

- Serious Sam 4 (Croteam, GDC 2019, "4 Million Acres"): blades are true geometry, "albedo colored
  from the floor", the floor material matched to the grass, a per-blade ambient shadow on the floor,
  and several density grids overlapped; a separate material set inside forests.
- Ghost of Tsushima (GDC 2021): normals tilted outward keep the field full; far grass becomes
  artist-authored textures.
- Star Citizen Planet Tech v5: the terrain shader colours the ground from the vegetation that is not
  loaded yet, so the ground already reads as grass before the blades appear; ground layers are
  bedrock, soil, then surface items (leaves).

What we built:

- `get_ground_shader_code()` now gives `grass_turf()`: the sward's root colour (`ground_tint`),
  tinted by the same colour patches as the blades (one shared function, so blade and floor agree
  everywhere), streaks of matted blades along a slowly turning direction, and dark thatch from the
  grass's `dryness` and `dry_color`. `grass_ground(world_pos, albedo)` keeps its signature and now
  mixes the soil to that turf where grass grows. New globals: `grass_ground_dry`, `grass_ground_params`.
- Bench: a third, very short and dense grid near the camera (`Turf`: 1600 /m², 3 to 7 cm, 5 m
  radius), as Serious Sam 4 overlaps grids.

Cost on the CPU rasteriser (1280x720 meadow): turf ground 610 to 595 ms (free, within noise); the
near turf grid 595 to 1033 ms. The grid's real cost needs the GPU bench (about 125k extra
one-triangle blades): on lavapipe every blade is CPU work. The ground alone removes the pale gaps;
the grid fills the nearest metres.

## Outer band (no edge at the radius)

Before: tiles were built out to `radius` and stopped, so the field ended in a hard line (the
valley scene's 40 m). Ghost of Tsushima (Wohllaib, GDC 2021) and Horizon Zero Dawn (van Muijden,
GDC 2017) thin blades out per blade, not per tile, and colour the terrain like the grass so the
field reads to the horizon.

- Blades: from 60% of the radius to just inside it, each blade drops out at its own point in a
  set order and shrinks to nothing (`fade_band` in the blade shader, set from the field's
  centre each update). The last blade is gone before the outermost tiles end.
- Ground: over the same band `grass_ground()` turns the ground under the grass to the blades'
  own colour seen from a distance (`grass_ground_far`: base to tip, some dry, darkened by the
  sward's self shade), fully covering, so beyond the blades the ground is the field's colour.
  The band is a global (`grass_ground_fade`), published by the widest GrassField3D, so a near
  undercoat field does not pull it in.
- Cost: one distance and a smoothstep per blade vertex; nothing on the CPU.


### Even thinning (blue-noise ranks)

A hash per blade gave white noise: as a level or the band thins the grass, survivors bunch into
clumps with bare gaps. Which blades a coarser level keeps now comes from a 64 x 64 blue-noise
rank mask (void and cluster, Ulichney 1993; built once at start, tens of milliseconds), turned or mirrored
per 64-cell block so it does not repeat. Every threshold of such a mask keeps an even spread, so
both the distance levels and the outer band (which drops blades in the order of their rank within
the coarsest level, `INSTANCE_CUSTOM.w` integer part) thin evenly. An R2 dither (Roberts 2018) was
tried first: even, but on the blade grid it shows diagonal rows. No cost per frame; the blade
count per level is unchanged.

## Sun and shade

Leaves acclimate to the light they grow in (Lichtenthaler 2007): shade leaves carry more
chlorophyll per area and less carotenoid, so they are darker and bluer green; sun leaves are paler
and yellower. Grass under shade stretches and flops (shade avoidance, Franklin 2008).

The light a plant grew in is (1 - crown shade) x slope exposure, where exposure is the climate
map's shortwave over its map mean, clamped to 0.3..1.5 (1 without a climate). It is fixed when the
plant is placed, so it costs nothing per frame:

- ground plants carry it in the fraction of INSTANCE_CUSTOM.y (the range keeps the whole metres);
- grass blades in the integer part of INSTANCE_CUSTOM.z over 4 (the rest lean stays below 4);
- shared shader `vegetation_light_tint()` (vegetation_light.h): shade towards x(0.72, 0.85, 0.84),
  full sun above 1.0..1.3 towards x(1.12, 1.05, 0.74). Light 1 (open flat ground) is unchanged.
- grass height x(1 + 0.35 shade - 0.15 sun), lean x(1 + 0.8 shade); shade keeps grass green
  (its dry share falls by up to 70%), a sunny slope adds a quarter.
- only leaves and stems respond; petals, litter, twigs and dead (worn) tissue keep their colour.

## GPU grass (research, stage 3)

Paulo's pick, 11 Nov: denser grass for the same cost, as Ghost of Tsushima does it. Research
before code.

### What the published games do

- **Ghost of Tsushima** (Wohllaib, GDC 2021). No blade is stored. Every frame a compute pass walks
  the tiles near the camera, reads the painted grass type and density maps, makes each candidate
  blade from a position hash, culls it against the frustum and by distance, and appends survivors
  to an instance buffer drawn indirectly. There are two vertex counts, 15 near and 7 far, and
  density falls off smoothly per blade, not per tile.
- **Ghost of Yotei** (Sucker Punch tech deep dive, PlayStation Blog, 23 Oct 2025). The same GPU
  compute renderer, now emitting twice as much grass and as many renderable items. Compute jobs
  occlusion-cull, allocate memory and fill draw records. In a far mountain view, "over one million
  trees, rocks, and bushes are culled down to about sixty thousand individual items". Weapon swings
  write a cut buffer that the grass reads.
- **Horizon Zero Dawn** (van Muijden, GDC 2017). Placement rules are evaluated on the GPU over
  terrain maps in tiles around the camera, and the density maps are made from rules rather than
  painted. This is the part we already do on the CPU with `PlantSpecies` and `TerrainDataMap3D`.
- **Frostbite** (Wihlidal, GDC 2016) and **Assassin's Creed Unity** (Haar and Aaltonen,
  SIGGRAPH 2015) laid out GPU-driven culling and compaction into indirect draws, which every later
  grass system relies on.
- **Unreal Engine 5.7** added Nanite Foliage (instanced assemblies, voxelised far foliage). It is
  aimed at trees and shrubs. Grass in UE still comes from the Grass Type / PCG paths.

### Where our grass spends its time today

- **CPU build per tile.** At 60 blades/m2 an 8 m tile builds in about 0.25 ms. The loop is per
  blade: rule checks, height lookup, light grid, packing. At three to five times the density that
  becomes 0.75 to 1.25 ms per tile, and 12 tiles in a frame (`max_tiles_per_frame`) makes a 9 to
  15 ms spike whenever the camera crosses a tile line. This is what caps density now.
- **Raster draws whole tiles.** Godot culls each tile's MultiMesh by its AABB, so with 8 m tiles
  per-blade frustum culling would save little; most of the drawn blades are in view.
- **The path tracer's merged BLAS cap.** A tile of 3,840 blades at about 8 triangles is 31k
  triangles, under `multimesh_merged_blas_max_triangles` (65,536). Three times the density is 92k:
  the tile would fall to the expanded path, one TLAS record per blade. Denser grass needs smaller
  tiles (4 m: 23k) or fewer triangles far out.

### What fits this engine

The path tracer settles the design. `multimesh_mark_buffer_rd_changed()`
(gpu-multimesh-records.md) brings GPU-written records to the path tracer cheaply when they change
as tiles stream in. Records culled and rewritten every frame, as in Ghost of Tsushima, would mean
a merged BLAS rebuilt every frame for every near tile, and blades behind the camera would drop out
of reflections and shadows. Path tracing must not remove detail. So:

1. **Stage 3a: GPU tile fill (Horizon placement, Tsushima blades).** On the CPU, per tile, only a
   small map is baked: suitability x shade rule, light and ground height, on 0.5 m texels (256
   texels for an 8 m tile instead of 3,840 blades). It is built from the same `PlantSpecies` rules,
   so placement does not change. A compute shader then writes the tile's records straight into its
   MultiMesh (`multimesh_get_buffer_rd_rid`) from the same hashes, blue-noise rank, clumps, light
   packing and custom data as the CPU loop today, and calls `multimesh_mark_buffer_rd_changed`.
   Blades come out identical, so raster and path tracing stay matched, but the per-blade work moves
   to the GPU, where 10k blades are a few microseconds. The current CPU path stays as the
   reference and the fallback (`--test` compares both).
2. **Density and tile size.** With the CPU cost gone, raise near density (for example 60 to
   180/m2) and use 4 m tiles inside the path tracer's animated radius so every tile stays a merged
   BLAS. Thinning by rank keeps working per blade. Check the refit budget (1.5M triangles) against
   the new near count; past it, the far tiles hold their pose as now.
3. **Stage 3b (later, raster only): per-frame visible compaction.** For raster without path
   tracing, a compute pass can cull blades against the frustum and Hi-Z and apply smooth distance
   thinning and a 7-vertex far blade into an indirect MultiMesh (`use_indirect`). With path tracing
   on, the tile records stay the source. This is only worth doing if the PC timings show vertex cost
   from blades out of view or occluded.

Cloud checks: identical records CPU vs GPU (hash of the buffers), tile build time before/after,
raster vs path traced crops of the denser field at 100%, with no SHADER ERROR. GPU frame timings
need the PC.

### Built (3a)

`GrassField3D.gpu_fill` (on by default), `modules/vegetation/grass_fill.glsl`. Per tile the CPU
lists the candidate cells (blue-noise rank under the level's keep, in a fixed order), bakes the
rules and the canopy's shade rule into a 0.5 m suitability grid, and passes the 1 m light grid
and the map's height texels under the tile; one compute thread per candidate makes the blade
with the same hashes, clumps, shade avoidance and packing as the CPU loop and writes its 16
floats in place (a rejected candidate is written as a zero record), then
`multimesh_mark_buffer_rd_changed()`. The MultiMesh gets the tile's box as a custom AABB, so it
draws before the records' readback lands. A suitability corner at zero keeps the cell empty, so
blades never creep into water by interpolation.

Measured on the cloud (lavapipe, bench `docs/bench/grass --test`):

| | CPU loop | GPU fill |
| --- | --- | --- |
| Blades matching the CPU set | | 99.1% (tall grass), 100% (undercoat) |
| Main thread, whole tall field at 60/m2 | 23.4 ms | 14.1 ms |
| Undercoat field (640/m2, short radius) | 29.4 ms | 17.6 ms |
| Tall field at 180/m2 | 70.7 ms | 38.9 ms |

Raster frames of both paths in still air differ by 0.14 of 255 on average. Two fixes found on
the way, both for every grass field and MultiMesh: the blue-noise rank took a mutex per blade
(now a one-time static), and MultiMesh allocation sent a zeroed copy through the staging buffer
(now `buffer_clear` on the GPU), which together were most of the CPU path's cost too.
What stays on the CPU per tile: the candidate list, the rules grid and the MultiMesh itself.
Moving candidates to the GPU would need compaction and a count the CPU does not have; not worth
it until the PC shows the remaining cost matters. GPU time per tile needs the PC.

## References

- Wohllaib, Procedural Grass in Ghost of Tsushima, GDC 2021.
- Rockenbeck, Blowing from the West: Simulating Wind in Ghost of Tsushima, GDC 2021.
- Jahrmann, Wimmer, Responsive Real-Time Grass Rendering for General 3D Scenes, I3D 2017.
- van Muijden, GPU-Based Procedural Placement in Horizon Zero Dawn, GDC 2017.
- Carrier, Procedural World Generation of Far Cry 5, GDC 2018.
- Sousa, Vegetation Procedural Animation and Shading in Crysis, GPU Gems 3, ch. 16 (2007).
- SimpleGrassTextured, https://github.com/IcterusGames/SimpleGrassTextured (MIT), code read.
- GodotGrass, https://github.com/2Retr0/GodotGrass (MIT), code read.
- Sucker Punch, Ghost of Yotei tech deep dive, PlayStation Blog, 23 Oct 2025.
- Wihlidal, Optimizing the Graphics Pipeline with Compute, GDC 2016.
- Haar, Aaltonen, GPU-Driven Rendering Pipelines, SIGGRAPH 2015 Advances course.
