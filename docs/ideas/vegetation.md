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
4. **World data maps** (done: `TerrainDataMap3D`, terrain-data-maps.md). Slope, curvature, flow accumulation, distance to water and water depth
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

## Reusing what the engine already has

- **Hair solver for near grass and reeds.** `PhysXHair3D` (TressFX port) already simulates strands
  on GPU compute: Verlet step, local and global shape constraints toward the rest pose,
  follow-the-leader length, collision spheres, weather wind. A grass blade or reed is a short
  strand, and the global shape constraint is Jahrmann and Wimmer's recovery force. Blades
  within a few metres of the camera can run on it, so trampled grass is real per-blade physics
  there; the trample field covers the rest. Hanging moss and vines in swamps are hair grooms.
- **PhysX articulations** for hero bushes and saplings next to the player (CryEngine's touch
  bending): a few joints per plant, only near the camera, otherwise bones driven by wind.
- **PhysX rigid bodies and Blast** for fallen logs, snapped branches and felled trees.
- **PhysX cloth** for large leaves and hanging fronds where they can tear.
- **PhysX Flow** as local wind: its velocity grid around the camera (a passing train, a
  helicopter, an explosion) adds to the weather wind that grass and trees read. This is God of
  War's fluid wind, limited to where Flow already runs. Flow also gives swamp ground mist.
- **PhysX buoyancy sampling** (boats) for lily pads and duckweed riding the water.
- **TerrainDeformer3D brushes** for paths worn into grass and fields flattened for good.
- **WeatherMap3D** for wind, rain, wetness, puddles and cloud shadows over fields.
- **Materials**: back lighting and transmission for thin leaves and blades, random-walk SSS for
  thick leaves and stems, opacity micromaps for leaf cards, path-traced mesh LOD for far trees.

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

## Next round candidates (15 Nov)

- **Plants between stones.** The rock generator (docs/ideas/rocks.md) will lay scree, river cobbles
  and shore shingle as instanced stones. Real ground flora there lives in the gaps, not on open
  ground: parsley fern and stone bramble on scree, biting stonecrop and saxifrage cushions in
  crevices, thrift and sea campion on shingle, wall-rue and spleenworts in rock cracks, cushion moss
  on stone tops. Placement reads the same stone list as `VegetationObstacles`: a gap rule (distance
  to the nearest two stones, shade, wetness from contact wetness) instead of the open-ground rules,
  and moss on stone tops by up-facing, reusing the tree lichen and moss code. This is the main thing
  the rock generator needs from vegetation; boulders and `get_boulders` can use it first.
- **No-hitch grass rebuild.** Changing grass settings at runtime rebuilds every tile in one frame
  (about 40 ms). Spread the rebuild across frames, nearest tiles first, keeping old tiles until the
  new ones are ready.
- **Far vegetation as geometry.** UE 5.7 Nanite Foliage turns distant foliage clusters into
  pixel-sized voxels with stored normal distributions instead of billboards (Epic docs,
  "Nanite Foliage"). Our far grass is a colour bake on the terrain. A voxel or aggregate far layer
  would keep volume and parallax at the horizon, but it needs a GPU cost bench first, so it is
  research only until the PC is back.
- **Seasons shots.** The seasons code is on main; the before/after shots for the site are not taken.
- **Plants that give way.** Grass tramples; ferns, bracken and nettles do not. Bend them from the
  same trample field.

## Next round candidates (16 Nov)

Put to Paulo as a card; nothing built yet.

- **Waterside and wet-stone plants (recommended).** Stream stones sort into bands by how often
  they are under water: always submerged carries Fontinalis antipyretica and Platyhypnidium
  riparioides, the splash and flood band Racomitrium aciculare and Hygrohypnum, the dry tops above
  ordinary moss (Scarlett and O'Hare 2006, in-stream bryophytes of English and Welsh rivers; Glime,
  "Bryophyte Ecology", vol. 1 ch. 11). Waterfall spray zones hold thallose liverworts (Conocephalum,
  Pellia, Marchantia) and filmy ferns on constantly wet rock (Ratcliffe 1968, Atlantic bryophytes).
  Banks carry marsh marigold at spring lines, meadowsweet in tall-herb fen, butterbur on gravel
  bars (Rodwell, "British Plant Communities" vol. 2 and 3: M27 Filipendula-Angelica, S-gravel
  communities). Placement reads what we have: the data map's `wetness`, `water_depth`,
  `water_distance`, and per stone the water's height and flow (PhysXRiver3D `sample_height`,
  `sample_depth`, `sample_flow`; PhysXWaterSurface3D `is_wet`; WeatherMap3D
  `get_contact_wetness_at` for spray). Stone tops gain a submergence term next to
  `stone_top_damp`. New presets: aquatic moss streamers (trailing in the flow direction), thallose
  liverwort rosettes, marsh marigold, meadowsweet, butterbur.
- **Dune grass that traps sand.** Marram thrives on burial and builds the foredune by slowing
  saltating sand (Maun 1998, "Adaptations of plants to burial in coastal sand dunes"; Hesp 2002,
  foredunes and blowouts). With the sand system's streamers and `blown` cells, marram tufts would
  lower the wind's take and deposit behind themselves (shadow dunes, nebkha). Lyme grass, sea holly,
  sea rocket. Needs the sand thread's deposit API.
