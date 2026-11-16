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
