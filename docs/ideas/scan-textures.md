# Scanned textures on every natural surface

Paulo, 24 Nov: "we should be using CC0 or whatever is applicable textures whenever applicable like we did for the
ground", "I am seeing many stretched and untextured geometry", "even mud", "even sand on the beach", "the same applies
to everything else". His example: a scree burial scout (landscape bench, valley layout, --view=pedestals) where the
cliff walls behind the rubble and the ground between the stones read as flat tan paint.

## Technique (research)

What the open-world engines do with scanned ground, and what we already have:

- Scanned material libraries as layer detail: Frostbite procedural shader splatting (Andersson, SIGGRAPH 2007
  "Terrain Rendering in Frostbite Using Procedural Shader Splatting"), Far Cry 5 terrain (Moore, GDC 2018
  "Terrain Rendering in Far Cry 5"), Megascans in UE/Quixel "material blend" landscapes. Each surface class keeps a
  macro colour from data (geology, climate, wetness) and a scan supplies the metre-to-millimetre detail. Ours:
  TerrainMaterialLayer.detail_only (scan divided by its own mean, so the geology keeps the colour). Built 20 Nov.
- Hiding repeats: hex-tiling (Mikkelsen 2022, JCGT "Practical Real-Time Hex-Tiling", after Heitz and Neyret 2018
  histogram-preserving blending). Built (tm_hex). Over tens of metres hex tiling hides seams, not sameness: macro
  variation (UE "macro variation" / Far Cry 5 "colour and detail variation maps") and two scans per surface blended
  by a low-frequency mask (Witcher 3 terrain, Gollent GDC 2014 "Landscape Creation and Rendering in REDengine 3":
  two materials per texel, slope blended). We have the 9 m / 2.2 m tone; a second scan per class is new.