- **Bog and moor plants.** Sphagnum hummock and hollow microtopography (Rydin and Jeglum, "The
  Biology of Peatlands"), cotton grass, bog asphodel, sundew, deer grass, in the wet hollows of
  the upland valley beside heather and bilberry.
- **Far plants as real volume.** Unreal 5.7 Nanite Foliage style aggregates for distant clumps
  instead of the colour bake; off by default until the PC measures cost.

## Trees shaped by their place: placement (16 Nov)

For the trees module's hard places (627a91de). A species with meta `wind_flag` is turned with mesh
+X down the prevailing wind (`litter_wind`, else the weather map's wind; 0.3 rad of scatter)
instead of a random yaw. A species whose meshes carry meta `tree_ground_slope` (slope in radians,
erosion) takes, at each spot, the variant grown on the slope nearest the ground's (ties broken by
the plant's random number) and is turned so mesh +Z runs downhill. Bench checks "wind-flagged
trees" and "bank trees".

## Far plants as volume (built 16 Nov, off by default)

Picked by Paulo on 16 Nov (fourth of the round). Sources: Unreal 5.7's Nanite Foliage, which
voxelises distant foliage so a forest keeps its mass instead of thinning to cards (Epic docs
"Nanite Foliage"; 80.lv on the voxel representation); Loubet and Neyret (2017), "Hybrid
mesh-volume LoDs for all-scale pre-filtering of complex 3D assets" (geometry near, volume far);
Neyret (1998) volumetric textures; Bako et al. (2023), "Deep Appearance Prefiltering".

- Before: a species past its `visibility_range` leaves only the far colour on the ground (the
  cover bake), so bracken, heather and tall herbs flatten into paint past 15 to 30 m, and the
  silhouette of a fern bank against the slope is gone.
- `VegetationField3D.far_volume` (on by default since 16 Nov: every feature ships on, the PC tunes cost later), `far_volume_range` (150 m), `far_volume_cell` (0.3 m):
  per tile and species, each placed plant's mesh bounds fill an ellipsoid of leaf cover over its
  foot; covers add on the voxel grid and a voxel is solid with probability 1 - exp(-cover)
  (Beer-Lambert, a fixed hash: a thin stand stays porous and never flickers); the solid voxels'
  faces open to the air make the mesh, corners jittered by a hash of their position (shared by all
  faces there, so the shell stays closed) so it reads lumpy, not boxy; colour is the species' far
  colour, darker three voxels down into the stand. Plain opaque geometry with vertex colour: the
  path tracer sees exactly what raster draws. It crossfades in from 0.85 of the range (dither,
  fade Self) as the plant shader thins the plants.
- Bench: vegetation `--far-volume[=off]`, bracken and heather (25 and 18 m ranges) down the
  256 m valley from eye height: 127 tile shells, 283k triangles for the whole map.
- Not yet: coarser cells with distance (a second shell at 2x cell past half the range would
  roughly quarter the triangles); per-voxel colour from the plants' real colours (it uses the
  single far colour); shells in the cover map's far band. PC: GPU cost against the far colour
  alone, and a denoised PT pair, before it can default on.

## Dune grass that traps sand (built 16 Nov)

Picked by Paulo on 16 Nov (third of the round). Sources: Maun (1998), "Adaptations of plants to
burial in coastal sand dunes"; Hesp (2002), foredunes and blowouts; Huiskes (1979), Biological
Flora of the British Isles: Ammophila arenaria; Raupach (1992, 1993) drag partition and Wolfe and
Nickling (1996) for the frontal area index; Rodwell vol. 5 (SD4 to SD8) for the sets; Stace (2019).

- The sand side is the ground module's (docs/ideas/sand.md, "Dune grass"; 96500d57): a
  GroundSurface3D lists shelter nodes in `wind_shelters` and asks each
  `sample_lateral_cover(position)` for lambda, the frontal area per square metre of ground.
  VegetationField3D answers it: each placed plant's frontal area (`PlantSpecies.wind_frontal_area`
  times its scale squared; negative, the default, takes 0.4 of its first mesh's height times its
  mean width, a grass tuft's optical porosity being about 0.6) spread bilinearly over a 1 m grid,
  rebuilt only when plants change (populate, streamed tiles in or out), read bilinearly, at most
  0.5. Streamed and populated fields give the same answer (bench check "sand shelter").
- Three presets, real geometry (plant_dunes.cpp): `PRESET_MARRAM` (45 to 70 rolled, stiff leaves,
  a third dead and flopped; dense pale panicles with spikelets breaking their outline),
  `PRESET_LYME_GRASS` (broad flat glaucous leaves, stout spikes), `PRESET_SEA_HOLLY` (spiny
  holly-like blades: `_add_disc_leaf` gained a saw depth, five to seven sharp points between
  rounded notches; a low-branching stiff bush, blue heads in spiny ruffs).
- `PlantGenerator.make_dunes(DUNES_EMBRYO | DUNES_MOBILE | DUNES_FIXED)` by distance to the water
  (2 to 30 m, 5 to 120 m, 80 to 400 m) and slope under 34 degrees, grasses in clonal patches.
- Burial: marram grows up through burial (Maun 1998), so a tuft's base sinking into the sand the
  ground deposits is the right look (nebkha: a hummock round the tuft). Raising plants with the
  deposit is not done; at the ground's 2 cm an hour it would not show in a session.
- Bench check with the real pair: a dry-sand GroundSurface3D following the bench field under a
  12 m/s wind carries 0.012 kg/m/s among the plants on its own and 0.00007 once the field is in
  its `wind_shelters`. Next: a dune view with nebkha forming round marram; the dune landforms
  themselves are the terrain thread's.

## Bog and moor plants (built 16 Nov)

Picked by Paulo on 16 Nov (second of the round). Sources: Rydin and Jeglum, The Biology of
Peatlands (2013) for the hummock and hollow pattern and the Sphagnum species by water table;
Rodwell, British Plant Communities vol. 2 (M17 to M21 mires) for the sets; Stace, New Flora of
the British Isles (2019) for sizes and flowering.

- Four presets, real geometry (plant_bog.cpp): `PRESET_SPHAGNUM` (a low hummock 40 to 90 cm
  across: a lumpy dome of packed shoots with 160 to 240 star-shaped capitula on it, 40% of them
  at the edge), `PRESET_COTTONGRASS` (channelled leaves, two to six nodding spikelets that flower
  dark in April and May, then hang as cotton balls into August), `PRESET_BOG_ASPHODEL` (flat
  equitant fans, spikes of yellow six-tepal stars July and August, orange capsules into winter),
  `PRESET_SUNDEW` (a flat rosette of red round blades, each with 16 rim and 7 inner tentacles
  tipped by a glossy drop, roughness 0.08; white flowers in July).
- Cotton heads: real heads are thousands of bristles 2 to 3 cm long, far too many to model, and
  40 to 70 strips per head read as spiky starbursts. Each head is a white ovoid core (two domes,
  1.2 to 1.7 cm radius) carrying the mass, with 40 to 55 short crinkled bristles breaking its
  outline, so it reads as a soft ball close up and as a white dot far off.
- Phenology in degree days over 5 C: cotton grass flowers 80 to 380, holds its cotton to 800;
  bog asphodel 900 to 1350; sundew 800 to 1300.
- `PlantGenerator.make_bog(BOG_BLANKET | BOG_VALLEY)`: all species on slopes under 8 degrees
  (fade 3) where the wetness index is over 8 to 10 (fade 1.5). One patch noise per stand (scale
  3 m, cover 0.4): hummock species in the patches, hollow species outside them
  (`patch_invert`), so hummocks and hollows sort themselves without a new rule. Blanket: red
  Sphagnum capillifolium hummocks with heather, ochre S. papillosum lawns, cotton grass, bog
  asphodel and sundew in the hollows. Valley mire: green S. cuspidatum in the hollows, ochre on
  the hummocks, cotton grass throughout, asphodel, sundew and rush.
- Bench: plants bench size checks, cotton heads in July but none in April, the bog sets' rules
  and hummock/hollow split.
- Next: pool edges (bog pools with bogbean, a Valence water read), and hummocks raising the
  ground itself rather than sitting on it.

## Next round candidates (17 Nov): Atlantic heath, Mediterranean scrub, worn paths

Atlantic heath and gorse (Paulo picked all three in order, 17 Nov; dwarf shrubs and sets built
17 Nov in plant_heath.cpp, gorse and broom come from TreeGenerator as shrub species and join
the sets when they land). Dwarf-shrub heath covers
acid, nutrient-poor ground from the Atlantic coasts of Portugal to Norway, on sea cliffs, sandy
plateaux and upland moor (Gimingham 1972, "Ecology of Heathlands"; Webb 1986, "Heathlands";
Rodwell 1991, British Plant Communities vol. 2, H1 to H22). The engine has heather already; the
heath needs its other woody plants, each real geometry:
- Common gorse (Ulex europaeus): a dense, rounded, very spiny shrub 1 to 2.5 m (dwarf forms on
  cliffs), the leaves reduced to grooved green spines 1.5 to 2.5 cm, branched spines on every
  shoot; coconut-scented yellow pea flowers 1.5 to 2 cm, heaviest February to May but some all
  year; black hairy pods that crack open in summer heat (Stace 2019; Richardson and Hill 1998,
  "The biology of Australian weeds 34: Ulex europaeus"). Old bushes grey and dead inside, the
  living shell outside: real branches, not a textured blob. Western gorse (U. gallii) is low
  (to 1 m), flowers July to October, so the heath is yellow most of the year.
- Broom (Cytisus scoparius): erect green, five-angled, nearly leafless switch-like twigs to 2 m,
  yellow flowers May and June, black pods (Stace 2019).
- Bell heather (Erica cinerea): low shrublet 15 to 60 cm on the driest, sunniest ground, whorls
  of three dark needle leaves, bell-shaped crimson-purple flowers June to September (Bannister
  1965, Biological Flora: Erica cinerea). Cross-leaved heath (E. tetralix) on the wet heath, grey
  hairy leaves in fours, pale pink bells in terminal clusters (Bannister 1966).
- Crowberry (Empetrum nigrum): prostrate mats of needle-like rolled leaves and black berries on
  windy upland and coastal heath (Bell and Tallis 1973, Biological Flora).
Built: `make_heath(HEATH_DRY | HEATH_WET | HEATH_MARITIME)`; real needles (whorls of three or
four, bell heather's axillary leafy tufts), about 35 to 45k triangles a plant; rules: rain over
700 mm, January over -4 C, growing season over 6.4 C (alpine above), shade under 0.3, wetness
under 7.5 (dry) or 7 to 10 (wet), maritime on exposure over 0.4 at 6 to 150 m above the sea.
Planned placement as researched: heath sets by aridity, soil depth and exposure: dry heath (heather, bell heather,
gorse) on free-draining slopes, wet heath (cross-leaved heath, cottongrass, sphagnum) in hollows
with the bog set, maritime heath (dwarf gorse, heather and thrift, wind-pruned) on cliff tops
with RULE_COAST_EXPOSURE, and gorse scrub where soil is deeper. Calluna's growth phases
(pioneer, building, mature, degenerate; Watt 1947, Gimingham 1972) give a heath's patchwork of
bush sizes and gaps: a per-patch age drawn from the patch noise.

Mediterranean scrub, garrigue and maquis (Paulo picked it second, 17 Nov; dwarf shrubs and sets built 17 Nov in plant_scrub.cpp, the tall maquis shrubs come from TreeGenerator). Evergreen, aromatic, sclerophyll
shrubs on summer-dry ground, from the Algarve and the Tagus hills round the basin (Blondel et
al. 2010, "The Mediterranean Region: Biological Diversity in Space and Time"; Flora iberica,
Castroviejo et al.; Rivas-Martinez 2011 for the bioclimate belts). Maquis is the tall, closed
scrub (1 to 4 m) on deeper soil; garrigue the low, open, broken cover (under 1 m) on thin soil and
limestone with rock showing between the bushes. Plants, real geometry:
- Gum rockrose (Cistus ladanifer): an erect shrub 1 to 2.5 m of few, upright, sticky branches;
  opposite, linear-lanceolate, dark glossy leaves 4 to 8 cm, varnished with labdanum resin (high
  specular sheen, grey felt below); white crinkled-silk flowers 5 to 10 cm, often with a maroon
  blotch at each petal base, April to June; woody, ten-celled capsules stand after. On acid soils,
  the dominant shrub of the Portuguese and Spanish interior jaras.
- Sage-leaved rockrose (Cistus salviifolius): a low, sprawling shrub 30 to 60 cm, small, wrinkled,
  grey-green ovate leaves, white flowers 4 to 5 cm.
- Rosemary (Salvia rosmarinus): 0.5 to 1.5 m, dense, linear, rolled leaves 1.5 to 4 cm, dark above,
  white felt below; pale blue two-lipped flowers along the shoots, winter to spring. Limestone.
- French lavender (Lavandula stoechas): 30 to 100 cm, grey-green linear leaves, dense flower heads
  topped by a tuft of purple bracts ("rabbit ears"), March to June. Acid soils.
- Thyme (Thymus spp.) and other dwarf aromatics: cushions 10 to 30 cm.
- Kermes oak (Quercus coccifera) and lentisk (Pistacia lentiscus): dense, small-leaved, spiny or
  leathery evergreen mounds that make most of the maquis' bulk.
- Dwarf fan palm (Chamaerops humilis): clumps of fan leaves on short trunks, 1 to 2 m, on the
  Algarve's coastal limestone; the only native palm of Europe.
Placement: the climate map's aridity and summer drought (RULE_ARIDITY, RULE_PRECIPITATION) and
mild winters (RULE_JANUARY_TEMPERATURE over about 2 C); maquis where the soil is deeper, garrigue
on thin soil and rock (RULE_SOIL_DEPTH, RULE_ROCK), sunny slopes first (RULE_SUNLIGHT); bare
ground between the bushes is part of the look. Summer dormancy: the cistus leaves fold and turn
grey-brown in July and August (summer semi-deciduous), a season the phenology must carry.
Built: `make_scrub(SCRUB_GARRIGUE | SCRUB_MAQUIS)` with rosemary, French lavender, thyme and
sage-leaved rockrose, real leaves on branched woody frames (about 20 to 55k triangles a plant);
rules: January over 3 C, rain 250 to 1,100 mm, aridity 0.2 to 0.75, shade under 0.35, wetness
under 8; garrigue on soil under 0.45 m with rock, maquis on soil over 0.35 m; thyme and lavender on
the sunniest ground. The rockrose's leaves fold, shrink, grey and partly fall from early July to
late September (by day of year). `apply_scrub_rules(species, stand)` gives TreeGenerator's gum
rockrose, kermes oak, lentisk and fan palm the same rules. Flowering windows are in degree days of
a mild coastal year (rosemary 0 to 900, lavender 300 to 1,300, thyme 600 to 1,500, rockrose 550
to 1,100); rosemary's autumn flowers before the year turns are not modelled (forcing restarts
on 1 January).
Woody shrubs (17 Nov): TreeGenerator builds gorse, broom, gum rockrose, kermes oak and lentisk
(`make_shrub_species`); the sets place them first, in clumps, under their own rules
(`make_woody_shrub`): gorse (mature and young) and broom on dry heath, gorse cushions on cliff-top
heath (Rodwell 1991, H7/H8), none on wet heath; lentisk, kermes oak and gum rockrose in the maquis,
kermes oak cushions and young gum rockrose in the garrigue. Two variants each: mature gorse is
about 2M triangles with real spines.

Worn paths and trampling (built 17 Nov). Where people and animals walk, plants thin out,
get shorter and flatter, and the walked line turns to bare, compacted soil; the edges keep the
low, tough plants (Cole 1995, "Experimental trampling of vegetation", J. Appl. Ecol. 32: two
measures, resistance to the first passes and tolerance to recover; Cole and Bayfield 1993, the
standard trampling protocol; Liddle 1997, "Recreation Ecology"). What sets the damage:
- Passes: cover falls steeply with the first 25 to 100 passes a year, then levels off; the
  centre of a used path is bare, a band either side keeps a shortened, trampled sward.
- Growth form (Cole 1995): tufted grasses, sedges and rosette plants (plantain, the "path weeds")
  resist and recover; erect forbs, ferns and dwarf shrubs (heather, bilberry) break and die. So
  the edge of a path is plantain, rye-grass and pearlwort, not taller meadow plants.
- Ground: wet and soft ground widens a path (walkers step round mud), steep ground cuts it into a
  rut with braids.
Engine pieces: a path mask (a texture or curves) and the existing trample field (VegetationField3D
already bends plants under bodies) for passes; per-species trample resistance as a PlantSpecies
value from the growth form; the plants' scale and density fall with passes, the resistant set
taking over at the edges; GroundSurface3D or the terrain material shows bare, compacted soil in
the middle. Desire lines could come from the mud and sand threads' tracks over time.
Built: `paths` on VegetationField3D and GrassField3D, each path a centre line with a width and
walkers a year; passes fall off across it as a Gaussian (sigma a quarter of the width) over a
wider tail (15 % of the walkers at three times the sigma, people stepping aside), bucketed on an
8 m grid. `PlantSpecies.trample_resistance` (passes that halve cover) comes from the growth
form via `PlantGenerator.get_trample_resistance` (plantain 1000, daisy and dandelion 600, clover
500, grasses and sedges 300, moss 150, forbs 40 to 80, heaths and scrub 40, ferns and bilberry
30); cover is `1 / (1 + x^2)` with x = passes / resistance, the plants left shrink towards 45 %
and lie over (the blades' rest lean grows with the wear). So at 3,000 passes the centre is bare,
grass keeps a shortened, flattened sward a step out and the path weeds hold the edge. The ground
tint carries the tread as a second channel and turns it to compacted soil: darker and browner
than loose ground, damp patches, grit and small stones. The grass field thins and shortens blades on both its CPU and GPU fill, and
its ground tint shows bare soil along the line. Left for the terrain material: a compacted-soil
layer read from the same paths.

## Salt marsh and shingle plants (built 17 Nov)

Where the land meets a sheltered sea the ground is mud and the plants sort by how often the tide
covers them; on open coasts the backshore is sand or shingle with a strandline flora. Both are
zoned by height in the tidal frame far more sharply than by anything else (Adam 1990, "Saltmarsh
Ecology"; Rodwell 2000, British Plant Communities vol. 5, "Maritime communities"; Silvestri,
Defina and Marani 2005, "Tidal regime, salinity and salt marsh plant zonation"). Lower limits are
set by flooding stress, upper limits by competition (Bertness and Ellison 1987; Pennings and
Callaway 1992).

Salt marsh, low to high (heights for a mesotidal coast, as metres above mean sea level):
- Pioneer zone, about mean high water neaps (0 to 0.7 m): glasswort (Salicornia europaea agg.:
  leafless, jointed, succulent green stems 10 to 30 cm, branching like little candelabra, turning
  red and purple in autumn) scattered on bare mud, and common cord-grass (Spartina anglica: stiff
  grey-green leaves 30 to 100 cm in dense clonal clumps that trap mud).
- Low to mid marsh (0.5 to 1.3 m): sea purslane (Atriplex portulacoides: a silvery-grey, mealy
  low shrub 20 to 60 cm lining the creek edges), sea aster (Tripolium pannonicum: fleshy leaves,
  pale lilac daisies in late summer), sea lavender (Limonium vulgare: rosettes of leathery
  leaves, flat-topped sprays of lavender flowers in July and August that colour the marsh), common
  saltmarsh-grass (here: a low sward of dry grass).
- High marsh (1.1 to 1.9 m): thrift cushions, sea lavender, sea aster, red fescue sward, sea rush.
Creek levees carry sea purslane; pans (shallow hollows) stay bare or carry glasswort. Marshes form
only on sheltered, low-energy coasts on nearly level mud (slopes under about 2 degrees).

Shingle and the strandline (above the highest tides, 1.8 to 4 m, on the beach's backshore):
sea kale (Crambe maritima: huge, waxy, glaucous, crinkled cabbage leaves in a low mound 30 to
75 cm, white honey-scented flower heads in June), sea campion (Silene uniflora: trailing glaucous
mats with white bladder-calyxed flowers), yellow horned-poppy (Glaucium flavum: grey-blue
crinkled hairy rosettes, yellow poppies and seed pods up to 30 cm long, curved like horns), with
thrift (Scott 1963, "The ecology of shingle beach plants"; Randall 1977; Packham and Willis
1997, "Ecology of Dunes, Salt Marsh and Shingle").

Built (plant_saltmarsh.cpp):
- Eight presets, real geometry: glasswort as jointed succulent candelabras (each segment swelling
  from a narrow node), cord-grass as stiff two-ranked clumps with pressed spikes, sea purslane as a
  sprawling mealy shrublet with opposite spoon leaves, sea aster with a fleshy rosette and a
  corymb of lilac daisies (a quarter rayless), sea lavender with zigzag sprays of one-sided spikes,
  sea kale as a mound of big cupped, waved, lobed leaves with flat flower heads then pods, sea
  campion as trailing mats with bladder calyces and notched petals, horned-poppy with lobed grey
  rosettes, silk poppies and long curved pods.
- `make_saltmarsh(SALTMARSH_PIONEER | SALTMARSH_MID | SALTMARSH_HIGH | SALTMARSH_SHINGLE)`; the
  mid and high swards reuse dry grass in saltmarsh-grass and fescue colours, sea rush is the rush
  preset in its grey-green.
- Three rules, all from the map's geology source when it has a sea: `RULE_TIDE_HEIGHT` (ground
  over `sea_level + height_offset`), `RULE_COAST_EXPOSURE` (`get_coast_exposure_at`) and
  `RULE_BEACH` (`get_beach_at`). A species using any of them is not placed without a sea. Marsh:
  exposure under 0.3, slope under 3 degrees. No beach rule for the marsh: a scout of the showcase
  map (17 Nov) found every sheltered, level cell in the tidal band under the generator's beach
  mask (it runs along every coast), and wave energy, not grain size, sets where marsh forms
  (Allen 2000, "Morphodynamics of Holocene salt marshes"); a sheltered beach head fines to mud.
  Shingle: beach over 0.15, 1.8 to 4 m, slopes to 15 degrees.
- Seasons: phenology per preset (marsh late, shingle from June); glasswort reddens from late
  August to purple-red by October and browns into winter; cord-grass spikes and sea lavender
  calyces stand through winter. Cord-grass sheds pollen (`VegetationPollen3D`).

## Ground mist and pollen with PhysX Flow (built 17 Nov)

Two things in the air just above the plants, both carried by Flow (docs/ideas/physx.md) so they swirl
round the bodies that walk through them and drift with the ground wind instead of hanging as cards.

### Ground mist

What it is. Radiation fog: on clear, calm nights the ground and the grass tips lose heat by longwave
radiation, the air touching them cools to its dew point and condenses. It starts as a shallow layer
under 2 m (the WMO's "shallow fog", METAR MIFG), thickest in hollows because the cooled air is
denser and drains down slopes into them (cold-air pooling; Whiteman 2000, "Mountain Meteorology";
Clements et al. 2003, Peter Sinks). Fog needs relative humidity near 100 %, wind under about
3 m/s (stronger wind mixes the cold layer away) and little cloud; it lifts or burns off within one to
three hours after sunrise as the sun warms the ground (Roach et al. 1976, Cardington; Duynkerke 1991,
"Radiation fog: a comparison of model simulation with detailed observations"; Gultepe et al. 2007,
"Fog research: a review"; Price 2011, "Radiation fog. Part I"). Grass makes it first: the canopy top
is the coldest surface.

How thick it looks. Fog droplets are about 10 um; extinction follows liquid water content,
beta = 144.7 LWC^0.88 per km with LWC in g/m3 (Kunkel 1984). Mist (visibility over 1 km) is
beta under 0.004 per metre; thin ground fog 0.01 to 0.05; thick valley fog up to 0.2 (20 m
visibility). So a ground mist is hardly visible across a metre but closes off the view along the
ground over tens of metres; that is how it should read. Droplets that size scatter strongly forward
(Henyey-Greenstein g about 0.8), so mist glows when looking towards a low sun.

What games do. Red Dead Redemption 2 uses froxel volumetric fog with a height layer and noise
(Bauer, SIGGRAPH 2019 Advances, "Creating the atmospheric world of RDR2"); Unreal has Exponential
Height Fog plus Local Fog Volumes; Frostbite unified froxel volumetrics (Hillaire 2015). All of
these are static density with scrolling noise: nothing parts round a walker or rolls into a hollow.
The near field here is a real fluid instead; the wide haze beyond it stays with the environment's
fog (Valence's weather side).

Build: `VegetationMist3D`, in the vegetation module, using Flow by class name (no build
dependency on PhysX; without Flow it warns and stays inert).
- One `PhysXFlow3D` with 25 cm cells, no combustion, no heat, heavier-than-air gas
  (`smoke_buoyancy` -0.15: a 1 to 2 % density excess of air a few degrees colder), a slow fade
  (evaporation), little vorticity (calm air), terrain floor on, forward scattering 0.75, white.
  `smoke_density` is set so one unit of smoke is the extinction the weather asks for.
- A ring of flat box emitters on a world-snapped 8 m grid round the camera (or a target), each
  sitting on the ground and coupling gently towards its cell's mist level, so the layer is held
  where it should be and anything that stirs it recovers over seconds. Each cell's level is the
  zone's mist amount times its propensity: hollows (the ground below the mean of a ring 8 m out)
  get more, crests less, wet ground more.
- The mist amount comes from the weather when a `WeatherMap3D` is set: humidity over 0.88,
  wind under 3 m/s at 10 m, little cloud, and the sun under 15 degrees (it burns off as the sun
  climbs). `mist_amount` overrides it.
- One wide box emitter puts the ground wind (0.6 of the 10 m wind) into the air with no smoke, so
  the mist drifts and curls round stems' bodies.

### Pollen puffs

What it is. Wind-pollinated plants (grasses, sedges, rushes, reedmace, nettle, docks, plantain)
hold ripe pollen in dangling anthers; a shake releases a puff. Release needs a jolt past a threshold
(Urzay et al. 2009, "Wind gusts and plant aeroelasticity effects on the aerodynamics of pollen
shedding", J Theor Biol), and walking through a meadow in flower lifts visible pale clouds. Grass
pollen grains are 25 to 40 um and settle at 2 to 3 cm/s (Gregory 1973; Klahs 2019 for
Diarrhena), so a puff hangs and drifts for many seconds and fades as it spreads. Anthesis is in the
morning on warm, dry days; plants shed only while in flower (`PlantGenerator.get_bloom_shares`).

Build: `VegetationPollen3D`.
- Species made by the `make_*` functions carry their preset (meta `plant_preset`) and, for the
  wind-pollinated ones, a pollen amount (meta `pollen`).
- It watches the bodies in the trample group (the same movers that press the grass). For each one
  moving faster than a slow walk, it looks up flowering plants of pollen species within reach in
  the `VegetationField3D` placements (a spatial hash rebuilt when the placement changes) and fires a
  short puff at their flower height: smoke by pollen amount, share in flower and speed, the jet
  along the mover's motion and a little up.
- One `PhysXFlow3D` with 4 cm cells, cream-yellow, very slightly heavy, slow fade, more swirl; a
  small pool of sphere emitters.

Checks: the bench `--mist` runs a dawn hollow with mist and a walker through it; `--pollen`
walks a body through flowering cocksfoot and timothy. Tests check that mist gathers more in the
hollow than on the crest, that it clears with the sun high or a strong wind, that puffs fire only
from plants in flower and only when brushed, and that both nodes stay inert without Flow.

## Alpine and scree plants (built 17 Nov)

Above the treeline the ground is not turf. The alpine belt starts where the growing season's mean
soil temperature falls to about 6.4 C, which is about 0 to 2 C mean annual air temperature in the
Alps (Körner and Paulsen 2004, "A world-wide study of high altitude treeline temperatures"; Körner
2003, "Alpine Plant Life"). The plants are low, because the air within 5 to 10 cm of the ground is
several degrees warmer than the air above (Körner and De Moraes 1979, cushion microclimate). They
sort by snow and wind along the micro-relief (Billings and Bliss 1959; Gjaerevoll 1956):

- Fellfield, the exposed convex ridges blown free of snow: cushion plants (moss campion, Silene
  acaulis: a hemispherical dome 5 to 30 cm across of densely packed shoots, each a tiny rosette of
  linear leaves, pink flowers sessile on the surface; Morris and Doak 1998) and purple saxifrage
  mats (Saxifraga oppositifolia: trailing stems with four ranks of tiny opposite leaves, purple
  cups the first flowers after the thaw), and Dryas octopetala mats on lime (Elkington 1971,
  Biological Flora: scalloped leaves, dark above, white-felted below, eight white petals, then
  twisted feathery plumes).
- Alpine grassland on the slopes: curved sedge (Carex curvula, "Krummseggenrasen" of acid soils:
  dense tufts of thin curved leaves whose dead tips curl and brown the sward; Grabherr 1987).
- Snowbeds, the hollows where snow lies into July: dwarf willow (Salix herbacea, the smallest
  tree: underground stems sending up two to four round, scalloped, glossy leaves 0.5 to 2 cm, tiny
  red catkins; Beerling 1998, Biological Flora) carpets with moss (Björk and Molau 2007).
- Scree: Jenny-Lips (1930) sorted scree plants by how they live with moving stones: wanderers
  creep over and through it (alpine toadflax, Linaria alpina: trailing glaucous stems with whorls
  of fleshy linear leaves, violet snapdragon flowers with an orange palate), stabilisers hold it
  with mats (purple saxifrage), coverers lie over it (Dryas). The existing crevice set
  (`make_stones(STONES_SCREE)`) stays for the gaps between blocks.

Games: KCD2 and Crysis paint upland grass and stop; the alpine look most shooters show is turf with
rocks. Real geometry for cushions follows the shoot structure: a closed dome core (so it never
reads hollow) under a surface of packed rosettes, the way a cushion is built (Körner 2003).

Plan:
- Six presets: moss campion, purple saxifrage, mountain avens, curved sedge, dwarf willow, alpine
  toadflax. Phenology from snowmelt: early flowers (saxifrage) on low degree-day sums.
- `make_alpine(ALPINE_FELLFIELD | ALPINE_GRASSLAND | ALPINE_SNOWBED | ALPINE_SCREE)`, placed above
  the map's own treeline (see below), by curvature (convex ridges for fellfield, hollows for snowbeds), sunlight (snowbeds on the shaded
  side), talus (scree) and slope.

### Fix 17 Nov: the treeline from the growing season, not the mean annual

The first build used a mean annual band (-8 to 1.5 C). The showcase's climate (January 1 C, July
17 C at the coast) leaves its 1180 m summit at 2.8 C mean annual, so nothing placed, yet real
oceanic mountains at that climate carry alpine heath far lower (Scotland: treeline about 600 to
650 m, alpine heath on summits whose mean annual is 0 to 3 C). The mean annual is the wrong
measure: mild winters raise it without warming the summer that trees grow in. The global treeline
model (Paulsen and Körner 2014, "A climate-based model to predict potential treeline position
around the globe", Alpine Botany) puts it where the growing season, the days with a mean over
0.9 C, averages 6.4 C, with at least 94 such days. That one rule gives the Alps' treeline near
2000 m and Scotland's near 650 m. Wind lowers it further on exposed oceanic ridges (Grace 1989;
Crawford 2008, "Plants at the Margin").

So `PlantSpecies.RULE_SEASON_TEMPERATURE` works the growing-season mean out from the map's January
and July means on the annual harmonic (closed form: the season is where a - b cos(theta) >= 0.9;
its mean is a + b sin(t0) / (pi - t0)), only for species that use it. The alpine sets take it
under 6.4 C, faded 0.8 C; fellfield 1 C lower down for the wind. On the showcase climate that
places alpine plants from about 950 m, fellfield on ridges from about 850 m.

## Dune plants on generated dunes (built 17 Nov)

The terrain generator now builds coastal dunes (TerrainGenerator3D `dunes_enabled`, on): Werner's
slab model with marram after Nield and Baas 2008, so each dune cell carries a stability, the
marram cover that holds its sand (0 bare and moving, 1 fixed). That is the ecological sequence the
dune sets stand for (embryo dunes on the strandline, mobile yellow dunes under young marram, fixed
grey dunes behind where the sand has stopped; Ranwell 1972, Maun 2009), so it places them better
than distance from the water alone.

- `PlantSpecies.RULE_DUNE` (`dune_range`, `dune_fade`): the generator's `get_dune_stability()`, or
  -1 where it has less than 0.3 m of dune sand. Applied only when the map's geology source has
  built dunes; elsewhere the dune sets keep placing by distance from the water as before.
- `make_dunes()`: embryo 0 to 0.3, mobile 0.15 to 0.8, fixed 0.6 to 1 (fade 0.1); their water
  distances widened (embryo to 40 to 60 m, mobile to 250 m, fixed from 30 m) so the generator's
  foredunes, tens of metres behind the beach, fall inside them.
- Blowouts and parabolic dune noses, where cover failed, read as bare or embryo sand.

## Far colour matched to the plants on screen (built 16 Nov)

The showcase scene showed a lighter ring of ground where the cover plants (bracken, heather) end.
Scene isolated it by hiding one field at a time: with the cover field hidden the ring is gone, so
it is the cover plants darkening the ground within their range (their shadowed depths, leaves in
each other's shade, the dark soil between stems) against the far colour that replaces them, which
is only the species' leaf colour. Grass already solves the same problem for its far ground with a
live-view match (`GrassField3D.far_match_view`, the standard "match the impostor to the mesh"
step of far-field LOD, as in the far-grass colour pass of Horizon Zero Dawn and Ghost of Tsushima
talks); the cover plants now get the same.

- `VegetationField3D.far_match_view` (on): after the maps are built, it reads the rendered image
  back (asynchronously, as grass does) along 16 arcs round the camera. In a band inside the range
  (0.1 R to 0.34 R, where every plant is drawn) and one just past it (1.03 R to 1.3 R), six
  distances each, it takes per arc the most covered spot (far cover alpha 0.55 or more) and the
  barest (0.1 or less) on screen, within about 8 m of each other. The ratio of covered over bare
  near, against the same far, is what the plants darken the ground by that the far colour misses.
- The far colour where it stands for plants (and the far volume, which stands for the same
  plants) is scaled by `vegetation_far_shade` (a global, 0.3 to 1.2) until the ratio is within 3%
  (at most 8 rounds), and looked at again once the camera has gone a fifth of the range. Bare
  ground is untouched, so it is a pure per-cover darkening, not a tint.
- The far colour is only the diffuse part of the ground's light: sky reflection and haze do not
  follow it, so on dark ground a step moves the contrast far less than itself. Each round measures
  that gain (contrast ~ shade^-gain) and steps by the ratio's root (at most x0.6 or x1.5 a round).
  A gain under 0.05 means the ground there does not draw the far colour at all (no
  `vegetation_far()`, or something covers it): the shade goes back to 1 and the match stops.
- Vegetation bench `--far-match[=off|dark]`: on its humus ground the match took 3 rounds to shade
  0.63 (gain 0.10), contrast within 1.3%. Path traced frames are not measured (still converging,
  noisy without a denoiser): the raster shade stays.
- Needs 6 sample groups; fewer (no cover in view, or all one kind) retries, then gives up and
  keeps the last shade. `get_far_match_stats()` reports shade, ratio, groups, round, state.
- Past the vegetation far map, the terrain's far blend reads the cover map, whose cover also counts
  grass (matched on its own, `grass_ground_far_scale`) and crowns. So the shade is baked into the
  cover plants' share of the cover map only, which is rebuilt once the match settles on a new shade
  (more than 2% from the one it was built with).

## Waterside and wet-stone plants (built 16 Nov)

Picked by Paulo on 16 Nov (first of the round; the research is in "Next round candidates (16 Nov)").

- Five presets, real geometry (plant_waterside.cpp): `PRESET_MARSH_MARIGOLD` (kidney-shaped glossy
  blades on arching stalks, golden cups March to May, then follicle stars), `PRESET_MEADOWSWEET`
  (reddish stems to about 1 m, pinnate leaves with the small leaflets between the pairs, frothy
  cream cymes June to September, brown stalks all winter), `PRESET_BUTTERBUR` (lilac-pink spikes
  March to May before the leaves, then round blades 30 to 50 cm across on stalks to 75 cm, dying
  back in autumn; the spikes lengthen and go white in fruit), `PRESET_RIVER_MOSS` (Platyhypnidium
  and Fontinalis: dark leafy shoots along +X, still in the wind), `PRESET_LIVERWORT`
  (Conocephalum: glossy forked ribbons with a darker midline groove, on the opaque centre surface).
- Big blades are discs, not strips (`_add_disc_leaf`): a sinus where the stalk meets the blade,
  the margin drooping and waving, a fine saw of teeth, the leaf texture mapped inside its round
  outline. A 40 cm butterbur blade as a three-column strip could not droop or wave.
- `PlantSpecies.stone_top_water` (Vector2, m): a stone-top species keeps to tops whose height over
  the water is in the band. Over the water is the bed under the stone plus the map's water depth;
  on the bank, the top's height over the ground plus a quarter of its distance to the water. River
  stones: aquatic moss -0.6 to 0.1 m (with a `water_depth_range` reaching under water), liverworts
  0.03 to 0.4 m on damp tops, green moss above 0.12 m, the dry olive moss above 0.3 m.
- `PlantSpecies.follow_flow`: turned so +X runs down the fall line (water runs downhill), with a
  little scatter; random on level ground.
- Plants on a stone's top and plants on the ground round it no longer crowd each other's
  footprints (sedges at the stones' feet were keeping moss off the tops).
- `PlantGenerator.make_banks(BANKS_STREAM | BANKS_LAKE | BANKS_FALLS)`: marsh marigold -0.15 to
  1.2 m from the water, butterbur colonies 0 to 4 m (streams only: it needs moving water's
  gravel), meadowsweet 0.6 to 7 m; at falls liverworts where spray reaches 0.3 and marsh marigold.
  All in clonal patches, none colouring the far ground.
- Benches: vegetation `--stones=stream` (rock-generator cobbles across the river's edge, clear
  water, the stream's banks; `VEG_STREAM_BANK=1` looks along the bank) and the check "stone tops
  by water"; plants bench size checks, butterbur flowering before its leaves, the bank sets.
- Next: aquatic moss on the stones' upstream faces as well as their tops (a cap-only placement
  leaves one clump per cobble); reading the live water's height (PhysXRiver3D) instead of the
  baked map once rivers move.

## Plants between stones (built 15 Nov)

Crevice flora (chasmophytes) root in the fine soil caught between stones and in rock cracks: the
stones shade the roots, hold water under them and keep trampling and grazing off (Pigott 1956 on
limestone and scree flora; Rorison 1960). On open scree the plants are sparse and almost all in
gaps; the fern of acid scree is parsley fern, and maidenhair spleenwort holds the cracks (Page
1997, "The Ferns of Britain and Ireland"). Shingle above the strandline is colonised in its stable
gaps by thrift and biting stonecrop (Scott 1963, "The ecology of shingle beach plants"); upland
river gravels by mossy saxifrage, stonecrop and, near water, sedges and rushes.

- Five presets, real geometry: `PRESET_PARSLEY_FERN` (fertile fronds taller, rusty in winter),
  `PRESET_SPLEENWORT` (black rachis, paired pinnae, bare old rachises), `PRESET_STONECROP` (the
  fat succulent leaves are solid beads, 20 triangles each, on the stem surface), `PRESET_SAXIFRAGE`
  (a cushion of lobed rosettes, white flowers May to July), `PRESET_THRIFT` (a cushion of grass-like
  leaves, pink heads April to September, papery after).
- `PlantSpecies.stone_gap`: the suitability is multiplied by `lerp(1, g, stone_gap)`, g = hug x gap:
  hug is 1 within 8 cm of the nearest stone's edge and 0 past 20 cm (the plant roots against a
  stone, in the crack where fines and water collect); gap = exp(-d / 0.12) shifted to reach 0 at
  0.48 m, d the distance to the second-nearest obstacle's edge. So the gap between two stones
  counts, and open sand and the foot of a lone boulder (`rock_foot`) do not. At 1 the species
  grows nowhere else. (First version, 15 Nov: gap only, reach 0.15 m to 0.6 m; on real scree
  it left plants 20 to 40 cm out on open sand, which read as weeds on sand. Crevice plants' leaves may now reach 18 cm in over a stone,
  arching over it, before they are made smaller.)
- `PlantGenerator.make_stones(STONES_SCREE | STONES_RIVER | STONES_SHORE)`: the sets.
- The rock generator's `RockField3D.get_obstacles(rect, min_size)` gives the same (x, ground y, z,
  radius) list `VegetationField3D.obstacles` takes.
- Moss on stone tops (built 15 Nov): `PlantSpecies.stone_top` species grow only inside
  `VegetationField3D.stone_tops` caps (x, y, z, radius), skipping the obstacle and crevice rules.
  Each cap comes from `RockField3D.get_stone_caps()` (rocks module, from the library mesh, no
  physics): its point, normal and the radius the up-facing surface stays within 1 cm of that plane.
  `stone_top_normals` carries the normals; a plant sits on the cap's plane, 3 mm in. Without
  normals a cap is a dome of its radius (a first ray version on the collision hulls was dropped).
  The scree and river sets add moss cushions on tops (spacing 9 cm, so they clump into patches).
  Scree has no ground moss any more: on sand between blocks it read as moss on bare sand, and
  scree mosses (Racomitrium, Andreaea) grow on the stones.
- Sun and shade on stone tops (16 Nov): boulder bryophytes sort by the top's microclimate. Exposed,
  high, sun-facing tops dry out between rains and carry hoary, yellow-olive xerophytes (Racomitrium
  lanuginosum, Grimmia); low, shaded, pole-facing tops keep the ground's humidity and carry deep
  green pleurocarps (Weibull and Rydin 2005, boulder bryophytes and canopy).
  `PlantSpecies.stone_top_damp` (-1..1) weights a top as damp or dry, per stone: damp where
  1.0 a + 0.5 low + 0.7 shelter + 0.6 shade reaches 0.45 (jittered 0.05 per stone), all at the cap:
  a its tilt to north (-Z, as the rocks' moss `pole_direction`), low 1 at 5 cm over the ground
  falling to 0 at 0.8 m, shelter the steepest rise (tan, full at 45 degrees) to a taller stone top
  within 2 m (south side full, north half), shade the canopy. Binary per stone so a top reads green
  or olive; a first per-plant, graded version mixed both on sunlit tops (docs review).
  make_stones pairs green moss (+1 scree, +0.6 river) with a smaller, sparser yellow-olive one (-1).
- Next: the river sets reading the stones' water film.
