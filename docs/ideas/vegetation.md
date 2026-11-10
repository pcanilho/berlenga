# Vegetation and biomes: grass, plants, trees, forests, meadows, swamps

Research note, 10 Nov 2026, written before any code. The goal: the engine generates convincing
natural scenes (forests, grass fields, swamps, parks) procedurally, at open-world scale, with
wind and contact, and with path tracing that keeps every blade and leaf. This note is the
umbrella. Details live in [grass.md](grass.md), [instanced-forests.md](instanced-forests.md),
[opacity-micromaps.md](opacity-micromaps.md), [open-world-geometry.md](open-world-geometry.md)
and the placement survey in the open-world project's `docs/ideas/forest-generator.md`
(Horizon, Far Cry 5, Deussen 1998, Lane and Prusinkiewicz 2002, EcoBrush 2017, Makowski 2019).

## What the engine has that vegetation can build on

- Terrain: `TerrainDeformer3D` and `TerrainBrush3D`, writing to Terrain3D by name or to a
  HeightMapShape3D. Height, slope and carved areas are known.
- Water: `PhysXRiver3D` (flow, carve, waterfalls) and `PhysXWaterSurface3D` (height, flow,
  shore). Distance to water and water depth can be derived per point.
- Weather: `WeatherMap3D` with one wind field, gusts, rain, wetness and puddles as global
  shader uniforms and `get_wind_at()`.
- Physics: PhysX for colliders, cloth and hair.
- Path tracing: captured vertex animation with BLAS refit under a per-frame budget
  (`RTDynamicScheduler`), skinned meshes, persistent MultiMesh slots, opacity micromaps, mesh
  LOD in the path tracer, back lighting, transmission and random-walk SSS.

Missing: a placement system, a plant generator, wind for trees and bushes that the path tracer
sees, a contact field, and the biome rules that tie them together.

## How others do it

- **Unreal Engine 5.7** (Nov 2025). PCG framework production ready (node graphs over terrain and
  world data, CPU or GPU execution, Biome Core plugin as the biome example). Procedural
  Vegetation Editor, experimental: a PCG-based graph that grows and shapes trees and exports
  Nanite-ready static or skeletal meshes. Nanite Foliage, experimental: Nanite Assemblies (a
  tree is instanced parts, not one mesh), Nanite Voxels for distance (better for branches than
  leaves, per Epic forum replies), and Nanite Skinning for wind: the Dynamic Wind plugin moves
  bones, "per bone instead of per vert". PCG can spawn instanced skinned meshes. An early user
  report (212k objects over 1.7 km by 0.7 km) found wind frames slower than the old foliage.
  Takeaway: Epic moved tree wind from vertex offset to bones, which is also what a path tracer
  can see.
- **Horizon Zero Dawn / Forbidden West** (van Muijden, GDC 2017). GPU placement per tile from
  density graphs over world maps, priority order with footprints, nothing stored.
- **Far Cry 5** (Carrier, GDC 2018). Houdini generates biomes, water, cliffs and roads from
  terrain; artists paint biome masks and edit on top.
- **Ghost of Tsushima** (Wohllaib and Rockenbeck, GDC 2021). Grass generated every frame on
  the GPU; one wind system for grass, trees, particles and clouds.
- **CryEngine.** Painted vegetation with layered vertex bending (Sousa, GPU Gems 3 ch. 16) and
  "touch bending" of physicalized branches on contact.
- **Unity HDRP.** Terrain trees and details, SpeedTree wind in vertex shaders; its path tracer
  ignores vertex animation.
- **SpeedTree** (commercial, the industry default). Rule-based tree growth, hand-tunable,
  LODs, and a wind model per branch level. Not usable (licence), but it sets the bar.

## Papers

Plant models:
- Weber and Penn, Creation and Rendering of Realistic Trees, SIGGRAPH 1995. Parametric
  recursive branching per level (length, taper, curve, split, gravity). Basis of Blender's
  Sapling and of EZ-Tree.
- Runions, Lane and Prusinkiewicz, Modeling Trees with a Space Colonization Algorithm, 2007.
  Branches grow toward attraction points filling a crown shape; natural gaps and competition.
- Palubicki et al., Self-organizing Tree Models for Image Synthesis, SIGGRAPH 2009. Buds
  compete for light and space; trees react to neighbours and obstacles.
- Pirk et al., Plastic Trees, SIGGRAPH 2012. Adapting a tree to its surroundings (walls,
  neighbours, light) after it is made.
