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
kermes oak cushions and young gum rockrose in the garrigue; dwarf fan palm in both where January averages 7 C or more. Two variants each: mature gorse is
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

## Next round candidates (17 Nov evening): farmland, burnt ground, arid ground, climbers

Researched for the next pick; nothing built yet.

**Farmland: crops by season, stubble, field margins and hedgerows.** The open-world bar (KCD2) is
mostly cultivated land, which the fork has none of. Crops grow as rows of real plants whose look
is set by the growth stage (the BBCH scale: tillering, stem elongation, heading, ripening; winter
wheat sown in October, green by April, golden in July, stubble after harvest), so a field reads by
season like the meadow does. Rendering follows the grass path: GPU-filled rows on a field's own
grid and drill direction (sowing lines show from afar), wind as a damped oscillator (honami waves
roll across a ripe field), far colour by stage (Serious Sam 4, GDC 2019, "4 million acres":
fields as instanced crop rows with terrain colour matched under them). Field margins carry
poppies, cornflowers, mayweed; hedgerows are a linear stand of hawthorn, blackthorn, hazel and
bramble (trees thread for the woody part) with a grassy bank. Cost: rows are dense (60 to 400
stems per m2), so the near band needs the same GPU fill and far colour as grass.

Farmland plan (Paulo picked all three in order, 17 Nov 19:02Z; farmland first):
- Facts. Winter wheat: drilled in rows 12 to 15 cm apart at 250 to 350 seeds/m2, tillers to 450 to
  650 ears/m2; modern straw 0.8 to 1.0 m, heritage and medieval wheats and rye 1.2 to 1.8 m
  (lodging in storms). Stages on the BBCH (Zadoks) scale drive the look: emergence and tillering
  (low blue-green rows, soil showing, Oct to Mar), stem elongation (closing canopy, Apr to May),
  heading and flowering (ears out, anthers, June), grain fill (green ears turn yellow from the
  top, glaucous to golden, July), ripe (straw gold, ears nodding in barley, upright in wheat),
  harvest leaves stubble 10 to 20 cm with straw rows, then ploughed brown soil. Barley: long awns
  that bend over; oats: open drooping panicles; rye: tallest, blue-grey. Modern fields show
  tramlines (paired wheel tracks every 12 to 24 m) and headlands turned at the ends; medieval
  open fields are strips (selions) about 200 m long and 5 to 20 m wide in furlongs, ploughed into
  ridge and furrow 0.3 to 0.6 m high (Wikipedia "Selion", "Open-field system").
- Margins and hedgerows (hedgelink.org.uk "Hedgerow components"): arable margin weeds strongest in
  the outer 2 to 6 m (poppy, cornflower, corn marigold, scentless mayweed, charlock), a grass
  strip, then the hedge: 2 to 4 m tall and wide, hawthorn and blackthorn with hazel, field maple,
  dog rose and bramble, on a bank with a ditch, standard oaks every 50 to 100 m.
- Engine. Stage 1: crops as real geometry built like the grass blades, a CropField3D on the
  GrassField3D tile and GPU fill code: each record a tiller (stem, two to four leaves, the ear or
  panicle as a short swept mesh), bent by the same damped-oscillator wind so ripe fields roll in
  waves; rows along a drill direction; near band real, then the far colour by stage. Stage 2: the
  field layout (polygons with crop, sowing day and drill angle; tramlines, headlands, stubble and
  ploughed soil; ridge and furrow for strip fields) and the seasons (BBCH from the WeatherMap3D
  day). Stage 3: margins (a make_margin set) and hedgerows as polylines like paths, woody
  shrubs from the trees thread, a bank and ditch through the ground module or terrain.
- Stage 1 built (17 Nov): `GrassField3D.crop` (wheat, barley, oats, rye) rather than a separate
  node, so crops share the tiles, rules, paths, trample, wind and far bake. Each record is a tiller:
  stem (5 segments, turned to the view), four distichous leaves (Bezier arch, twist, taper, lowest
  die first), the ear as a 4-sided flattened ring with spikelet creases, awns as single triangles
  (barley 14, rye 18), oats as 12 whorled branches with hanging spikelets; about 66 to 100
  triangles, a 30-triangle mesh for levels 2 and 3. Rows: plants on each drill row at the density
  over two tillers a plant, 7% failed seeds, a golden-ratio rank along the row (thinning keeps the
  spacing even), rows of a 3 m drill bout wander together. `crop_stage` (BBCH) is uniforms only:
  leaf scale, stem share, ear emergence 49 to 59, ripening 75 to 91, leaves dying from the bottom
  55 to 85, nod (barley 1, rye 0.45, oats 0.35, wheat 0.15), stubble 15 cm at 99. Specular
  antialiasing (Kaplanyan et al. 2016, via fwidth since the path tracer's translation has no
  dFdx) stops the waxy leaves sparkling. Cost: 400 tillers/m2 at tile_size 1.5 m keeps a tile under
  the 65536-triangle merged BLAS cap. CPU fill for now; GPU fill and a soil ground for crops come
  with stage 2.
- Stage 2 first part built (17 Nov): `GrassField3D.fields` (polygon, drill angle, stage or
  sown/harvest day, tramline spacing and gauge, headland). Tramlines skip drill rows over the
  wheel tracks; headland rows run parallel to each edge, mitred at the corners by nearest edge.
  Stage per field is a uniform array (instance y carries the slot), so the season moves fields
  without rebuilding tiles; a sowing calendar (AHDB growth guides) gives the stage by day. The
  ground: every crop node's fields in one shared map (`crop_ground_map`, 0.5 m texels, slot and
  wheelings) and a 256x2 look table; `crop_ground()` in the ground shader draws drilled soil with
  slot lines and darker wheelings, the colour of tillers dropped by the coarser levels, and past
  the band the crop's colour and cover by stage. Still to do: ridge and furrow and ploughed soil
  (needs terrain heights: terrain thread), GPU fill for crops.
- Stage 2 second part (18 Nov, after docs declined the first frames): ears as real spikelets
  (two alternate rows of lozenges on a slim rachis, glumes shaded darker where they tuck under;
  barley's awns from each spikelet's tip), oat panicles in whorls of three with drooping
  spikelets, wind lean scaled per tiller so a stand is not a comb. The far canopy colour is
  measured, not guessed: the grass bench's `CROP_FAR_ONLY=1` draws only the ground, and
  `crop_canopy_look()` is set so its screen mean matches the tillers' over the same ground
  (darker and more saturated than the leaf colour: the gaps are in shade), and the ground under a
  closed canopy has no sheen. Straw swaths: after harvest a swath of loose straw down the middle
  of each combine pass (a third of the tramline spacing, 1.4 m wide, heaped in the middle) as
  lying straw records (plot slot + 64), and as paler stripes on the far ground. The meadow and
  VegetationField3D plants keep out of every crop node's fields (`get_crop_field_mask()`, group
  `vegetation_crop_aware`).
- Stage 2 third part (18 Nov): plot stages moved from a uniform array to a 64x1 texture (a
  float[64] pads to 1 KB in std140 and overflowed the path tracer's 512 B material slot, so PT
  read every plot as green). Soil gaps close with view slant (Nilson gap fraction:
  cover = 1 - (1 - cover)^(1/mu)). The far canopy's sun lobes come from the first crop node's own
  far bake (`crop_ground_sun`, `crop_ground_lobe1/2`), so far and near agree against and with the
  sun. Past the near band the crop is a relief canopy: parallax occlusion mapping (Policarpo et
  al. 2005; Tatarchuk, "Practical parallax occlusion mapping", 2006) marches the view ray back
  through a canopy height field (rows, clumps, ears, height by stage from
  `crop_canopy_height()`), so tramlines read as lanes cut into the crop with shaded walls rather
  than painted stripes, and the canopy top shades by depth into it.
- Far crop matched live (18 Nov): raster draws the tillers much brighter than the path tracer (no
  shade inside the stand), so a fixed far colour cannot match both. The first crop node now runs
  the meadow's live-view match (`far_match_view`): pairs of screen points in the same field, one at
  half the tillers' radius, one just past it, read back asynchronously; the far canopy is scaled
  per channel (`crop_ground_view`) until they agree, up to five rounds ten frames apart.
- Stage 3, margins and hedgerows (18 Nov). `PlantSpecies.RULE_FIELD_EDGE` (`field_edge_range`):
  metres from the nearest field's edge, negative inside; a species using it may grow in the crop.
  `PlantGenerator.make_margin()`: `MARGIN_HEADLAND` (corn poppy, cornflower, scentless mayweed,
  from 5 m inside the crop to the edge, where most of a field's weeds are: Marshall 1989; Wilson
  and Aebischer 1995) and `MARGIN_VERGE` (cow parsley, cocksfoot, nettles at the hedge foot,
  knapweed, 0.3 to 2.8 m out). `GrassField3D.hedges` (on): the trees thread's hedge segments
  (TreeGenerator `SHRUB_HEDGE`, 2 m long) laid end to end `hedge_offset` (3.5 m) out from each
  field's edge, turned along it: hawthorn 55%, blackthorn 15%, hazel 12%, field maple 10%, dog rose
  8%, in runs of about three segments (the common mixed hedge; Pollard, Hooper and Moore 1974);
  a boundary two fields share gets one hedge; one 6 m gateway per field on its longest side.
- Far field, what else is out there (18 Nov, Paulo asked). Unreal's Nanite Foliage (experimental)
  voxelizes a mesh's clusters into 4x4x4 voxel bricks where that simplifies with less error than
  triangles, each voxel keeping a normal distribution sampled per pixel, drawn front to back in
  depth buckets: distant canopies stay volumetric geometry rather than cards. AMD's research
  (2026) traces about 25,000 animated plants by deforming tetrahedral cages round static dense
  meshes (BVH memory 47 times smaller); NVIDIA's path-traces 60 million plants with bone-merge
  LODs, thin-preserving edge collapse, opacity micromaps and a partitioned TLAS. For a crop the
  canopy is a height field (one surface over the ground), so the parallax canopy already gives
  what a voxel brick would; voxel bricks pay off where the far shape is not a height field: hedges,
  woodland edges, tree crowns. Hedge segments are 70k to 330k triangles each, so a far LOD for
  them is the next real need.
- Meadow clumps are Voronoi cells (nearest of jittered seeds), not grid squares, which showed as
  blocks of lighter and darker grass.
- References for rendering: Serious Sam 4, GDC 2019 ("4 million acres", crop fields as GPU
  instanced rows); Ghost of Tsushima, GDC 2021 (blades and wind); Butler and Ebert, "Procedural
  field grasses".

**Burnt ground and recovery (with Flow fire).** After a heath or scrub fire the ground is black
with ash and charred stems standing; heather regrows through pioneer, building, mature and
degenerate phases over 6 to 30 years (Gimingham 1972, "Ecology of heathlands"), Mediterranean
scrub resprouts (kermes oak, lentisk) within months while seeders (rockroses) germinate from the
soil seed bank (Calvo et al., "dynamics of Mediterranean shrub species after fire"). A `burn`
age per area would swap sets: ash and char, then resprouts and seedlings, then the full stand.
Ties to PhysX Flow fire and the trees thread's charred wood.

**Arid ground: desert and steppe plants for the new sand seas.** Terrain built desert sand seas,
which have no plants yet. Dune vegetation sorts by sand mobility (crests bare; tussock grasses such
as Stipagrostis on flanks and interdunes; shrubs on stable plinths), and stable desert ground is
covered by biological soil crust, a dark, knobbly skin of cyanobacteria, lichens and mosses that
crunches under tyres and stays broken for decades (Belnap and Lange, "Biological soil crusts";
Kidron, crust dynamics on dunes). Plants: Stipagrostis tussocks, saltbush, sparse annuals after
rain, crust as ground detail that the sand and ground modules can break.

**Climbers and ivy carpets.** Ivy as ground cover in woods and over rocks and walls, its leaves
juvenile-lobed on the ground and entire on flowering shoots; ivy and old man's beard on trunks
(the trees thread owns the stems, this thread the leaves). Placed on rock faces and tree trunks
by their surface normal and shade.


Live fires (19 Nov): the fire contract's change log (WorldFire.burn_changed(area), get_burn_generation()) is
checked twice a second. A streamed field samples its burn grid again over the changed area, once no tile is
being placed, and places the tiles over it again (as for cave holes); a populated field populates again.
Bench vegetation `--burn=live`: 23 plants, 0 on the fresh burn, 11 at half the recovery, 23 once it goes.
Replaced tiles (burn or cave holes) keep showing until their replacement is placed: StreamTile.stale, and when
the new tile comes back the species the old one showed are committed and the old instances freed in the same
frame, as grass swaps its tiles. The bench counts the unburnt plants beside the scar every frame: 680, never fewer.

## Arid ground: plants and crust on the desert sand seas (Paulo's pick 17 Nov; built 18 Nov)

The terrain generator's sand seas (`TerrainGenerator3D.sand_sea_rect`, Werner slab dunes) are bare. Desert dune
vegetation sorts by how much the sand moves, much as coastal dunes do by marram cover, but with no plant building
the dune: crests and slip faces move every windy day and stay bare; the lower flanks and plinths move less and hold
perennial tussock grasses and shrubs; the interdune corridors (thin sand over gravel or silt) are stable and carry
shrubs, the rain-flush annuals and a biological soil crust (Tsoar 2005 on dune mobility and vegetation; Danin 1996,
"Plants of Desert Dunes"; Kidron and others on crusts of Negev and Saharan interdunes; Belnap and Lange 2003,
"Biological Soil Crusts").

