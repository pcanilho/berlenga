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

## References

- Wohllaib, Procedural Grass in Ghost of Tsushima, GDC 2021.
- Rockenbeck, Blowing from the West: Simulating Wind in Ghost of Tsushima, GDC 2021.
- Jahrmann, Wimmer, Responsive Real-Time Grass Rendering for General 3D Scenes, I3D 2017.
- van Muijden, GPU-Based Procedural Placement in Horizon Zero Dawn, GDC 2017.
- Sousa, Vegetation Procedural Animation and Shading in Crysis, GPU Gems 3, ch. 16 (2007).
- SimpleGrassTextured, https://github.com/IcterusGames/SimpleGrassTextured (MIT), code read.
- GodotGrass, https://github.com/2Retr0/GodotGrass (MIT), code read.