- Makowski et al., Synthetic Silviculture, SIGGRAPH 2019. One model for tree shape and
  ecosystem, few parameters per species.

Wind:
- Diener et al., Wind Projection Basis for Real-Time Animation of Trees, Eurographics 2009.
  Per-branch modal response to a wind field, cheap at runtime.
- Habel, Kusternig and Wimmer, Physically Guided Animation of Trees, Eurographics 2009.
  Branch bending from beam physics, evaluated per hierarchy level.
- Zhao and Barbič, Interactive Authoring of Simulation-Ready Plants, SIGGRAPH 2013. Domain
  decomposition so whole plants can be simulated and touched.
- Jahrmann and Wimmer, I3D 2017 for grass (see grass.md).

Wetlands (for swamps): ecological zonation by water depth, the hydrosere. From deep to dry:
submerged plants, floating-leaved plants (water lilies, duckweed), emergent plants (reeds,
cattails, sedges) in shallow water, then marsh and wet meadow, then wet woodland (alder,
willow; cypress and tupelo in warm swamps) with standing dead trees and fallen logs. Each zone
is a band of water depth or water-table depth, which is exactly a density rule.

## Godot addons (code read)

- **ProtonScatter** (MIT). Editor-time modifier stacks (random, Poisson, project on collider,
  exclude) into MultiMesh; stores every transform. Good authoring model, not open-world.
- **Spatial Gardener** (MIT) and **Terrain3D's instancer** (MIT). Painting into stored
  MultiMesh cells.
- **SimpleGrassTextured** and **GodotGrass** (MIT): see grass.md.
- **EZ-Tree** (MIT, three.js, a Godot port exists). Weber-Penn style recursive branches with
  gnarl, twist, growth force and a trellis; LOD by re-meshing one skeleton at lower detail;
  leaves are alpha cards with a sine and simplex vertex sway. No bones, so its wind is invisible
  to a path tracer.
- **gdTree3D** (MIT GDExtension around proctree, BSD). Simple parametric trees, collision
  generation, wind weights in vertex colours, growth from sapling. Same wind limitation.

None of them places by rules over terrain and water at scale, and none animates in a way the
path tracer sees. Their ideas are reusable; their code is not what we need.

## Gaps in existing features, to close first

Checked against main at aab890e5.

1. **GPU-filled MultiMesh under path tracing.** `RTTlasBuilder::_add_expanded_mm_surface` builds
   each record's TLAS transform from the CPU copy of the MultiMesh buffer. Records written by a
   compute pass (`multimesh_get_buffer_rd_rid`), which GPU placement and grass need, never reach
   the CPU copy, so the path tracer would trace stale or empty records. Fix: a GPU pass that
   writes TLAS instance records straight from the MultiMesh buffer, with motion from the
   previous frame's buffer. Engine core.
2. **Instanced bone animation.** Godot skins one Skeleton3D per node. A forest of 100k trees
   with wind bones needs per-instance bone poses computed on the GPU (a few bones per tree)
   and skinning that both raster and the capture path read; Unreal 5.7 added instanced skinned
   meshes for the same reason. Engine core.
3. **Wind near the ground.** `WeatherMap3D` holds the mean wind in 32 m cells (4096 m over 128)
   plus gusts from Kaimal-weighted waves. Missing for vegetation: sheltering (wind drops
   sharply inside a canopy, Cionco 1965 and Raupach's canopy profile, and in the lee of
   buildings) and small gust patches of a few metres that sweep across a field. Weather files:
   for the thread that owns water and weather.
4. **World data maps.** Slope, curvature, flow accumulation, distance to water and water depth
   are not available as maps. The terrain deformer knows heights and carves; rivers and water
   surfaces know their own extent. A small GPU pass per tile can bake them. Water files: water
   depth and distance queries only, no changes to the water solvers.
5. **Wet foliage.** Weather wetness reaches materials through global uniforms, but no foliage
   material reads it (darker bark, wet sheen on leaves, drips). Material work, small.
6. **Animated MultiMesh at scale.** Captured MultiMeshes stay on the per-frame TLAS path
   (instanced-forests.md). Fine for grass tiles near the camera; partitioned TLAS is needed
   before whole forests sway. Needs the PC to measure.

Already in place and checked: alpha scissor in the path tracer, volumetric fog under path
tracing, buoyancy sampling of water surfaces (boats), skinned mesh refit, BLAS refit budget.

## Proposed design (module `modules/vegetation`)