Plants (North African and Middle Eastern ergs, the best studied):
- Drinn (Stipagrostis pungens): the dune grass, rigid grey-green rolled leaves in dense tussocks 0.5 to 1 m that
  trap sand into hummocks; tolerates burial; on flanks and mobile plinths, never on the crest itself.
- Shrubs on the lower flanks and interdunes: Calligonum comosum (leafless jointed green twigs, a broom-like bush 1 to
  2 m with red-bristled fruit), white broom Retama raetam (silvery leafless rush-like stems, white flowers in
  spring). Sand gathers round them into nebkhas (low mounds); the terrain does not build those yet.
- Saltbush (Atriplex halimus, A. canescens): grey mealy rounded shrubs on silty, saline interdune floors.
- Ephemerals ("acheb"): annual grasses and herbs that cover the interdunes for a few weeks after rain, then dry to
  straw; placed only when the season's soil water says it rained.
- Biological soil crust on stable interdune sand: a dark brown-black knobbly skin of cyanobacteria, lichens and
  mosses a few millimetres to 1 cm thick, rough and pinnacled in cool deserts, flat and smooth in hot ones; broken by
  feet and tyres into pale fresh sand that takes decades to heal.

Placement: `PlantSpecies.RULE_SAND_MOBILITY`, 0 stable to 1 moving, -1 outside the sand seas. From the sand sea's
depth on a 4 m grid: the depth's place between its local low and high over about one crest spacing, raised on slopes
steeper than 20 to 30 degrees (slip faces) and 0 under 0.3 m of sand (interdune ground). Sets by `make_desert()`:
mobile flanks (drinn), plinths and interdunes (drinn, Calligonum, Retama), interdune flats (saltbush, crust patches,
ephemerals). Crust as a flat dark knobbly preset placed like the forest floor's small items; the ground module may
later break it under wheels, as it does the sand.