- Steep faces without stretching: tri/biplanar projection (Quilez 2020 "biplanar mapping"; Golus 2017 "Normal
  Mapping for a Triplanar Shader", whiteout/RNM blend). Built past cliff_slope. Gap: below cliff_slope a 40-50 degree
  face still uses plain XZ (stretch up to 1/cos = 1.6x at 50 deg) and cliff relief steps (vertical lips) take the
  heightfield's normal by design (4af2f284): a lip that reads as a near-vertical wall with XZ texture is stretched.
- Height blending between layers (Mishkinis 2013 "Advanced Terrain Texture Splatting"; UE height lerp). Built (h
  per layer, blend_depth). New overlays use the same rule at their edges.
- Snow, mud and wet ground keep their own physical shading (SnowSurface3D microstructure, the mud thread's Bingham
  layer, material wetness after Lagarde 2012); scans only add the detail those models do not make (fresh snow grain
  and crust, mud clods and tyre smear).

## Why Paulo's shot reads flat

- The scans live only in the art repository and only the landscape and rocks benches load them (fetch_art.sh, then
  `_apply_ground_scans`). A run in a container that never fetched art draws every layer flat (one log line says so).
  Fix: one engine loader (`TerrainMaterial3D.load_scan_set(dir)`, reading a scans.cfg manifest beside the scans) that
  every bench calls, which warns loudly when the folder is missing; the snow, links and ground benches call it too.
- Even with scans, the ground between the stones is a forest-floor scan (forest_ground_06) tinted to the climate's
  bare-soil or straw colour, and the cliffs are a 2 m dark_rock_02 tile whose fine relief is eased on faces. A
  forest-litter scan tinted straw is exactly "flat paint".

## Per system

Each surface system, what draws it today and what it should get: a scan (detail on the system's own colour), a
shader feature, real geometry, or nothing. Paulo, 24 Nov: "all this sort of systems should be queried to understand
if they should be augmented with shaders and or textures or more."

| Surface (owner thread) | Today | Augment with | Scans (CC0) and tile |
| --- | --- | --- | --- |
| Rock layer (terrain) | dark_rock_02 scan, fall-line streaks, bed bands | scan: second rock scan in 25 m patches | rock_surface 2 m |
| Cliff faces past ~45 deg (terrain) | the rock layer's ground scan, eased | scan: a face scan (beds, joints); stretch fix | rock_face_03 2.7 m |
| Scree aprons (terrain + rocks) | rocks_ground_02 + real stones near | keep; second scan later if repeats show | |
| Soil, humid sward (terrain + vegetation) | forest_ground_06 under GrassField3D blades | keep (blades are geometry) | |
| Cured straw cover, dry climates (terrain) | forest_ground_06 tinted 2.5Y 5/4 (Paulo: flat paint) | scan: grass thatch, luminance only | ambientCG Grass001 1.5 m |
| Bare soil patches (terrain) | forest_ground_06 tinted | scan: dry crusted soil | dry_ground_01 4 m |
| Bare alluvium, dry beds (terrain) | gravelly_sand tinted | scan: cracked silt crust | mud_cracked_dry_riverbed_002 2 m |
| Mud: ruts, digs, prints, wallows (mud) | soil scan darkened by its water model | scan on the turned soil; mud's own wetness stays | brown_mud_02 1.3 m |
| River banks (terrain bank relief, mud) | soil layer on the 42-52 deg bank faces | scan: wet bank soil with roots/leaves at the waterline band | brown_mud_leaves_01 1.3 m, mud_forest 2.35 m |
| River and lake beds, gravel bars (terrain + water) | gravelly_sand alluvium under the water | scan: rounded river gravel by depth and current | river_small_rocks 2.9 m, ganges_river_pebbles 2.16 m |
| Beach (terrain + sand) | sand_03 (same as dunes) + grain shader | scan: coastal sand; damp swash band | coast_sand_04 3.94 m, damp_beach_sand 2 m |
| Dunes (sand) | sand_03 + grain, ripples, dune tone shaders | keep: shader features fit the physics better | |
| Forest floor litter (vegetation) | stochastic leaf/needle sprite shader + 3D litter near | scan under the sprites for humus and fine litter between leaves; sprites and 3D litter stay | forest_leaves_02 3 m (broadleaf), forest_ground_04 3.15 m (pine needles), forest_floor 2.1 m |
| Moss carpets and cushions (vegetation, rocks) | procedural colour + real cushions | scan: moss detail on the cushion and carpet material | ambientCG Moss002 |
| Trails and walked ground (world links) | soil packed, cover removed | scan: compacted path soil | stony_dirt_path 2.17 m |
| Burn scars (world) | char tint | scan: burned ground | burned_ground_01 1 m |
| Snow (snow) | microstructure shaders: cups, sastrugi, strata, glints | scan normal and roughness only for grain and crust; shaders stay | snow_02 2 m, ambientCG Snow010A |
| Lake ice (snow) | procedural cracks and bubbles | shader: keep (Fresnel, bubbles are physical); scan optional for surface frost | ambientCG Ice002 |
| Rocks, boulders, scree stones (rocks) | procedural rock shader | scan detail triplanar on the stones, detail only | rock_boulder_dry 1.8 m, rock_surface 2 m |
| Cave walls and floors (rocks) | procedural | scan: wet limestone/rock face | rock_face_03, rock_wall_02 |
| Bark (trees) | procedural bark textures + modelled relief, lichen, peel | scan per species as detail (relief stays geometry) | pine_bark, bark_brown_02, bark_platanus, bark_willow_02, ambientCG Bark001-016 |
| Deadwood, logs, stumps, cut faces (trees) | procedural wood | scan: weathered wood and end grain | ambientCG Wood049-099 |
| Leaves (trees, plants) | procedural leaf textures | scan atlases: real leaf colour and venation | ambientCG LeafSet009, LeafSet010 |
| Grass blades (vegetation) | geometry, procedural colour | keep: blades are geometry, colour from season | |
| Clouds, sky, water surface, rain | physical models | nothing: no scan applies | |

Picking rule (from the 20 Nov round): low contrast around the mean (p98/mean under about 2), no bright debris, no
baked shadows deeper than the layer's own AO; roughness taken detail_only (Poly Haven roughness has no common scale).

## Plan

1. Shared loader and manifest (engine): `TerrainMaterial3D.load_scan_set(dir)` reads `scans.cfg` (section per slot:
   scan, tile, maps) and builds the textures with mipmaps; returns the number of slots filled and warns when the
   folder is missing. The benches call it instead of their own copies.
2. Terrain overlays: new optional TerrainMaterialLayer slots `cliff`, `cured`, `bare`, `beach`, `bed`, `mud`, each a
   scan blended over its parent layer by the mask the shader already computes (cliff weight, cured share, bare
   patches, beach mask, water depth, ground deform mud), with the height-blend rule at the edge and the parent's
   colour kept (detail_only). Second scans for rock and scree (`variant_layer`) mixed by a 25 m noise.
3. Stretch fixes: biplanar from 35 degrees (not cliff_slope) with the same weights; cliff relief lips sample by the
   displaced face normal for texture projection only (layer choice stays on the heightfield normal).
4. Then the owning threads: rocks (detail scan in the rock shader), snow (snow_02 detail), mud (mud slot weights
   from its own maps), trees (bark detail per species).
5. Raster vs PT at 100% crop for each, PT frame before push.

Credits: one line per scan in the README References and CREDIT.txt beside the files in the art repository.

## Built (24 Nov, terrain side)

- `TerrainMaterial3D.load_scan_set(dir, slots, maps)` reads `scans.cfg` beside the scans (art repository
  terrain/layers, art 037232a; fetch_art.sh). Both benches call it; a missing folder warns instead of drawing every
  layer flat in silence. Rocks bench: `--overlay-scans=0` (the five layers' scans only), `--layer-tint` (each layer one
  flat colour, to see which layer draws what), `--geo-at=x,z` (the generator's geology round a point), `--scan-check`.
- Overlay slots (`cliff_scan`, `rock_variant_scan`, `cured_scan`, `bare_soil_scan`, `bare_alluvium_scan`,
  `beach_scan`, `mud_scan`): each replaces its parent layer's scan detail by the shader's own mask (rock faces past
  about 45 degrees, 25 m patches, the cured share, the bare patches, sand within beach_height + 4 m of the sea,
  GroundSurface3D's exposed soil), height blended at the edge. The parent keeps its colour and roughness.
- Paulo's shot (rocks bench `--world --view=scree --snow=2`): `--layer-tint` showed the tan walls were the alluvium and
  sand layers. Layers go by the heightfield's normal under cliff relief (so treads keep soil), which also gave the
  relief's risers the tread's layer. Faces whose own normal is past about 55 degrees are now rock (no loose layer
  holds past its angle of repose).
- Rock faces had half the scan's tone and 85% of its relief eased away (a ground scan seen from above read as draped
  cloth on a wall). With a face scan in the cliff slot nothing is eased.
- Macro variation: the rock, cliff and rock-variant scans are sampled again at six times their size (tone 0.6, relief
  0.7), so walls tens of metres off keep metre-scale structure instead of a filtered even tone. Rock only: loose
  grains and stones are not self-similar over that range.
- Biplanar projection from 35 degrees instead of the cliff slope (50): 40-50 degree slopes no longer stretch 1.6x, and
  there is no seam where the projection switched.

- Second batch (art adds excavated_soil_wall, dirt_floor, river_small_rocks): `bank_scan` on soil and alluvium faces
  of 42 to 53 degrees by the surface's own normal (a sward does not close over them; past 55 they are rock),
  `trail_scan` where the stand map's trail wear opens the cover (before the trail's own tint and tread), `bed_scan` on
  alluvium where the drainage area is a hundred times alluvium_area's on gentle ground (bedload channels keep rounded
  gravel, not silt). The layer functions take a second mask vector (om2).

Next on the terrain side: burn scars (WorldBurnScar3D draws its own char), waterline band on banks. Then the owning threads: forest floor and moss (vegetation), stones and caves
(rocks), snow detail (snow), bark, deadwood and leaves (trees), mud weights from its own maps (mud).