1. **World maps per tile** (`VegetationMap3D`). Computed on the GPU when a tile streams in:
   height, slope, curvature, flow accumulation, distance to water and water depth (from rivers
   and water surfaces), sun exposure, wetness (from the weather state), carved and paved masks
   (from the terrain deformer), plus painted biome masks. Deterministic, cached per tile.
2. **Species and biomes as resources.** `PlantSpecies`: mesh variants with LODs, footprint
   radius, priority, clustering, and a small density rule stack over the world maps (curves on
   slope, water depth bands, wetness, distance to taller plants). `Biome`: a weighted list of
   species. Swamp, meadow, temperate forest, conifer forest and city park ship as examples.
3. **Placement** (`VegetationField3D`), Horizon's method: per tile, species in priority order,
   density dithered against a tiled blue-noise pattern (Bridson Poisson disk), each placed
   plant writing a footprint that lowers density for later species. Output is one MultiMesh
   per species per tile, which persistent MultiMesh slots and later partitioned TLAS handle.
   Hand-pinned hero plants act as footprints. Nothing stored but maps and edits.
4. **Grass and ground cover** (`GrassField3D`, grass.md): generated every frame on the GPU,
   density from the same maps.
5. **Plant generator** (`PlantGenerator`, editor tool with runtime API). Weber-Penn branching
   with optional space colonization crowns, outputting: bark mesh with LODs, leaves as real
   geometry near and alpha cards with opacity micromaps farther, collision for trunks, and a
   small bone hierarchy (trunk plus one or two branch levels). Bushes, reeds and dead trees
   are presets. Gives unlimited free species variants with wind built in; scanned free trees
   (Poly Haven) can still be rigged by the same tool.
6. **Wind on bones.** Each bone responds to `WeatherMap3D` wind with a per-level modal model
   (Diener 2009, Habel 2009); leaves add vertex flutter on top. Bones are skinning, which the
   path tracer refits; this follows Unreal 5.7's move and our rule that moving geometry uses
   bones, not vertex offsets, under path tracing. Far trees hold their pose (scheduler).
7. **Contact.** The trample field from grass.md bends bushes and reeds too; PhysX colliders on
   trunks; hand-placed bodies push branches through the same field.
8. **Swamps as the showcase** of water plus vegetation: zonation bands from water depth;
   floating plants placed on `PhysXWaterSurface3D` and `PhysXRiver3D` that ride the surface
   height and drift with the flow; mud and wet soil from terrain wetness; dead snags and logs
   from the generator; ground mist from the weather side.

## Path tracing

- Real geometry wherever the camera is close (blades, leaves), so most hits need no alpha test;
  opacity micromaps for the cards farther away.
- Grass: capture plus refit per tile (grass.md). Trees and bushes: skinned, refit near, held far.
- Tile layout matches persistent MultiMesh slots now and partitioned TLAS later; RTX Mega
  Geometry clusters and animated cluster templates are the step after (PC needed).
- Detail rule: the traced geometry is the raster geometry, so blades and leaves match at 100%
  crop. Every stage ships with raster versus path traced crops.

## Stages (one PR each)

0. Gaps 1, 2, 4 and 5 above, each with a bench or test; gap 3 handed to the weather owner.
1. World maps, species, biomes and placement, with a bench scene (`--vegetation`), using
   existing free meshes.
2. Grass field with wind and the trample field.
3. Plant generator with bones and LODs; bush, reed and dead tree presets.
4. Bone wind from the weather map, under path tracing.
5. Swamp: zonation, floating plants on water, mud, snags.
6. Path tracing scale-up (partitioned TLAS, Mega Geometry) on the PC.

The cloud can build and run all of it on Lavapipe; GPU timings and PT/RR crops wait for the PC.

## References

- Unreal Engine 5.7 release notes and coverage (Nov 2025): gamefromscratch.com/unreal-engine-5-7-released,
  80.lv/articles/unreal-engine-5-7-is-now-availabe; Epic forum threads 2685424 and 2677852.
- van Muijden, GDC 2017; Carrier, GDC 2018; Wohllaib and Rockenbeck, GDC 2021; Sousa, GPU Gems 3.
- Weber and Penn 1995; Runions et al. 2007; Palubicki et al. 2009; Pirk et al. 2012;
  Makowski et al. 2019; Diener et al. 2009; Habel et al. 2009; Zhao and Barbič 2013;
  Jahrmann and Wimmer 2017; Bridson 2007.
- ProtonScatter https://github.com/HungryProton/scatter; EZ-Tree https://github.com/dgreenheck/ez-tree;
  gdTree3D https://github.com/JekSun97/gdTree3D (all MIT, code read).