Built: `PlantSpecies.RULE_SAND_MOBILITY` (read once a populate from the generator's sand sea), presets
PRESET_DRINN, PRESET_DESERT_ANNUALS and PRESET_SOIL_CRUST, and `PlantGenerator.make_desert(DESERT_FLANKS |
DESERT_INTERDUNE)`. The crust skin fades to the sand's colour at its rim, so no disc edge shows. Checked in
docs/bench/vegetation `--desert`: no plant on a crest or slip face, the flanks' drinn on the flanks, crust only on
stable sand, annuals between the dunes. Cost, 120 m streamed round a walker on lavapipe at 640x360, on against off:
+9.5 MB memory, +1 MB video, +134k primitives, +604 draw calls, walking worst 0.44 ms (0.03 off), raster frame
504 ms against 162 ms. Path traced cost goes on the PC queue.

Woody shrubs (19 Nov, trees' 16f1f982): `make_desert(..., shrubs = true)` adds calligonum (mature on the flanks,
mobility 0.1 to 0.6; young in the corridors), white broom and saltbush (corridors, saltbush only the most stable
flats), sparse and grouped. Bench VEG_DESERT_SHRUBS=0 for the cost: with against without, +3.8 MB memory,
+267 MB video, +1.78 M primitives, +19 draw calls, raster frame 592 ms against 505 ms. The shrub meshes are heavy
(about 70k triangles each in view): asked trees about LODs/impostors for them. The desert bench now reads mobility
bilinearly, as the field does (nearest-cell reads failed after the sinuous brinks, 81cae021).

Nebkhas (19 Nov, terrain-gen's cf579682): the sand mounds a desert shrub gathers round itself (Tengberg 1995; Khalaf
et al. 1995; Wang et al. 2006). TerrainGenerator3D.get_nebkhas(rect) gives each mound's top and height; with shrubs,
`make_desert(DESERT_INTERDUNE)` adds three mound shrubs (calligonum, saltbush, white broom, PlantSpecies meta
"nebkha"), and VegetationField3D crowns every mound with one of them (picked by the mound's hash), its crown 0.4 of the
mound's radius (radius about 2.7 times its height). The desert shrubs placed by density keep 2 m off a mound's foot.

## Oases at desert springs (Paulo's pick 19 Nov)

Trees' date palm oasis stand (TreeGenerator.STAND_DATE_OASIS, trees.md 5ai) placed where the sand seas hold water,
with the desert shrubs round it and a green sward on its damp ground.

Where oases are (research):
- Ergs hold water where the ground dips to the water table. The Badain Jaran desert has over a hundred permanent
  lakes in the hollows between its megadunes, fed by groundwater and springs in the lake floors (Yang and Williams
  2003; Dong et al. 2016); the Ubari sand sea in Libya has its palm-ringed lakes in interdune corridors (Mandara,
  Gaberoun). The Western Desert's oases (Siwa, Kharga) sit in hollows the wind scoured down to the water table
  (Said 1990).
- The water table is a subdued copy of the land's shape (Haitjema and Mitchell-Bruker 2005): flat under the dunes,
  so the deepest hollows reach it first and a spring's water stands at one level round it.
- Belts out from the water (Badain Jaran and Saharan oasis surveys): reeds at the edge, then the damp salt meadow
  and tended palms with fruit trees beneath (Battesti 2005; Zaid and de Wet 2002: the palm wants "its feet in
  water"; most absorbing roots in the top 2 m, reaching groundwater a few metres down), then a ring of shrubs
  (tamarisk, calligonum, white broom, saltbush) where the water is deeper, then bare sand.
- Biological soil crust does not form on wet, worked ground.

Model (VegetationField3D and GrassField3D read the same, from the generator; no private copy of a water state the
water systems own):
- Springs: the terrain's (see "One spring list" below; first built here as a hollow search, now removed).
- `PlantSpecies.RULE_SPRING` is the wetness, 1 where the
  ground lies at or under that level, falling to 0 where it stands 5 m above it (out of a palm's root reach), and
  to 0 by 250 m out; -1 off the sand seas. It follows the corridor's shape, since the dunes' flanks rise out of it.
- `make_desert(DESERT_OASIS)`: the oasis stand (tended palms 0.55 to 1, wild palm clumps by the water 0.8 to 1,
  olives beneath 0.5 to 0.95) and a shrub ring (calligonum, white broom, saltbush, 0.08 to 0.6). Crust and the
  annuals keep to wetness under 0.4. A species using the rule grows on the sand like a sand plant.
- GrassField3D lets its sward onto the sand sea where the wetness passes 0.5 to 0.85.
- One spring list (19 Nov): the terrain's (`TerrainGenerator3D.get_springs()`, its lowest interdune floors, which it
  carves to the water system's pools when `oasis_springs` names the OasisSprings3D). The field's own hollow search
  is gone, so the palms, the pools and the carved ground cannot disagree; `VegetationField3D.get_springs()` passes the
  terrain's list on (discharge 0: the pools' default).
- Round the pools (`VegetationField3D.spring_pools` to the OasisSprings3D; `PlantSpecies.RULE_POOL_DISTANCE`, metres
  from the shore by `get_outline()`, negative in the water): common reed from 2 m into the pool to 1.2 m up the shore,
  rushes 0.6 to 4.5 m behind (Phragmites australis and Juncus rigidus / maritimus fringe Egypt's oasis pools and
  springs: Zahran and Willis 2009, "The Vegetation of Egypt"; Ozenda 1977 for the Sahara).

## Boreal taiga floor (trees' STAND_BOREAL_TAIGA, 20 Nov)

The Fennoscandian and Russian boreal forest is classed by its floor (Cajander's forest site types; Kuusipalo 1985,
Oksanen and Ahti 1982): the floor is the stand's signature, and the tree species follow the soil.
- Dry pine on sand and gravel (Cladonia type, CT; Calluna type): a near-continuous carpet of reindeer lichens
  (Cladonia rangiferina, ashy grey, C. stellaris, yellowish domes 5 to 10 cm high), lingonberry and crowberry,
  heather in the open, sparse feather moss in the crowns' shade (Kumpula et al. 2000; Kershaw 1977).
- Mesic spruce (Myrtillus type, MT): a closed mat of feather mosses (Pleurozium schreberi, Hylocomium splendens) with
  bilberry, a little wood sorrel, needle litter, few lichens: shade and the moss's water hold them back.
- Birch in burn patches: the post-fire pioneers (birch, aspen, fireweed, wavy hair-grass) on bare mineral soil and
  birch leaf litter; lichens take 30 to 60 years to return after fire, feather mosses 20 to 40 (Schulten 1985,
  Kumpula 2000s; Nilsson and Wardle 2005).
Pine and spruce read apart by the crowns' shade: pine stands are open (light through the crowns), spruce dense.

Build: a reindeer lichen preset (PRESET_REINDEER_LICHEN): a lumpy, cauliflower-like dome of packed branch tips,
pale ashy grey-green, a few branched podetia standing proud at its rim; slow to recover from fire (40 years), crushed
underfoot. FLOOR_BOREAL_TAIGA: lichen under open conifer crowns (shade under 0.5, conifer over half), feather moss
and bilberry under dense ones, crowberry and heather in the open, wavy hair-grass and fireweed where the crowns are
broadleaf (the birch patches), birch litter there, needles and twigs under the conifers; fly agaric, brittlegill.

## Far vegetation without bald ground (Paulo 19 Nov: "at a distance it's bald")

What the showcase stills show: past 30 to 45 m (each plant layer's visibility range) the meadow, heath and scrub read
as a smooth flat green; the far grass rings and shell (to 500 m) and the plants' far volume (to 150 m) carry the
colour but almost none of the structure (clumps, tussocks, flower and dry patches, the shadows between them). Real
ground cover keeps a grain to the horizon: clumps cast shadow, mixed species show as colour patches, the sward's sheen
changes with slope and wind.

Published ways to carry it:
- Real geometry, thinned: Ghost of Tsushima (Wohllaib, GDC 2021) keeps procedural blades out to hundreds of metres by
  widening fewer blades with distance; Jahrmann and Wimmer 2017 the same on the GPU. Costly but the best match.
- Voxelised far foliage: UE 5.7's Nanite foliage (State of Unreal 2025, the Witcher 4 demo) turns far plant
  instances into voxel aggregates that keep their silhouette and porosity to the horizon; our hedges already do this
  (far voxel bricks) and the plants' far_volume shell is the same idea at 150 m.
- Vegetation-aware terrain shading: the placement's density feeds the terrain a macro normal, AO and colour variance
  so the ground past the geometry keeps the clumps' light and shade (Serious Sam 4, "4 Million Acres", GDC 2019;
  Pesce 2024, "Making a greener grass", far grass normals returning to the terrain's with a sheen term).

Plan (card to Paulo):
1. Far shell and far volume with structure: per-cell height from the placed plants' own clumping (patch noise the
   placement uses), colour from the species actually there (flower and dry patches), AO in the gaps, the fibre
   sheen as a far term. Match ground detail's and trees' far handoff distances.
2. Shrubs, heath and scrub as voxel clusters out to 1 to 2 km (as the hedges), so slopes keep their dark patches.
3. A density map for the terrain material (owner: terrain material) to darken and roughen the ground under far
   cover: only the provider side here.
4. Measure each layer's end on current main first (the p2 and v10 stills may predate the far shell).

Paulo picked the plan with real geometry carried further (19 Nov). Measured on main first: the grass bench with its
far rings and shell keeps the sward's grain to the horizon (rings to 250 m, shell to 500 m), so the bald look is the
plant layers': a flower at 15 m range thinned from 5 m and was gone at 15 m, a 30 m tussock at 30 m, and past that
only the far volume (off in several benches) or the ground's colour.

Step 1, the plants' far tail (VegetationField3D.far_reach, default 3): a streaming species keeps all its plants to
half its range, then by rank the share (r/2 / d)^2, the survivors grown by its inverse square root up to 1.6 times
(Ghost of Tsushima's widening), fading out over the last quarter of range times far_reach. Instance counts grow about
2.2 times over the old fade for a 15 m species with the default (the integral of the kept share), not the 9 times a
plain longer range costs, because each streamed tile's cells hold only the ranks its nearest point keeps
(_stream_rank(), a tile of slack) and are built again, new cells first, when the camera comes 30% nearer in keep. The
plant shader and the impostor share the curve (vegetation_far_scale(), custom x: rank in the fraction, the tail's end
in whole metres). Small items (detail_size) keep their pixel reach; non-streaming fields keep the old fade.

Step 2, the far shell (VegetationField3D.far_shell, to far_shell_range 4 km): past the streamed plants every ranged
species becomes one surface draped over the ground, the "canopy height model" of remote sensing: per cell each
species' cover is its suitability (with its patches) times density times far_color alpha; the cell stands at 0.4 of
their expected height by cover (lumped by a hash; low enough that GrassField3D's far rings and columns stand through
it), in their mean drawn colour by cover (_mesh_albedo(): base_color times vertex colour with the shader's worn-tissue
yellowing, so a summer sward is gold, not its leaves' green), darkened for the shade between tops and mottled at 3 m
and 23 m. Corners take the mean of the cells round them, so it thins down into the ground with no boxes (the first
try, voxel-like columns, read as scattered blocks). Cells 2 m to 400 m, 8 m to 1600 m, 32 m past it; it rises out of
the ground from 0.6 of where the plants stream. Shade-needing, sand, coast and pool species are left out.

What it does not replace: a sward's blades. Paulo's montado frame (19 Nov) was bald past 60 m because the floor set
alone has no GrassField3D; with the summer sward (rings to 250 m, columns to 500 m) blades show to the horizon. The
scene wires the sward under the cork and pine stands.

Step 3, Ghost of Tsushima's density curve on the sward (19 Nov, Paulo: "why aren't we just applying those
strategies?"): GrassField3D keeps full density to half its radius, then halves at 0.5 and 0.75 (_lod_for), and the
far rings start at a quarter of the near density (RING_BASE_SHARE 4, was 8). Meadow bench: primitives 7.6 M to 10.9 M
(+43%), +165 MB. The far shell's height and colour were tuned against it (specular 0.1, roughness 1: at grazing angles
the sky's reflection read grey; textured cards take their texture's mean colour). GPU timing is on the PC queue.

### Next round (card 19 Nov late)

1. GPU-built far blades. Ghost of Tsushima (Wohllaib, GDC 2021) and Horizon Zero Dawn (van Muijden, GDC 2017) build
   grass per frame in compute from tile masks, so far blades cost no stored mesh: a tile writes its visible blades into
   an indirect draw, and the count follows distance. Our far rings are CPU-built and cost 69 MB plus the GoT share. The
   step: a compute pass over the ring tiles reading the same rules texture the CPU bake reads (GPU fill is already
   matched to the CPU check, quantised in 1/16), writing an instance buffer for multimesh_set_rt_instanced. PT needs
   the blades in a BLAS, so the RT side keeps a coarse CPU-built set or rebuilds the BLAS per tile on change only
   (camera moves of a tile). Measure memory and lavapipe frame time against the rings.
2. Far blades blend into the ground. Pesce ("Making a greener grass", 2024) and GoT both bend far blade normals
   towards the terrain's and add a sheen term, so a far field shades like a soft surface and does not shimmer. Ours:
   far blades keep their own normals (the sparkle prefilter only widens roughness). The step: ease normals to the
   terrain normal by a distance factor in the grass shader (both raster and the PT translation, fwidth only), cut far
   blades to 3 vertices (GoT's far LOD), and match the far shell's sheen so rings and shell meet with no seam.
3. Heath and scrub far silhouettes. The far shell is a smooth draped surface; gorse, heather and broom stands keep
   a lumpy outline to the horizon in photos and in UE 5.7's voxelised Nanite foliage. The step: for shrubs between
   0.5 and 2 m, displace the shell's cells by a clump field from their own spacing (as the hedges' far bricks), with
   AO in the gaps, so hills past 1 km keep their texture. Shell only, no new instances.

Built 20 Nov (Paulo picked all three):
1. Far ring tiles fill on the GPU (grass_fill.glsl's ring mode): the CPU walks the nested cells and keeps the rank and
   obstacle survivors, tested against the rules baked on a 33 x 33 grid per tile (0.5 m at level 0), and the shader
   places them straight into the MultiMesh buffer. Meadow bench: ring build 277 ms to 100 ms, 547k blades (556k
   before), memory unchanged: on a device with ray tracing the renderer reads every MultiMesh's records back for the
   TLAS, so a GPU-written buffer keeps its CPU copy, and the CPU drops grid-rejected blades so the buffer holds no
   zero records. A full GoT per-frame indirect draw would skip that copy only with GPU-built TLAS instances: an
   engine change, not done. Worn tiles stay on the CPU (the wear grid is 0.25 m).
2. Blades past 6 m ease to the ground's own normal (finite differences of the data map's height texture, a material
   uniform, so nothing depends on shader globals) instead of straight up, 0.55 near and 0.85 by the rings' middle.
3. VegetationField3D.far_shell_clumps: three value-noise octaves (7, 19, 53 m) made uniform by their sampled quantile,
   thresholded at 1 - cover, so each stand's mass stands on its cover's share of the ground and opens elsewhere; an
   octave fades as cells pass a third of its size. Found on the way: unranged species (a heath's gorse) left the
   ground bare past the stream radius (now in the shell), and the shell's darkening for "shade between tops" read at
   half the near plants' brightness (dry heath bench: shell 0.0075 linear, a near gorse 0.016), as black blotches; the
   far colour already holds the plants' self-shade, so that factor is gone. Species meta "far_shell" false keeps a
   species out (the trees' own far forms).
   Next: measure the shell against the plants on screen as the far match does for the ground (vegetation_far_match.cpp).

## Mediterranean woodland ground layer (audit 19 Nov; card to Paulo)

The showcase's cork oak stand stands on bare sand with litter: every other forest has a floor set, the cork oak and
stone pine stands none. Real montado and dehesa (Portugal's and Spain's open cork and holm oak woods) keep a continuous
layer of annual grasses and herbs under and between the crowns, grazed short, browned by June; where grazing
stops, cistus and lavender scrub comes in (Joffre et al. 1999, "The dehesa system of southern Spain and Portugal";
Pinto-Correia et al. 2011 on montado scrub encroachment; Moreno and Pulido 2009). Under the crowns the grass is
greener and later to dry (shade, nutrient islands: Moreno 2008).

Plan (FLOOR_MEDITERRANEAN in make_floor, existing presets):
- Dry grass as the main layer, denser in the open than under crowns; thistle and French lavender scattered in the
  open; sage-leaved rockrose in loose patches (the encroachment), thyme on thin soil.
- Oak litter (cork oak's leathery leaves fall all year, little at a time) under the crowns only; twigs.
- GrassField3D under the stand: the meadow sward with summer dryness (the scene wires it).

## Burnt ground and regrowth (Paulo's pick 17 Nov; built 19 Nov)

PhysX burns plants now (fire_state_map: char, scorch, stubble; fire_char_map: ash). Nothing grows back: a burn stays a
fresh burn. What comes back, and when (temperate heath, grassland, scrub):

- **Days to weeks.** Grasses resprout from protected crowns: a green flush through the black within 2 to 4 weeks in
  the growing season (the "green-up" grazers follow). Ash blows and washes off in the first rains (PhysX does this).
- **Months to 3 years: fire bryophytes.** Funaria hygrometrica (the "fire moss": a pale green carpet with orange-brown
  setae and nodding capsules), Ceratodon purpureus (red-brown tufts), Marchantia polymorpha (flat liverwort thalli
  with umbrella-like receptacles) colonise the bare burnt soil within months and fade as vascular plants close in
  (British Bryological Society, Field Bryology 129, 2023; Frontiers in Plant Science 2024, bryophytes after fire).
- **Year 1 to 6: fireweed and resprouters.** Chamaenerion angustifolium (rosebay willowherb, "fireweed") floods burns
  from wind-blown seed and rhizomes, flowering magenta from the second summer. Bracken resprouts from rhizomes in the
  first season and can take over.
- **Heather.** Calluna resprouts from stem bases if the stand was young (under about 15 years) and germinates from
  the seed bank (heat and smoke cue); the pioneer phase is 3 to 6 years, building 7 to 15, mature 15 to 25,
  degenerate past that (Gimingham 1972; FEIS, Calluna vulgaris). Charred heather stems stand for 2 to 5 years before
  they break down.
- **Scrub.** Mediterranean resprouters (kermes oak, lentisk) send up shoots within months; seeders (rockroses)
  germinate after the first autumn rains (Calvo et al.).

Plan:

1. A contract: `WorldFire.burn_age_at(pos)`, the years since the ground last burnt (negative: never). Provider
   PhysXFireSpread3D for fires in play (game time), plus authored scars (a scene's old burns). Asked of world-links.
2. `PlantSpecies.RULE_BURN_AGE` (years; default range takes every age and "never") for burn species, and
   `PlantSpecies.burn_recovery` (years to full cover after a burn, from the preset) so every existing species thins
   on a fresh burn and comes back over its own time, with no per-set wiring.
3. Presets: charred shrub stems (black twiggy skeletons, breaking down with age), fire moss (Funaria carpet with
   setae), fireweed (rosette in year 1, tall magenta spikes from year 2), resprouting grass tussock (charred base with
   green shoots). Marchantia: PRESET_LIVERWORT already.
4. `PlantGenerator.make_burn(BURN_HEATH | BURN_GRASS | BURN_SCRUB)`: the sets by age.
5. Bench: a burn with patches of several ages (a stub WorldFire), raster frame, on/off costs.

Built (19 Nov): world-links' `WorldFire.burn_age_at` (b7e28ec4; WorldBurnScar3D for authored scars, PhysXFireSpread3D
for fires in play, c15a10b7). VegetationField3D samples it on a grid (2 m or more, at most 512 a side) when a species
needs it; `PlantSpecies.RULE_BURN_AGE` places the burn presets, `PlantSpecies.burn_recovery` (from
`PlantGenerator.get_burn_recovery`, set by tag_species) thins everything else: grasses back in 0.7 years, herbs 1.5,
heaths 8, mosses 10, soil crust 30. Presets PRESET_FIREWEED, PRESET_FIRE_MOSS, PRESET_CHARRED_STEMS,
PRESET_BURNT_TUSSOCK (plant_burn.cpp) read the season's `burn_age`; `make_burn(BURN_HEATH | BURN_GRASS | BURN_SCRUB)`
builds each in age bands. Bench vegetation `--burn[=off]`: four scars 0.05 to 5 years old in dry heath; all checks
pass (heather 122 per metre of strip unburnt, 0 on the fresh burn, 84 at five years). Cost (lavapipe, 640x360, raster, 80 m
streamed): 114.6k plants (heath and burn sets), +31.5 MB memory, +132 MB video, 2.43 M primitives, 1984 draw calls. A fire burning now shows once
the field places again (regrowth is slow). Next: the grass sward's own green-up (GrassField3D) by burn age.

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

## Far hedges as voxel bricks (Paulo picked 18 Nov; built)

A hedge segment is 70k to 330k triangles, and a farmland view holds hundreds of them. Past some
tens of metres a hedge's leaves and twigs are far below a pixel, so their shape and their light are
all that is left to draw. Nanite Foliage (Unreal, experimental) keeps that as geometry: clusters
that simplify badly as triangles become 4x4x4 voxel bricks, each voxel keeping its coverage, colour
and a distribution of normals, one normal picked per pixel so the lighting stays rough. The same
idea goes back to aggregate LOD for foliage (Neyret's volumetric textures, 1998; Loubet and Neyret,
"Hybrid mesh-volume LoDs for all-scale pre-filtering of complex 3D assets", 2017) and to SGGX
microflake distributions for filtered volumes (Heitz et al. 2015).

The fork has no compute rasteriser and must stay path-traceable, so the bricks become a mesh:

1. Voxelize each segment variant once (CPU, at species build): coverage, mean albedo and the normal
   distribution's moments per voxel (mean normal and its length as the spread, the Toksvig idea) at
   10, 20 and 40 cm.
2. Per level, emit the exposed faces of covered voxels (coverage over a threshold that keeps the
   segment's silhouette area), greedy-merged, the shell eroded by coverage so it is not blocky; per
   vertex the voxel's albedo, mean normal and spread.
3. Shade with the stored normal, widened by the spread (a rough, view-independent lobe) and a
   sub-voxel dither of the normal from the distribution, so a far hedge keeps the sparkle of lit and
   shadowed leaves rather than a flat paint.
4. Swap by distance with the mesh's own LOD chain (Godot mesh LODs and visibility ranges), the
   near segments real twigs. Shadows and PT trace the same shell.

Then woodland edges and tree crowns the same way, if it holds up.

Built (`VegetationVoxelLod::make_shell`, `GrassField3D.make_voxel_shell`, `hedge_voxel_lods` on):

- Triangles are sampled every voxel/3 with their material's look (BaseMaterial3D or a ShaderMaterial
  made from its code, so leaf flutter keeps working): colour, vertex colour (linear unless the
  material says sRGB), texture with its alpha cut-out. Normals are turned outward from the core for
  two-sided leaves.
- Coverage 1 - exp(-0.5 L) for leaf area L per voxel face; solid past 0.35. Faces of partly filled
  voxels sit inset, so the shell is not a lattice of whole cubes.
- The shell must not look brighter than the hedge it stands for. Three terms, all from the voxel
  grid: the filled share of the 26 neighbours as ambient occlusion; a deep leaf cloud's single
  scattering brings its reflectance to about half the leaf albedo (Ross 1981; Goudriaan 1977), so
  0.45 + 0.55 exp(-tau/2) with tau the leaf optical depth behind the face; and the rays the leaves in
  front let through end on shade (a fifth of the leaf brightness). Normals blend the voxel's mean
  leaf normal (by its length) with the crown's rounded outward direction, the spherical-normal
  foliage trick, so the brick lights as one soft mass.
- Hawthorn segment: full 279k triangles; 10 cm 4.5k; 20 cm 0.9k; 40 cm 0.2k; about 50 ms per level.
  Far views: the mean colour of each level is within a few sRGB values of the full hedge. Levels
  swap by visibility range: full to 45 m, 10 cm to 90 m, 20 cm to 180 m, 40 cm beyond, 3 m fades
  (a voxel about two pixels wide at 1080p). A visibility range is measured to the instance's AABB
  centre, so segments go in 8 m cells, one MultiMesh per cell and level.
- Not done: per-pixel normal dither from the distribution (step 3); greedy face merging.

Docs declined the first fields frame (18 Nov). Fixes:

- Far hedges read as a bead necklace of identical round bushes. Each segment now sits up to 0.18 m
  off the line, either way round, 12 to 24% longer than its 2 m step so neighbours grow together,
  with a height that wanders over tens of metres (0.7 to 1.4) and its own width; three variants.
  Hedgerow standards (field maple, hawthorn grown on as trees: Rackham 1986) every 35 to 80 m, with
  their own voxel levels (15, 30, 60 cm at 70, 140, 280 m).
- Stubble read as snow: its far look is shaded like a ripe stand (0.55 of its straw, pushed from
  its grey) and the combine's passes alternate light and dark (harvest stripes).
- A far barley field had a dark band: the live far match was one scale for all fields, the median
  of mixed crops. Each field with six or more pairs of its own now gets its own scale (row 3 of
  crop_ground_plots) over the global one.
- The wheat's far half was a smooth blur against the tillers' speckle: the relief canopy now adds
  the ears' grain at the pixel's narrow width, band-limited octaves (14, 7, 3.5 per metre) kept
  under their Nyquist limit (Lagae et al. 2010), never coarser (coarser octaves made blotches).
- Tramline lanes show their soil, part shaded, not black lines.

## Stand age (built 20 Nov, WorldStand contract 399d29a6)
- RULE_STAND_AGE (28): years since the last stand-replacing disturbance, -1 old growth (inside ranges starting at -1
  or reaching 10000 y). RULE_DISTURBANCE (29): 0 none, 1 fire, 2 wind, 3 beetle; pick one kind with (k, k).
- Left at their defaults, a species takes TreeGenerator.make_succession's meta succession_age / succession_kind
  (-1 any), so stage edges are each species' own.
- The field samples WorldStand on the burn grid's spacing. On stand_changed it reads the changed cells again and
  re-places only streamed tiles where some species' stand weight changed (advance_years inside every stage costs
  nothing). Populated fields populate again; a field's own disturbance reports (trees' stand_disturbance meta) are
  not followed, so it does not repopulate itself for ever.
- Known gap: two populated fields that both report disturbances and both use stand rules would wake each other on
  every populate (each report removes and re-adds its discs). Trees' report could skip unchanged discs.

## Forest edges (built 20 Nov, for trees' make_edge)
- Research: edge influence reaches 20-60 m into a forest (Harper et al. 2005); a shrub mantle of blackthorn,
  hawthorn, hazel and dog rose grows from about 8 m out to 3 m in; edge trees keep their low branches on the lit side.
- RULE_FOREST_EDGE (30): signed metres from the edge of the field's forest, positive inside, -1000 with no canopy.
  Species left at the default take meta "edge_distance" (Vector2), as succession_age.
- The forest is where the field's canopy species (canopy radius over 0, no shade rule, not edge users) would close
  it: their rules, patches, masks, stand stages and density summed past 0.35, on a grid of 2 m or more. Not the
  placed crowns, which stream round the camera; every tile sees the same edge. Signed distance by an exact Euclidean
  distance transform both ways (Felzenszwalb and Huttenlocher 2012), read bilinear.
- Variants with mesh meta "tree_open_side" (Vector3) are turned so that side faces down the edge distance
  (towards the open), +-15 degrees.
- Stand changes that move a canopy stage (a windthrow opening a clearing) rebuild the edge grid and re-place the
  streamed tiles where it moved.
- Check: vegetation --test "forest edge" (a 25 m windthrow clearing: mantle ring 16-28 m, edge trees facing in).

## Next round (card 20 Nov afternoon): floors that follow forest age, edge light, far rings without CPU copies
Research so far:
- Clearing succession on the floor (Wohlgemuth et al. 2002, Swiss windthrow plots after Vivian, slf.ch
  wo_2002fosnola; Estonian Myrtillus spruce windthrows; Forests 7(11) 287 on post-windthrow ground vegetation):
  in the first 1-3 years ruderals and light-demanding herbs take the opened ground (fireweed Chamerion
  angustifolium, foxglove, groundsels), raspberry Rubus idaeus and grasses (Calamagrostis epigejos / villosa,
  Deschampsia flexuosa) peak at about 3-10 years and can hold the ground for a decade; shade-tolerant forest herbs
  and dwarf shrubs (bilberry) mostly survive under the debris and recover as the canopy closes; mosses (Hylocomium,
  Pleurozium) drop after opening and build back over decades. Fire differs: fire moss and fireweed first (our burn
  presets already do this through RULE_BURN_AGE).
- Edge light (Harper et al. 2005; Dendrobiology 71 edge study; Can. J. For. Res. 34 289): light, temperature and
  ground flora change up to 20-60 m in; edge-loving grasses, brambles and nitrophilous herbs thin within the first
  10-30 m, shade herbs and mosses take over deeper in.
Design (to build after Paulo's pick):
- Floor species take RULE_STAND_AGE ranges per role: clearing ruderals (0, 4) fade 2, raspberry and clearing grass
  (1, 15) fade 5, forest herbs and mosses (-1 old growth or 15+) with a slow fade in, so a clearing flips through
  stages as WorldStand ages it. Wind and beetle kinds share the ruderal stage; fire keeps the burn presets.
- Edge: floor species get RULE_FOREST_EDGE weights: grass, bramble, nettle strongest from -5 to 15 m in, fading by
  30 m; shade herbs and moss carpets fade in from 10 to 30 m. Uses the same edge grid as trees' make_edge.
- Far rings: on RT devices the multimesh buffers get a CPU copy for the path tracer; far ring tiles are past the PT
  vertex-capture reach, so mark them raster-only (no CPU copy) where the renderer allows. Measure memory before/after.
- Built 20 Nov (floors that follow forest age): PRESET_RASPBERRY (97; upright suckering canes 0.8-1.7 m, pinnate
  leaves of 3-5 leaflets, red fruit on second-year canes; ~36k tris, impostors past range like bramble). Every
  forest floor but hedgerow and montado appends a clearing set on RULE_STAND_AGE with RULE_DISTURBANCE (2, 3):
  fireweed 0.5-10 y, raspberry 2-18 y, tall clearing grass (cocksfoot form, 1.3-1.8x) 1-20 y, and in broadleaf
  stands foxglove 1-6 y and nettle 1-10 y; all with shade 0-0.5 so the closing young stand shades them out. Forest
  mosses take stand age (6 y on, fade 10): thin on a fresh opening, full again in old growth (-1). Vegetation bench
  "clearings" check.
- Built 20 Nov (edge light on the floor): every forest floor but hedgerow and montado adds edge species on
  RULE_FOREST_EDGE that tolerate crowns (shade to 0.85) near the edge: tall grass (cocksfoot form; wavy hair grass
  under conifers) -8 to 12 m fade 10, bramble -6 to 10 m and nettle -4 to 6 m under broadleaves, raspberry -5 to 8 m
  under conifers. Interior shade herbs and forest mosses (sorrel, woodruff, anemone, violet, moss, feather moss) take
  12 m on, fade 18 (half at the edge line). A floor field reads the edge of its canopy_field's forest too. With no
  forest known, open-ended interior rules do nothing and edge rules place nothing.
- Maps built from rules alone (far shell, cover map, sward shade) leave out context-only species (burn presets,
  stand stages that skip old growth, edge-only species) instead of spreading them over all their ground.
- Built 20 Nov (far rings without CPU copies): the path tracer kept a CPU mirror of every MultiMesh's records on
  RT devices (a readback after each GPU write, and a warm-up in the TLAS gather), though only expanded surfaces
  (and merges that fall back to expanding) read it; captured surfaces (grass, plants in wind) read the GPU buffer.
  Now the gather fills the mirror only when a non-captured surface may expand, and a GPU write reads back only
  for an existing mirror, a missing custom AABB or rt_instanced. Grass bench --far-grass=rings PT (320x180, 1 spp):
  static memory 603 -> 515 MB, video 6619 -> 6554 MB; images equal within 1-spp noise (block means 85.35 / 85.09).
  gpu_multimesh --test PASS in gpu, cpu and gpu+taa modes, gpu and cpu PT frames identical.
- Fixed 20 Nov: the sward (GrassField3D.undergrowth_field) thins under context-only plants again, weighed by the
  last setup's burn, stand and edge grids (VegetationField3D._context_weight; the field keeps a copy, refreshed when
  burns or the stand change). A floor field reading its canopy field's edge now also reads the stand for those
  trees' stages (the edge moved round a windthrow only for the trees' own field before).

## Beaver ponds and meadows (card 20 Nov evening; built 20 Nov)
Research:
- Beaver ponds raise patch diversity along a valley; drained ponds become "beaver meadows" of sedges and grasses that
  hold for decades before woody plants return (Naiman, Johnston and Kelley 1988, BioScience 38; Wright, Jones and
  Flecker 2003, Ecology 84: herbaceous species richness rises about a third at landscape scale where ponds and
  meadows mix with forest; Cunningham et al. 2006; Ecoscience 19(3) 2012 on meadow succession).
- Drained bed sequence: bare mud seeds in at once with rushes (Juncus) and small sedges; within a few years a dense
  sward of tussock sedges (Carex stricta / C. elata in Europe) and tall grasses (Calamagrostis canadensis in North
  America; tall cocksfoot form as our stand-in), with meadowsweet, marsh marigold in the wet hollows and buttercups
  on drier ground. Willow and alder close it after 20-60 years where browsing and wet soil allow.
- Active pond zoning follows still-water hydrosere bands (Spence 1982; Keddy 2010): floating-leaved lilies in
  0.5-2 m, bulrush in the shallows, tussock sedge along the flooded margin, rush and meadowsweet up the wet shore.
Built:
- PlantGenerator.make_beaver(BEAVER_POND / BEAVER_MEADOW). The pond set is placed by water depth and distance only
  (lily 0.4-2 m floating; bulrush 0.05-0.6 m; tussock sedge 0-0.25 m and up to 4 m ashore; rush 0-6 m, marsh
  marigold 0-4 m, meadowsweet 1-10 m from the shore). The meadow set takes RULE_STAND_AGE with RULE_DISTURBANCE
  (4, 4), WorldStand.DISTURBANCE_BEAVER (world-links 05df7a23, age = years since the pond drained): rush and small
  sedge 0-4 y, then dense sedge, tall grass, buttercup 3-38 y, meadowsweet 4-38 y, marsh marigold 2-28 y (fade 3) within
  8 m of the remaining stream. All shade 0-0.4 to 0.6; the trees' STAND_BEAVER_POND supplies the dead pond trees
  and the closing willow and alder. Vegetation bench "beaver" checks.

## Deer browsing (card 20 Nov evening; built 20 Nov)
Research:
- Browse heights: roe about 1.1 m, fallow 1.6-1.8 m, red deer 1.8 m (Forestry Commission Practice Note 6; Deer
  Initiative field signs). The ground flora is all in reach.
- Under heavy pressure British and European woods lose bramble (the winter staple), honeysuckle, ivy on the floor and
  palatable forbs (wood anemone, violets, woodruff, bilberry, fireweed); grasses, sedges and unpalatable plants
  (bracken, Dryopteris ferns, foxglove, nettle) gain: "grassy woods" (Kirby 2001; Gill and Fuller 2007, Ibis 149;
  Morecroft et al. 2001, Wytham exclosures; Boulanger et al. 2018, Global Change Biology, deer and forest understories).
- Browsed bramble stays a low hummock of short canes bitten back each winter, many leaves eaten whole or torn, few
  flowers; browsed raspberry and saplings lose the leader and bush out from the buds below the bite (Bergman 2001).
- Deer graze clearings and rides into lawns of fine bents and fescues 2-8 cm high with clover and other creeping herbs,
  kept short year round (Putman 1986; Kuijper et al. 2009, Bialowieza). Deer tear rather than cut (no upper incisors),
  so blade ends are ragged and dry pale.
Built:
- WorldStand browse discs (world-links cef8dbd7): browse_at 0..1, browse_reach_at. PlantSpecies.RULE_BROWSE (31) reads
  it on the field's stand grid (0 where none); changes re-place only tiles where a browse rule's weight moves. Species
  that need browsing to grow are context-only (left out of far shell, cover map and sward shade by rules alone).
- PlantGenerator season "browse" (0..1): bramble canes low and spreading, bitten back with blunt ends and side shoots,
  leaves eaten or torn; raspberry canes bitten at the leader and bushy below; flowers and fruit scarce. Leaf.cut (the
  torn end, slanted, drying pale) does the torn blades and leaflets. Unbrowsed meshes are unchanged (same draws).
- PRESET_GRAZED_LAWN (98), reworked after review (the first version read as a sparse tuft of long blades): a 24 cm
  cell of 300-380 fine blades cropped to one height (2.5-4 cm, 82% torn at it, new leaves a little over), a flat
  darker thatch and a few white clover leaves (the petal surface, round leaflets with the chevron); ~3.7k vertices.
  Floors place it 0.2 m apart to 18 m.
- GrassField3D.graze_height (0.04 m): the sward itself is grazed where deer browse hard, in the shader from a pressure
  grid (graze_map, smoothstep 0.3-0.8 of browse_at), so near tiles, rings, GPU fill and the path tracer agree with no
  rebuild; torn pale ends, fewer dry tips, a fresher green, blades 30% narrower (fine regrowth; the turf ground under
  the sward closes the gaps). Followed on stand changes. Meadow species sets need browse rules of their own for the
  tall grasses and palatable herbs (the grass bench's do: RULE_BROWSE 0-0.45 fade 0.25; rosettes and nettle, dock,
  thistle keep). Grass bench --graze=P: a 6.5 m disc 6 m ahead framed from 1.6 m (docs pair: --sun-elev=50
  --sun-yaw=-30 --sun-energy=1.3, --graze=0 for the before).
- make_floor (all but hedgerow and montado), appended so every other species keeps its seed: bramble and raspberry
  whole to 0.35 (fade 0.15) and bitten (meshes at 0.85) past it; palatable herbs (woodruff, anemone, violet, bilberry,
  ivy, fireweed, herb Robert, wood sorrel) thin to about a fifth at 1; the tall clearing grass gives way past 0.5; deer
  lawn and clover in the light (shade to 0.45) from 0.45; grass (wavy hair grass under conifers) under crowns from 0.4;
  fern from 0.5. Vegetation bench "deer browsing" checks.
- Not done: honeysuckle and ivy-on-ground variants by browse (ivy on stems is trees'); browse reach is not used by the
  ground flora (all of it is in reach).

## Next round options (card 20 Nov evening): game trails, boar rooting, anthills and molehills
Research so far (to confirm before building):
- Game trails: deer follow the same lines between cover, feeding lawns and water, wearing 20-40 cm paths of bare or
  short-turfed ground; trampling shifts the plants by tolerance (Cole 1995 trampling resistance, already in
  get_trample_resistance; Liddle 1997 on paths). Build idea: trail polylines from a least-cost path over slope and
  cover between browse discs, lawns and water (a shared contract so ground detail and mud can wear the same lines),
  read by a PlantSpecies rule as metres from the trail; species thin by their trample resistance, plantain and
  grazed lawn take the verges.
- Boar rooting: wild boar turn the topsoil 5-15 cm deep in patches of a few to hundreds of square metres, mostly in
  moist, rich woodland and meadow edges; vegetation cover drops and regrows over one to two years, with ruderals first
  (Bueno et al. 2013; Barrios-Garcia and Ballari 2012, Biological Invasions review; Welander 2000). Build idea: a
  rooting disturbance with an age in months (WorldStand-style discs, owned by world-links), turned soil as ground
  detail (ground thread), uprooted herb meshes and a regrowth band by age here.
- Anthills and molehills: yellow meadow ant (Lasius flavus) mounds 20-50 cm high and up to 1 m wide on old unploughed
  grassland, built over decades, sunnier south faces, wild thyme and other small herbs on top (King 1977, J. Ecol.;
  Dean et al. 1997); molehills are fresh soil heaps 10-20 cm, bare then colonised within a season. Build idea: mounds as
  real ground geometry (ground thread or here as soil meshes with grass), with a mound top species set (thyme, sheep's
  fescue, rockrose) placed by a new mound rule; molehills by soil wetness and earthworm-rich ground.

## Fixes from the lawn reshoot (20 Nov evening)
- Sorrel and dock flower spikes drew as flat red bars from a few metres. Two causes: even rungs of four flat tepals
  (a ladder) and meshoptimizer's chain on hundreds of tiny petal islands, whose sloppy fallback welds them into strips.
  Now loose whorls of 2-8 (sorrel) and 3-8 (dock) drooping tepals on short stalks, spacing, size and colour varied
  (Rumex acetosa: green flushed red in flower, rusty in fruit), and their petal surfaces take the card LOD chain
  (rank-ordered shrink, area kept) like fire moss's capsules.
- Swirl stripes in the near turf: the ground's streaks and thatch turned world xz by a noise angle, which swings far
  points along arcs round the origin (concentric rings). Now a 2 m grid of rotated streak cells blended bilinearly
  (grass_streaks). The honami's fronts were also straight lines a sward wide; coherent canopy structures are a few
  heights across (Finnigan 2000; Py et al. 2006), so the fronts now bend with a slow crosswind warp and the wave's
  strength comes and goes in patches, with twice the per-clump phase jitter (the fallback wind's gusts the same).
- Grazed lawn mosaic (21 Nov, the clearing shot read as one stamp repeated): crop heights follow a patch-grazing
  mosaic (Adler, Raff and Lauenroth 2001) at about 1 m and 3 m, close-cropped patches and taller roughs, and each blade
  its own bite (skewed tall, 12% just regrown), widths varied.
- Grass bench --deer-wood=P: a clearing in TreeGenerator's real trees (P > 0 STAND_DEER_WOOD and its make_edge, the
  browse line; P = 0 the beech stand and its mantle), make_floor's beech floor with its browse set, the sward cropped
  by WorldStand browse P over the clearing and wood, from 1.6 m. About 12.5 GB and 30 min a 1920x1080 raster frame in
  the cloud: one at a time.

## Clearing reshoot fixes (21 Nov)
- The grazed clearing read as brown litter, not a lawn. Two causes, both systemic, not the bench's:
  - Leaf litter plants: RULE_LITTER sums every crown's drift tail (e-folding 6 m), so a clearing ringed by crowns
    reaches full litter in its middle. Litter now also needs some crown shade (RULE_SHADE from 0.06, fade 0.05), as
    forest_floor() already draws the ground: in full light the sward grows through what drifts in and earthworms pull
    it down within months (Satchell 1967; Knollenberg et al. 1985). The rim keeps its drift.
  - The ground between bitten-down blades showed soil and litter. A close-grazed sward tillers densely (20,000 to
    40,000 tillers per m2 against 5,000 to 10,000 lax; Hodgson 1990, Grant et al. 1983), a closed carpet. The ground
    field now hands its graze map to grass_ground() (globals grass_graze_map / grass_graze_rect): under a lawn the turf
    covers the soil fully, with less thatch and the lawn's brighter tint; rooting's torn share (G) opens it to soil.
- The bench clearing is 48 m between trunks, the edge trees' crowns leaning in to leave about 30 m of open sky; the
  eye stands 6 m south of its middle. GRASS_DEER_SHADE prints the crowns' shade along the view (0 open sky).

## Game trails (round 3, first pick)
Deer, boar and other large animals wear narrow, lasting paths between the places they use: cover, feeding lawns,
water, and the fords and saddles between them (Liddle 1997, ch. 3; Cole 1995 for the plants' tolerance). Trails form
because each animal prefers ground already trodden: the active walker model (Helbing, Keltsch and Molnar 1997, "Modelling the
evolution of human trail systems", Nature 388, 47; Helbing, Schweitzer, Keltsch and Molnar 1997, "Active walker model
for the formation of human and animal trail systems", Phys. Rev. E 56, 2527) gives
branching networks, not a straight line per pair of places. Least-cost routing over terrain is the usual way to lay
wildlife corridors and trails in GIS (Adriaensen et al. 2003, least-cost modelling), and games use the same A* over a
cost field for roads and paths (e.g. Galin et al. 2010, "Procedural generation of roads", Eurographics).
- `VegetationField3D.make_game_trails(data_map, area, goals, options)` (static, vegetation_trails.cpp): A* on a grid
  (options.cell, 2 m) for every pair of goals within options.reach (250 m), nearest pairs first; a cell's cost is
  raised by slope (cheap to about 10 degrees, then steeply; none over max_slope_deg 40), by water (fords only up to
  ford_depth 0.4 m) and by a seeded two-scale roughness (logs, tussocks, soft ground the map does not hold), and
  lowered by the traffic already routed over it (attraction 0.6: a worn trail costs down to 40% of fresh ground), so
  later routes join earlier ones. Out as worn paths (the fields' `paths` Dictionaries plus "kind": "game_trail"): one
  piece per stretch of equal traffic, a shared stretch carrying the summed passes and widening about 0.1 m each
  tenfold; corners cut by Chaikin's scheme twice. Default 500 passes a year a link, 0.35 m tread.
- The fields wear them as any path: GrassField3D.paths and VegetationField3D.paths (VegetationPaths), each plant by
  its trample resistance.
- Contract asked of world-links: the network published on WorldStand (or its own provider) so ground detail, mud and
  snow wear the same lines and the fields read them without the scene copying them.
- Vegetation bench `--trails` (also in --test): none through deep water or up steep ground, merged, shared stretches
  busier, repeatable.

## Boar rooting (round 3, second pick)
Wild boar dig for roots, bulbs, fungi and soil animals, turning the topsoil 5-15 cm deep in patches from a square metre
to hundreds, mostly in moist, rich woodland and at meadow edges. The sward and herbs are torn out and left as sods
flung over, grass side down; plant cover drops by half or more and comes back over one to two years, ruderals and
nitrophilous herbs first (Bueno et al. 2009, Ecol. Model. 220; Bueno et al. 2013, Plant Ecol.; Welander 2000, J. Zool.
251; Kotanen 1995, Ecography 18; Barrios-Garcia and Ballari 2012, Biol. Invasions 14).
- Contract (world-links 371c030c): WorldStand DISTURBANCE_ROOTING 5 discs (add_disturbance, fractional years), field
  layer only, read by WorldStand.rooting_age_at (years since the youngest rooting, -1 none); stand_age_at ignores it.
- PlantSpecies.RULE_ROOTING (32): years since rooting, -1 never, which only ranges from -1 or to 10000 take (as stand
  age). Sampled on the stand grid, followed like browse.
- PRESET_ROOTED_SOD (99): 3-8 turf clods 4-16 cm, lumpy (a 3D noise pushes the dome in and out), 65% grass side down
  with pale roots splayed over the top and a few blades poking from the rim, the rest tipped with their tuft; soil
  crumbs thrown round. By the season's rooting_age the grass dries in weeks, rots by a year, the clods slump and
  crumble, and shoots come up between them from about two months.
- make_rooting(ROOTING_WOOD / ROOTING_MEADOW): the sods in three age bands (fresh 0-0.15, drying 0.15-0.6, slumped
  0.6-1.1 yr), then herb Robert, nettle and dock (wood) or mayweed, dock, thistle and plantain (meadow) from about
  three months to two and a half years. List after the stand's species. The sods' far colour is turned soil, greening
  with age.
- make_floor gives its field layer RULE_ROOTING: litter back from 0.35 yr (next leaf fall), herbs, grasses and ferns
  from 0.8 yr (fade 0.6), mosses and lichen from 1.5 yr (fade 1.2); bramble, raspberry, bracken and plants on logs
  untouched.
- Vegetation bench `--rooting` (also in --test); plants bench `--close=99 --rooting=A`.
- GrassField3D: its browse grid (graze_map) carries the rooting too (G: the share of blades torn out, all when fresh,
  back over two years); those blades go, the rest stand at half height.
- One rooting look (21 Nov): the terrain material draws the turned humus (0.062-0.11) and ground detail lays the humus
  clods, both by freshness 1 - smoothstep(0.25, 2, years). The sods use that fade (bands 0-0.25, 0.25-0.9, 0.9-2 yr),
  the terrain's humus colour, no loose crumbs of their own; the grass's torn share is the same freshness.
- Not done: the turned soil itself (ground detail's: clods, exposed humus, 5-15 cm troughs). The sward's ground tint
  opens to soil under fresh rooting since 21 Nov (grass_ground() reads the graze map's G).

## Anthills and molehills (round 3, third pick)
Research:
- Yellow meadow ant (Lasius flavus) hills: domes of fine soil 0.3-1 m across, up to 0.5 m high, built over decades
  in old, unploughed, grazed pasture (a field of them dates the grassland), tens to hundreds a hectare in groups. The
  crest lies east of centre and the long axis east-west: the morning-warmed face is built steepest. Their turf is
  finer and greyer than the pasture's (sheep's fescue, wild thyme on the warm face), with bare crumbs of fresh workings
  near the top, most in spring (King 1977, J. Ecol. 65; "The persistence of Lasius flavus ant-hills", Oxford; "Ant-hill
  heterogeneity and grassland management", Oxford, Dryad doi:10.5061/dryad.fqz612jrm).
- Red wood ants (Formica rufa group): thatch domes of needles, twig bits and resin, 0.5-2 m across and up to 1 m
  high, on an earth bank, at sunny gaps and edges of conifer and mixed woods; mounds built under forest fail after clear
  felling (Sorvari and Hakkarainen, F. aquilonia in clear fells). Entrances mostly on the sunny side.
- Moles (Talpa europaea): heaps of loose soil 25-50 cm across, 10-20 cm high, in lines and arcs over the runs, in
  moist, deep, worm-rich soil of pasture and broadleaf woods; few on acid sand, in waterlogged ground or under conifers.
  Fresh heaps are dark and crumbly, crust grey in days, slump over weeks and grass over within months (The Mammal
  Society; Wildlife Online, European mole).
Technique: like the sods, meshes from PlantGenerator presets placed by rules, the grass kept out from under them
(the established approach for small ground features in open-world foliage systems: scatter a mesh and cut the grass
with an exclusion mask, as Horizon Zero Dawn's GPU placement and UE's procedural foliage blocking volumes do).
Built:
- PRESET_ANTHILL (100): a dome on a ring grid, crest moved east (+x; the species keeps random_yaw off), long axis
  east-west, green-brown turf base, 380-520 fine bristle blades, 2-5 thyme cushions on the south-east face, 1-6 bare
  patches of pale crumbs near the crest (season "ant_workings" 0-1).
- PRESET_WOOD_ANT_NEST (101): a mossy earth bank, a thatch dome leaning a little south, 1,400-1,900 needles lying every
  way (fresh rusty over grey), 60-110 twig bits, 6-12 dark entrances, most on the south side.
- PRESET_MOLEHILL (102): a lumpy cone sharpened while fresh, 40-70 crumbs on it and spilt round it, the pasture's grass
  flattened under the spill. By season "mound_age": crust paler in days, slump lower and rounder over weeks, grass in
  from the rim and over the top within a year. Mound surfaces get no wind weight.
- make_mounds(MOUNDS_MEADOW / MOUNDS_WOOD): meadow = ant hills in groups out of the shade, molehills at three ages
  along the patches' rings (patch_ring: runs); wood = wood ant nests (conifer 0.3+, shade 0.15-0.7, forest edge -2 to
  40 m), a few molehills under broadleaves. All off waterlogged ground (water distance 3 m+) and steep slopes.
  Species carry meta "mound", "sward_mound" (radius at scale 1), molehills "mound_age".
- GrassField3D follows the mounds its undergrowth and canopy fields place (polled like the trails): discs of each
  "sward_mound" species join its obstacles, and only the tiles over mounds that came or went are built again.
- Not done: mounds as ground relief for ground detail / terrain (they are meshes on the ground); a WorldStand
  contract for mole activity (age of heaps by place).


## Next round options (card 22 Nov): storm lodging, leaf drifts, grazing roughs
Research notes, nothing built yet:
- Storm lodging in meadows. Tall grass and herbs go down in swirled patches after heavy rain and gusts: the weight of
  water on the leaves and the wind's moment bend the stems past their base strength (Baker et al. 1998, J. Theor.
  Biol. 194, the lodging model for cereals; Berry et al. 2004, Adv. Agron. 84). The patches lie in the wind's
  direction with swirls where gusts turned, and stand back up over days as the stems dry (grasses recover through
  node gravitropism; cereals far less once ripe). Inputs: WeatherMap3D rain and gusts, the sward's height and the
  stage. Output: a lodging map the blades read (lean and direction), recovering with time since the storm.
- Leaf drifts. Autumn litter is blown along the ground and piles up in hollows, against trunks, logs, rocks and walls,
  and along the downwind edges of clearings; ridges and open ground are swept bare (Orndorff and Lang 1981;
  Boerner and Kooser 1989, litter redistribution by wind in deciduous forest). Inputs: leaf fall season, wind from
  WeatherMap3D, terrain curvature and the fields' obstacles. Output: litter that moves and gathers (drift map, deeper
  litter plants and ground tint in the sinks).
- Grazing roughs and dung. Cattle and horses avoid grazing near their own dung for months, leaving rank tufts in a
  grazed lawn (MacDiarmid and Watkin 1972; Bokdam and Gleichman 2000 for horse latrines), and nettles and docks come
  up on nutrient-rich spots; rabbits keep latrines on mounds. Inputs: WorldStand browse (grazed where) and a dung
  scatter. Output: tall rough patches and dung pats in the deer and cattle lawns, breaking the even crop.

## Storm lodging (round 4, first pick)
Built in GrassField3D (`lodging`, on by default; `get_lodging_at`, `advance_lodging`).
- Research. Lodging is the permanent displacement of stems from upright, by stem buckling or by the root plate
  turning in wet soil (Baker et al. 1998, J. Theor. Biol. 194; Berry et al. 2004, Adv. Agron. 84). The model is a
  moment balance: the wind's base bending moment grows with the gust speed squared and the shoot's height, rain held
  on the leaves adds weight and wet soil lowers the anchorage, so the same gale lodges a soaked meadow and spares a
  dry one. Lodged patches lie along the gust that laid them, with swirls where it turned, and are patchy because the
  stand's strength varies with fertility and density. Young grass culms lift again at the nodes (gravitropism) over
  days; ripe cereals hardly recover. Games mostly fake it with a static flattened texture; here it follows the weather.
- How. A grid of 96 x 96 cells of 4 m round the camera (moves with it, keeps the overlap), stepped eight rows a frame
  in weather seconds (WorldWeather.time_scale). Each cell reads wind_at(p, 2 m), surface_wetness_at and rainfall_at:
  risk = (U / Ucrit)^2 x (1 + 0.6 wet) x (1 + min(rain / 40, 0.5)) / (1 - 0.35 wet) x weakness, with
  Ucrit = 10 m/s x sqrt(0.6 / h) for the sward height h and weakness 0.3 to 1.7 from world-space noise (41 m and
  13 m). Above risk 1 the lodged share rises toward smoothstep over risk 1 to 4 within about 90 s, along the wind
  turned by a swirl of up to 40 degrees; otherwise it decays with tau = 2.5 days (crops 40) x (1 + 2 wet).
  Calm with nothing lodged skips the sweep after one probe.
- Shader. lodge_map (RGBA8: direction, share) over lodge_rect. Up to the share x 1.05 of the blades lie down along
  the direction (+-17 degrees each), kinked at the base and 7% shorter; up to x 1.6 lean after them at the patch
  edges. Grazed lawns do not lodge. No rebuild: tiles, rings and the path tracer read the same map.
- Check: grass bench --test, a wet 12 m/s gale for an hour lays the 0.5 m meadow down in patches along +x; ten calm
  dry days later it stands again.

## Leaf drifts (round 4, second pick)
Built in VegetationField3D (`get_litter_drift_at`, PlantSpecies.RULE_LITTER_DRIFT 33) and PlantGenerator
(PRESET_LEAF_BANK 103, in make_floor's broadleaf floors).
- Research. Fallen leaves move with the wind until something stops them, so the litter is redistributed after the
  fall: hollows, furrows and the feet of obstacles gather it, ridges and knolls are swept to the soil (Orndorff and
  Lang 1981, Am. Midl. Nat. 106, leaf litter redistribution in a West Virginia hardwood forest; Boerner and Kooser 1989,
  Am. Midl. Nat. 121, litter several times deeper in hollows than on ridges and sideslopes). Behind trunks, logs and
  stones the lee eddy drops what the flow carried (the snowdrift mechanism at a smaller scale), and a bank also forms along the windward foot. Games paint this with decals or
  leave the litter even; here the litter species follow it and leaf banks are real geometry.
- How. drift = 1 + 3.5 x (the ground's depth under its ring at 2.5 m and at 6 m, 0.6 and 0.4, clamped -0.24 to
  0.3 m), times 1 - 0.6 smoothstep(25 to 45 degrees of slope), plus 1.3 x the strongest obstacle foot: rocks
  (obstacles), logs (thicker than 5 cm) and trunks (the crowns' stem foot), exp(-gap / 0.5 m) within 1.6 m, times
  0.55 + 0.45 cos(angle from the downwind direction), clamped 0.15 to 2.5. The wind is litter_wind or the weather
  map's. The placement caches it on 0.5 m cells. RULE_LITTER now reads litter x drift (capped at 1): litter thins
  on crests and fills hollows. RULE_LITTER_DRIFT places by the drift itself.
- Leaf bank. The stand's own leaves (stand_litter_leaves) heaped over an ellipse 0.9 to 1.6 m long, 8 to 18 cm
  high, crest towards one end, lumpy, 2.2 layers of coarser leaves, over a dark matted under-layer (centre colour 0.2, 0.13, 0.07)
  so no ground shows between leaves; leaves follow the heap's face. make_floor appends it last to broadleaf floors
  (seeds of the rest unchanged): drift 1.35 up (fade 0.25), litter 0.45 up, crown shade 0.06 up, density with the
  year's leaf cover. sward_mound 0.55 keeps the grass out from under it.
- Check: vegetation bench --test, drift in the deepest hollow over the highest crest, a stone's downwind foot over
  its windward foot and open ground; banks last in broadleaf floors, 8 to 25 cm high, none under conifers.

## Grazing roughs and dung (round 4, third pick)
Built in PlantGenerator (make_grazing, GrazeSet GRAZE_CATTLE / GRAZE_HORSE / GRAZE_DEER; presets PRESET_COW_PAT 104,
PRESET_HORSE_DUNG 105, PRESET_DEER_PELLETS 106, PRESET_DUNG_ROUGH 107; PRESET_MAX 108; season key "dung_age").
- Research. Cattle reject the herbage round their own dung for months, so a grazed pasture carries rank, dark green
  tufts round older pats, several times the pat's area, that flower while the lawn cannot (MacDiarmid and Watkin
  1972, J. Br. Grassl. Soc. 27, "The cattle dung patch"). Pats are 20 to 30 cm across, glossy olive for a day or
  two, crusted in weeks, broken and grassed through over months. Horses dung in latrines they never graze, so a
  paddock becomes a mosaic of short lawns and tall roughs with piles of droppings, nettles and thistles (Odberg and
  Francis-Smith 1976, Equine Vet. J.). Deer leave pellet groups of 30 to 100 pellets with no rough round them.
  Games rarely show any of this; pastures read as an even mown lawn.
- How. Cattle: roughs (sward_mound 0.4 m, spacing 4.5 m), pats at 0.005, 0.06 and 0.4 years, docks and nettles in
  sparse patches. Horse: one shared patch layout (seed 11100, cover 0.25, 18 m) for latrine roughs, dung piles,
  nettles and thistles, so they gather in the same clumps. Deer: pellet groups at RULE_BROWSE 0.3 up (fade 0.15),
  also in the wood (shade to 0.9). All in the light (shade under 0.4), slopes under 30 degrees, 2 m from water.
  Roughs and pats keep GrassField3D blades out through sward_mound, as the mounds do; the lawn round them is the
  sward's own (graze_height and the lawn mosaic).
- Meshes. Pat: a ring-grid dome with a wobbly outline and coil ridges, crust paling from the top and rim, broken
  into plates with grass through it when old; roughness 0.5 fresh to 0.88 crusted. Horse dung: 6 to 14 domed balls
  with straw fibres. Deer pellets: 2 x 6 domes each, 2 to 4k triangles a group. Rough: 500 to 750 blades 20 to
  45 cm, shorter at the edge, 12% dead, 5 to 10 nodding flowering culms.
- Check: vegetation bench --test, cattle pats of three ages 2 to 6 cm high, a rough over 25 cm that keeps the sward
  out, one latrine layout for the horse set, deer pellets only on the browsed lawn.

## Next round options (card 22 Nov, round 5): meadow fairy rings, dewy webs, blowing leaves
Research notes, nothing built yet:
- Meadow fairy rings. Marasmius oreades and other grassland fungi grow out from a point as a ring of mycelium,
  0.1 to 0.5 m a year, rings of several metres to tens of metres: an outer band of dark, lush grass (nitrogen the
  fungus frees), inside it a zone where the grass is browned or dead (the mycelium makes the soil water-repellent),
  then normal sward again in the middle, and the fruit bodies in a ring after rain (Shantz and Piemeisel 1917,
  J. Agric. Res. 11, the zones). Built as: ring discs the GrassField3D reads for
  blade colour and height (dark band, dead band), a meadow mushroom set on the ring's edge with the season's fruiting.
- Dewy webs. On autumn mornings sheet webs of money spiders (Linyphiidae) and orb webs on tall stems show up by the
  thousand once dew beads on them; gossamer drifts over the sward on calm days. Built as: thin web meshes (sheets
  between blades, orbs on tall herbs and thistles) shown where WorldWeather dew or fog is high, sparkling in the sun.
- Blowing leaves. In autumn gales fallen leaves lift, tumble and skitter over the ground and pile up where the leaf
  drifts are (round 4), and swirl in eddies behind obstacles. Built as: a GPU particle layer of the stand's own leaves
  near the camera, driven by WorldWeather gusts, landing into the drift field; few and cheap.


## Meadow fairy rings (round 5, first pick)
Built in modules/vegetation/fairy_rings.h (FairyRings::at, FairyRings::bands, and the same code as GLSL for the
grass shader), GrassField3D.fairy_rings (on by default, off for crops), PlantSpecies RULE_FAIRY_RING 34 and
PlantGenerator PRESET_FAIRY_RING_MUSHROOM 108 with make_fairy_rings (PRESET_MAX 109).
- Layout. One shared ring layout for the whole world, so grass and mushrooms agree without any data passing: 40 m
  cells, 30% hold a ring, radius 2 to 14 m (more small than large), centre kept inside its cell so a point only
  reads its own cell. Rings past 40% "broken" open into arcs, as old rings do where they meet each other or a path.
- Grass. Three bands across the ring edge (Shantz and Piemeisel 1917): a dark, taller outer band (front, 0.55 m),
  a dead band just inside (0.5 m: blades browned to straw, shorter, some missing), and a weaker inner dark band.
  Blade height and colour only; no extra geometry, no textures.
- Mushrooms. A Marasmius-like champignon: buff cap 1.1 to 2.4 cm with a central umbo, 22 widely spaced gills, a
  tough pale stalk 3.5 to 7 cm, in clusters of 3 to 8. Placed only on the ring's dead band (RULE_FAIRY_RING
  -0.25 to 0.1 m, the signed distance to the ring front), fruiting from late May to mid November, in the open.
- Check: vegetation bench --test (mushrooms only on the rings, none off them); grass bench --fairy for a look shot.

## Dewy webs (round 5, second pick)
Built in modules/vegetation/plant_webs.cpp (PRESET_SHEET_WEB 109, PRESET_ORB_WEB 110, PRESET_MAX 111,
PlantGenerator.make_dew_webs) and the plant shader's silk variant (PLANT_SILK).
- Research. Money spiders (Linyphiidae) make horizontal, non-sticky sheet webs and reach very high numbers in
  grassland and crops (Sunderland, Fraser and Dixon 1986, Pedobiologia 29: up to 160 per m2 in cereal fields;
  Bristowe 1958, The World of Spiders: over a million an acre from August to December). Silk is 1 to 4 microns
  thick, so a web is unseen until dew or hoarfrost beads it; on a still autumn morning a whole field turns grey-white
  with sheets and the orbs of garden spiders hang between tall dead stems, and both vanish as the sun dries them.
- Technique. The threads are not geometry: each web is a few triangles (a sheet's disc, an orb's disc, crossed strips
  for single threads) and the fragment shader draws the threads from the UVs, each widened to at least a pixel and
  faded by its true width over the pixel (Persson's phone-wire anti-aliasing); past a family's spacing its mean cover
  takes over, so far webs keep their true brightness and never shimmer. Dew drops are modelled as beads along each
  thread (0.16 to 0.56 mm across, one per 0.5 mm), resolved up close and averaged far off. The surface is blended (a
  hash only made a fixed sparkle in raster); the path tracer takes the same cover in its any-hit stage. Raster and
  path traced orbs match at full size (spiral, hub, free zone, open sector, frame and anchors).
- Sheets: four families of jittered, broken threads 0.7 to 1.2 mm apart, densest in patches a few centimetres across
  and thinning to nothing at the edge; small flat sheets 5 to 10 cm near the ground and hammocks or domes up to 22 cm
  higher in the grass, with scaffold threads up to the blade tips. Orbs: 25 to 35 radials out to a five-sided frame,
  a tight hub spiral, a free zone, the capture spiral 3 to 5 mm apart (wetter, as its glue gathers water), a quarter
  with one sector open (Zygiella-like), hung between two dry stems with dead heads, frame threads tied to the stems
  and the ground.
- Dew. The silk reads WeatherMap3D's surface wetness (where dew lands) and hoarfrost (white, matt beads); a "dew" key
  in the season sets it instead for scenes without a weather map. Dry, only the hub's denser silk faintly shows.
- Season: sheets from March to early December, most from late July; orbs late July to October; none under snow.
- Check: vegetation bench --test (silk shader on every web, sheets low, orbs on stems, seasons); plants bench
  --close=109 / 110 --dew=1 for a look, --dew=0 to see them gone.

## Blowing leaves (round 5, third pick)
Built into LeafFall3D (modules/vegetation/leaf_fall_3d.cpp), which already had falling leaves (Andersen, Pesavento and
Wang 2005 plates), gale lifts, kicks by bodies and leaves on water and snow. What was missing was the ground: lying
leaves never moved until a gust past the lift speed threw them, and loose litter was one uniform number round the
camera, not the litter the crowns dropped.
- Research. Wind moves fallen leaves over the forest floor and gathers them in hollows and against obstacles
  (Orndorff and Lang 1981, J. Ecol. 69, "Leaf litter redistribution in a West Virginia hardwood forest"); in a breeze
  that cannot lift them they slide, flip and cartwheel along the ground, the motion everyone notices in autumn.
- Creep. Past creep_speed (2 m/s at 10 cm, half on ice) a lying leaf slides downwind at 0.3 of the excess (to
  3 m/s), its heading jittered, flipping over now and then and turning as it goes; with less wind it slows by
  friction (4 m/s2) and stops. Past lift_speed it is thrown up as before.
- Shelter. The wind on the ground is the open wind over drift^0.6 (clamped 0.35 to 1.3), drift being the round-4
  leaf drift field of the VegetationField3D (hollows, the lee of rocks, logs and trunks): leaves slide off exposed
  ground and stop in the drifts, so moving leaves end where the drawn drifts are.
- Litter source. field_litter (40 loose leaves per m2 under full litter, on by default) reads
  VegetationField3D.get_litter_at: the gusts set leaves sliding, or lift them, only where the crowns dropped litter,
  most from exposed spots. litter_density, the old uniform source, stays for scenes without a field.
- Not done here: whirls (small vortices that spin leaves up in the lee of buildings, a few metres across; Tratt et al.
  2003 measured dust devils as Rankine vortices with 5 to 10 m/s swirl and 2 to 3 m/s updraft). They belong in the
  shared wind (WeatherMap3D), so leaves, dust, smoke and grass all feel them, not in one system's private copy.
- Check: vegetation bench --test (or --test=blowing): still in light air, all sliding downwind past the creep speed
  and none lifted; leaves taken off a wood's litter by the wind. weather --test=leaves and links --test still pass.

## Avalanche tracks (after round 5; world-links 0c249ead)
Built in modules/vegetation/plant_avalanche.cpp: PlantGenerator.make_avalanche_track and DISTURBANCE_AVALANCHE_KIND 6.
SnowAvalanche3D stand_damage marks the broken stand as WorldStand DISTURBANCE_AVALANCHE with the years since; this is
the ground layer that grows there (the bent alder and willow shrubs are the trees').
- Research. Only plants that bend under the snow or regrow fast hold in a track; where it runs most often the
  vegetation and soil are torn away, and avalanches carry soil, boulders and broken wood into the debris (ALPECOLe,
  "Avalanches", University of Zurich 2011). Active chutes are kept in shrubs with forbs and ferns: green alder,
  salmonberry, willow, with false hellebore, lady fern, fireweed and valerian (NRCS ecological site F003XN926WA,
  North Cascades; LANDFIRE BpS 10830, North Pacific Avalanche Chute Shrubland, 2020). A 2007 Journal of Vegetation
  Science paper often cited on avalanche-track plant diversity is listed as withdrawn, so it is not used here.
- Stages by years since the avalanche: torn turf (rooted sod at 0.3 years) to 2, broken twigs to 4 and shed needles to
  3 from the snapped conifers; fireweed from 1 to 25, lady fern 2 to 60, raspberry (for salmonberry) 2 to 40, a
  broad-leaved tall forb (butterbur's form, for hellebore and Adenostyles) 3 to 60, bilberry (for huckleberry) 4 to
  60, all in patches. Slopes up to 45 degrees, out of the edge's deeper shade (0.6).
- Check: vegetation bench --test, a 5-year kind 6 track holds the flora, none round it or on a windthrow of the same
  age; debris species fresh, tall forbs later.

## Next round options (card 22 Nov, round 6): snowmelt sward, flood wrack lines, seed down in the wind
Research notes, nothing built yet:
- Snowmelt sward. Where snow lay long, the grass comes out flattened and matted in one direction, straw-pale, with grey
  and pinkish webs of snow mould (Microdochium nivale, Typhula) in patches that lay longest; it greens from the edges
  over two or three weeks. Built as: GrassField3D reading the snow's lie time (WorldSnow) for a matted lean, pale
  tint and mould patches that fade as the season warms.
- Flood wrack lines. After a flood a river or lake leaves strand lines of straw, reed stems, twigs and leaves along
  the high-water mark, caught on fences and the lower branches, the grass beneath combed flat downstream. Built as:
  wrack meshes placed along a contour a set height over the water's level (WorldWater), the sward laid flat below it.
- Seed down in the wind. In late summer thistle, fireweed and willowherb down and dandelion clocks loose their seeds
  in dry, sunny wind; they drift in the air, catch in the grass and pile in corners. Built as: light particles in the
  shared wind from the plants that are seeding (their bloom stage), few and cheap, settling into the grass.
- Later card: lee whirls. WeatherMap3D.add_vortex(position, radius, swirl, updraft, spin) is on main (41dbafbc90) and
  LeafFall3D feels any vortex through wind_at; what is missing is an owner that spawns whirls in the lee of buildings,
  rocks and wood edges in gusty wind (Tratt et al. 2003 sizes). A candidate for a future card.

## Snowmelt sward (round 6, first pick)
Built: GrassField3D.snowmelt (default on, off for crops), get_snowmelt_at(position, day), static snow_lie(climate,
height, day).
- Timing: the weather's own degree-day snowpack (WeatherSeason climate through WorldContracts.weather(), the same pack
  the season uses), stepped daily from 1 August at each height. A lie counts once the pack holds over 3 mm of water for
  10 days. The field fills a 16-band table (50 m steps round the camera) of days lain and days since the melt when
  the day changes; the blade shader reads it by blade height, so a slope melts out from the bottom up. Two noise
  octaves (about 11 m and 1.7 m) move the lie by up to a couple of weeks: drifts lie longer and melt later. The
  per-site snow grid (SnowCover3D) is not read: it covers a few hundred metres round the player, the sward reaches
  kilometres, and both follow the same weather.
- Look: over about 20 days of lie, blades lie flat in swirls (direction from 2.2 m noise) and turn pale and
  straw-yellow, fading out over 4 to 24 days after the melt. Over about 45 days, grey snow mould spots (a few
  centimetres to 30 cm, clustered by large-scale noise) show as grey lint at the melt, drying to bleached straw within
  a week, gone after about six weeks.
- Sources: UMass Extension, "Snow molds" fact sheet (gray snow mould needs about 45 to 60 days of cover; grey lint-like
  mycelium on matted leaves; straw to grey-brown patches); Penn State Center for Turfgrass Science, gray snow mold
  (Typhula incarnata, about 60 days under snow; patches from a few inches to about a foot); pink snow mould spots
  under 8 in. Neither gives a recovery time: the three to six weeks here are an estimate.
- Bench: grass --snowy --day=130 (fresh melt) or --day=140 (late); prints GRASS_MELT. Grass --test checks under snow on
  day 100, matted on day 130, upright by day 200.

## Flood wrack lines (round 6, second pick)
Built: PlantGenerator.make_flood_wrack, PRESET_FLOOD_WRACK (111, PRESET_MAX 112, plant_wrack.cpp); PlantSpecies
RULE_HIGH_WATER (35, ground minus the peak, m) and RULE_FLOOD_AGE (36, years); GrassField3D.flood_wash. All read
WorldWater.high_water_at (world-links' contract: peak surface, days since, the flow's direction then; the water
producers fill it).
- Sources: USGS, Koenig et al. 2016, Identifying and preserving high-water mark data (TM 3-A24): debris lines are
  trails of twigs, grasses and other small debris on grassy overbanks; the landward edge of a debris line is usually
  the crest, and debris sags as the water falls (lower, thinner lines); seed lines (seeds, needles, grass) go within
  hours or days; folded-grass wash lines are the boundary between grass bent by the floodwater and grass that was not
  flooded, with bending and yellowing below the mark. No width or lasting time is given for debris lines: the strip
  widths (0.2 to 0.5 m), the five weeks the grass lies flat and the year the wrack lasts are estimates.
- Mesh: a strip 1.2 to 2.6 m along +Z, a silted mat under straw blades, reed culm pieces, twigs (some forked) and
  leaves laid along the line, landward edge sharp, foot frayed; about 10 to 20k triangles, drawn to 35 m.
  follow_flow puts mesh +X down the fall line, so the strip lies along the contour.
- Placement: the main line from 8 cm under the crest to the crest, dense (on a gentle bank that is a metre or two
  wide; the grass there is pressed lowest, under the wrack); lower lines 0.35 to 0.9 m under it, sparse
  and smaller; both gone a year after the flood.
- Grass: a 64 x 6 m grid of the mark round the camera (read every 5 s); blades under the crest (with 6 cm of noise)
  combed flat along the peak's flow direction and yellowed, fading over 7 to 35 days, with a grey-brown silt film
  fading over 20 to 120 days.
- River scout (grass bench --river; GRASS_RIVER_TOP=1 looks straight down): a PhysXRiver3D (5 m3/s, flood x6)
  gives a mark 0.68 m over its surface, 7.5 m from the centre line; 132 strands lie on it (ground 0.10 m under to
  0.02 m over), 20 on the lower lines, and the grass below is combed and silted. The wrack must come ahead of the
  ground cover in the biome's list: appended after it, the sward's footprints left 16 strands on the same line.
- Sea and fixed-level lakes now give a mark too (still level plus storm run-up, Valence b7a8c6b63a): strand lines
  follow it with no change here.
- Not yet: live re-placement of wrack when a new flood peaks (the field places at populate and on stand changes).

## Seed down in the wind (round 6, third pick)
Built: SeedDown3D (seed_down_3d.cpp). Heads of the scene's dandelion, thistle and fireweed species (meta
plant_preset) within 40 m of the camera loose plumed seeds in their season's share of heads in seed
(PlantGenerator.get_bloom_shares); the wind at the head from 2 to 10 m/s ramps the release, a wet surface halves it,
rain stops it. Each seed rides WorldWeather.wind_at at its height, falls at its falling speed, and rides a vertical
gust (Langevin, sigma_w about 1.25 u*, u* about a tenth of the local wind, T_L about 0.5 z / sigma_w). It lands caught
in the grass, on bare ground or floating on water (carried by WorldWater.flow_at), lifts again in gusts over 3 m/s
10 cm up, and is recycled after 90 s. Real geometry (seed, beak, 20-32 bristle strips, backlit) in one MultiMesh a
kind, so the path tracer sees them. still_shot holds them for stills.
- Sources: Cummins et al. 2018, Nature 562 (dandelion: falls at 0.391 m/s, 0.633 mg, 13.8 mm pappus of about 100
  bristles, porosity 0.92; most seeds land within 2 m, some travel over 1 km on updrafts). Seale et al. 2022, eLife
  11:e81962 (the pappus closes when damp, 60-90 degrees, and then falls two to three times faster; at 9.8 m/s 24% of
  dry and 50% of wet seeds stayed on the head, so release favours dry wind). Thistle and willowherb falling speeds
  (0.3 and 0.25 m/s) and the release rate are estimates: no measured values found.
- Not yet: the drift correction for turbulence changing with height (Thomson 1987); seeds held up by updrafts over
  sun-warmed ground (the shared wind has no thermals); scenes add the node (it is not made by the fields).
- Bench: vegetation --test checks the fall speed, the damp fall, drift and loft in wind, and release only in wind;
  grass --day=245 --seed-down looks along a thistle's plume (sparse: a head looses a seed or two a second).

## Plants on the drawn bank relief (22 Nov, world-links request)
Landscape3D draws slump scarps and cliff ledges over the heightfield (bank_relief 0.8 m, cliff_relief 0.6 m,
WorldGround.relief_at: x the drawn surface's height over the heightfield). Plants placed on the heightfield
stood inside a pushed-out bank or floated off a cut one. Now:
- VegetationField3D: each plant (and each canopy record) stands at the heightfield plus relief_at's x; floating
  plants and plants on logs are unchanged.
- GrassField3D: the per-tile height texels the blade shader samples carry the relief at each texel's centre, and
  the CPU blade path adds it per blade, so both paths stand on the same surface. Done on the CPU, not in the vertex
  shader, so the path tracer's merged MultiMeshes (whose MODEL_MATRIX is the field's) get it too.
- The relief is sampled at the data map's texel spacing (0.5 m in the benches), so steps finer than that are
  smoothed under the grass; Landscape3D's own relief is built at its finest level and is close to that scale.
- Check: vegetation --test "bank relief" (a WorldGround stub raises one half 0.3 m; plants follow it exactly).

## Live flood wrack (round 7, first pick)
A flood leaves its drift line at the peak's crest; field crews read flood peaks from these lines (high-water
marks such as drift lines, seed lines and mud lines: Benson and Dalrymple 1967, USGS TWRI 3-A1). So the wrack
should move when a new flood peaks over the old line, and fade as the mark ages.
Built: VegetationField3D._follow_floods. Every 2 s, where a species reads the marks (RULE_HIGH_WATER), each
streamed tile's WorldWater.high_water_at on a 3 x 3 lattice is hashed (peak to 5 cm, age to 30 days); a tile whose
key changed is marked stale and placed again, the old tile showing until its replacement is in (as burns do).
GrassField3D's flood wash already re-reads its map every 5 s. get_flood_replaced_tiles counts the re-placements.
- Check: vegetation --test "live flood wrack" (the mark rises 0.4 m; 64 tiles placed again within 2.5 s; all
  wrack on the new line, none on the old).
- Cost: 9 high_water_at calls per streamed tile every 2 s, only with wrack in the biome.
- Populated (non-streamed) fields do not follow; scenes stream.
- While a storm's water is still rising the mark rises with it, so the tiles near the river are re-placed each
  check; the strands under the water are hidden by it until it falls.

## Pollen clouds in dry wind (round 7, second pick)
In early summer a gust through flowering pines or spruces shakes out a visible yellow cloud, and a meadow in flower
smokes pale in the wind. Wind-dispersed pollen settles slowly in still air: jack pine 2.53 cm/s, black spruce
3.19 cm/s (Grana 1995, doi:10.1080/00173139509429031; grass grains 2 to 3 cm/s, see "Pollen puffs" above), so it
rides the turbulence for minutes. Release: anthers open as they dry and close in damp air; grass pollen counts fall
with humidity and rain, rise with warmth, and are low in 0 to 2 m/s of wind but moderate to extreme in about 4 to
10 m/s (Waudby et al. 2022, Aerobiologia 38: 533-555, citing Taylor et al. 2002 for the anthers).
Built: PollenCloud3D (pollen_cloud_3d.*), the landscape scale beside VegetationPollen3D's close brushing puffs.
- Every 0.25 s: release_factor = dry air (full at 55% humidity and under, none over 85%) x dry leaves x no rain x
  wind (3 to 8 m/s); thresholds are estimates fitted to the ranges above.
- In flower: conifers by conifer_window (days 130 to 170, an estimate for northern temperate pines and spruces);
  the sward by PlantGenerator.get_pollen_release for cocksfoot and timothy in the season here (grass_window when
  the weather gives no season).
- Sources: the nearest needle-leaved crowns of the scene's VegetationField3Ds within range (get_crowns_near now
  reports "conifer"), each at its crown's middle with the wind there; sward points on a 6 m lattice within 30 m
  where a GrassField3D's rules grow grass.
- Drawn: one PhysXFlow3D of its own, 0.5 m cells, smoke density 0.35, a little heavy (-0.03), slow fade (0.05),
  sulphur yellow, scattering albedo 0.85 (estimate), the weather's wind; sphere emitters with the wind's velocity.
- Check: vegetation --test "pollen clouds" (10 crowns shed in dry wind, none damp, calm or in winter; nearest first).
  Grass bench --pollen-cloud --day=165 sheds from 6 sward points with Flow running; the Flow gas is too slow on the
  cloud's software renderer to judge the look (50 frames at 640x360 take ten minutes): look shot on the PC queue.

## Turf overhangs on cut banks (round 7, third pick)
A bank of root-bound topsoil over weaker soil fails in layers: the flow undercuts the lower layer and the rooted sod
holds on as an overhanging cantilever until it breaks off (cantilever failures of composite banks: Thorne and Tovey
1981, Earth Surface Processes and Landforms 6). Roots give the sod much of its strength (Pollen and Simon 2005,
Water Resources Research 41, W07025). Slump scarps and terrace edges show the same lip with roots hanging below it.
Built: PRESET_TURF_LIP (112, PRESET_MAX 113, plant_bank.cpp), PlantGenerator.make_bank_turf, PlantSpecies
RULE_BANK_EDGE (37, RULE_MAX 38).
- RULE_BANK_EDGE: from a point not itself on a face (the ground behind it under 35 degrees), along 16 directions,
  the first 5 cm step steeper than 45 degrees within 1 m, and how far the drawn ground (heightfield plus
  WorldGround.relief_at) falls over the 0.6 m past it; the largest fall wins. VegetationField3D moves the plant
  onto that edge and turns mesh +X over the drop (a little scatter).
- The mesh: a sod slab 0.7 to 1.5 m along the edge standing out 12 to 35 cm, 10 to 20 cm thick with a ragged foot,
  its cut face and underside dark topsoil, 1300 to 1700 blades a square metre leaning out and hanging over the lip,
  40 to 70 roots a metre trailing 10 to 45 cm (dimensions are estimates from photographs). About 6.5k triangles.
- make_bank_turf: spacing 0.9 m, a fall of 0.35 m or more, ground under 35 degrees, on land, level on its top.
- Checks: vegetation --test "turf overhangs" (a stub scarp: 65 lips along its top edge, none elsewhere, none without
  it); plants --close=112 --bank (a row on a 1.2 m step, a scout only: the step is a stand-in).
- Not yet: lips breaking off and falling as the bank erodes (a PhysX/Blast job), and roots on bare scarp faces with
  no lip.

## Sod lips breaking off (round 8, first pick)
The overhanging sod of a composite bank fails as a cantilever (Thorne and Tovey 1981: shear, beam and tensile
failures of the overhang) once the flow has cut far enough under it, or under a load; the failed blocks lie at the
bank's foot until the next flows carry them off.
Built: SodLipFall3D (sod_lip_fall_3d.*) and VegetationField3D plant removals.
- VegetationField3D.add_plant_removal(position, radius, species): plants taken away for good; streamed tiles there are
  placed again without them (a populated field populates again); get_instances_near(species, position, radius)
  reads the drawn instances' transforms and meshes from the field's MultiMeshes. Mowing will use the removals too.
- SodLipFall3D, twice a second over the lips within range of the camera: a body of the load group (grass_trample)
  standing on an overhang breaks it at once; water at the foot (WorldWater.height_at 0.6 m out over the drop, between
  0.7 m under the lip and its top) is a hazard of 1/undercut_time (600 s); a soaked bank in heavy rain (wetness over
  0.8, rain over 5 mm/h) 1/soaked_time (3600 s). Both times are estimates on the game's time scale, with a
  deterministic draw per lip and look.
- A failed lip: removed (0.6 m), and its mesh falls as a RigidBody3D box of sod (the slab's 20 cm, 1500 kg/m3 wet
  topsoil, an estimate), turning about the edge and pushed out over the drop. Past max_blocks (24) the oldest freeze
  where they lie.
- Check: vegetation --test "sod lips" (dry bank: none; a step: that lip alone, not placed again; the river 0.3 m
  under the lips: exactly the 14 lips in that band break).
- Not yet: breaking the block on landing (Blast), and the river carrying blocks off.


## Roots on bare scarps (round 8, second pick)
A cut bank or slump scarp shows the roots of what grows on top. Their depth follows the global profiles of Jackson et
al. 1996 (Oecologia 108: 389-411; share of roots above depth d cm = 1 - beta^d, beta 0.952 for grasses, 0.970 for
trees): the sward's fine roots a dense mat in the top 15 cm or so, thinning to 50 cm; trees' woody laterals deeper
and wider. The roots are grown by space colonization (Runions, Lane and Prusinkiewicz 2007, "Modeling trees with a
space colonization algorithm", Eurographics Workshop on Natural Phenomena), here in the face's plane: attraction
points scattered by that depth profile, tips stepping toward the points near them with a downward pull, radii by the
pipe model (exponent 2.5).
Built: PlantGenerator.make_bank_roots (plant_bank.cpp), two species.
- PRESET_BANK_ROOTS: the sward's fibrous roots, on any earthen face of 0.4 m or more past an edge (RULE_BANK_EDGE),
  off rock (RULE_ROCK under 0.4).
- PRESET_BANK_ROOTS_WOODY: a few woody laterals (to about 4.5 cm across) coming out of the face, branching to fine
  ends; within 4 m of the canopy's edge (RULE_FOREST_EDGE from -4 m; the laterals' reach past the crown is an estimate).
- The roots lie on the face, a little proud of it and now and then dipping under it; about a third of the finest ends
  hang free off it.
- Each variant is grown on its own face angle (50 to 88 degrees, mesh meta "bank_face_slope"); VegetationField3D
  measures the face below the edge (its fall over its run while steeper than 35 degrees, up to 1.5 m) and takes the
  variant nearest it, so the roots sit on slumped and near-vertical faces alike.
- Check: vegetation --test "bank roots"; plants bench --close=113 --bank.
- Not yet: roots shown in the sod blocks' undersides when a lip falls; roots on rock clefts (trees' cliff roots).

## Mowing and hay rows (round 8, third pick)
Haymaking in a temperate climate: mow, ted (spread to dry), rake into windrows once nearly dry, bale; "about three
days of good weather to cure hay" (UNH Extension, "Haymaking"); in the UK "rarely made in under three days" (FAO,
Grassland Fodder Conservation, ch. II). Stubble of 3 to 4 inches (7.5 to 10 cm) for quick regrowth (UNH; Ontario
OMAFRA, "Cutting, conditioning and raking for faster hay drying"). With a 2 to 3 m cut "the swath should be gathered
to 1.6 m" (FAO). Well-made hay feels crisp and looks bright; bleached hay is weathered (FAO).
Built: GrassField3D.hay_meadows (polygon, row_angle, mow_day or fixed days), all in the blade shader from a 0.5 m
index map (mow_map) and a texel a meadow of state (mow_state: days since mown, row angle; a texture, not a uniform
array, for the path tracer's material slot), so the days pass without a rebuild.
- Every blade is cut to a 7.5 to 10 cm stubble that grows back over about six weeks (an estimate).
- 60% of the blades are the cut grass (an estimate): day 0 lying in swaths, passes 3 m apart, 1.6 m wide, heaped
  10 cm; days 1 and 2 tedded flat over the stubble every way; day 3 raked into windrows 6 m apart (two swaths to a
  row, an estimate), 1.2 m wide, heaped 30 cm; from day 4 baled and carted. Gathered by moving each blade across its
  row in the shader (tiles' culling boxes grow by 3.2 m with meadows), so a windrow holds its whole strip's grass.
- Colour: wilts duller on the day, cures to green-gold over three days.
- VegetationField3D: the generator's herbs over 12 cm in a mown meadow are not placed until 28 days on (an estimate);
  GrassField3D.mown_days_at over every meadow node; streamed tiles are placed again when a meadow's days change.
- Check: grass --test "hay meadows", vegetation --test "hay meadows"; grass --hay=D (0, 1, 3, 5).
- Not yet: bales lying in the field on day 3 (a Tripo or generated prop), cut herb stalks in the stubble, the far
  ground showing the rows past the blades' reach.

## Round 9 candidates (research, not built; waiting on a pick)
- Hay bales: a large round bale of 5 x 5 ft (1.5 m) is the usual reference, about 1,000 lb (450 kg) at
  10.17 lb/ft3; a 4 x 4 ft bale is about half of it, a 5 x 6 ft about 44% more (UF/IFAS EDIS AN326, "Implications
  of Round Bale Dimensions on Hay Use"). Bales would be dropped along the windrows on day 3 and carted by day 4 or 5.
  Small and large square bale sizes are not yet sourced. Possible mesh: generated (wrapped twine bands, loose outer
  layer, flattened bottom) rather than Tripo, so each bale's hay matches the windrow colour.
- Cut stalks: instead of hiding herbs over 12 cm in a mown meadow, draw them cut off at the stubble height (the
  plant shader clipping above the cut while mown) and regrowing over the 28 days.
- Far rows: the swaths and windrows on the far ground past the blades' reach, as the crop fields' straw swaths
  already do (paler stripes in crop_ground()).
- Sod blocks breaking on landing (Blast) and carried off by the river (WorldWater flow at the foot).
