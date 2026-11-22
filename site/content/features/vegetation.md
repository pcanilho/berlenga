---
title: "Vegetation"
weight: 65
group: "World"
summary: "Biome plant placement from terrain maps; grass and generated meadow, forest floor, mushroom, stone and wetland plants in the weather's wind; GPU-placed instances seen by path tracing; per-instance bones."
---

## Placement

- **Plant placement** (`VegetationField3D`, `Biome`, `PlantSpecies`): places species by spacing, footprint and rules on slope, wetness and distance to water over a `TerrainDataMap3D`; evenly spread, the same for a seed. 12,899 plants over 256 m place once in about 45 ms, then draw as ordinary MultiMeshes.

  ![A valley with trees, bushes and reeds placed by biome rules, raster and path traced](media/vegetation-placement.jpg)

  *Plant placement: trees on dry ground, bushes between them, reeds along the banks (placeholder meshes).*

- **Streamed placement** (`VegetationField3D.streaming`, `stream_radius`, 200 m default; `still_shot`): tiles are placed on worker threads around the camera, identical to a full placement, and `still_shot` stops streaming once the view settles; 2 to 3 ms of main-thread time a frame through 1 km of oak wood.
- **Placement by geology** (`TerrainDataMap3D.geology_source`; `PlantSpecies` `soil_depth_range`, `rock_range`, `talus_range`): plants follow the generated soil, rock and scree; trees stay off bare rock and thin soil, grass thins on rock.
- **Placement by climate** (`temperature_range`, `january_temperature_range`, `precipitation_range`, `aridity_range`, `sunlight_range`): species by temperature, winter frost, rain, aridity and sunlight.
- **Density masks** (`density_mask`, `density_mask_rect`; per species `mask_channel`, `mask_invert`): a painted texture thins or clears any species per channel, for clearings and paths.
- **Worn paths** (`VegetationField3D.paths`, `GrassField3D.paths`): along paths, plants and grass thin out and shorten by how well they resist trampling, leaving bare trodden soil.
- **Cereal crops** (`GrassField3D.crop`, `crop_stage`): wheat, barley, oats and rye as real plants in drill rows, with spikelet ears and barley awns, through their growth stages from green rows to gold ears and stubble with straw swaths.
- **Crop fields** (`GrassField3D.fields`): field outlines with their own drill direction, sowing date, tramlines and headland; the ground carries the crop's colour into the distance.
- **Hedgerows and margins** (`GrassField3D.hedges`, `hedge_voxel_lods`, `PlantGenerator.make_margin()`): field edges carry hedgerows of mixed native shrubs with a gateway per field, and headland and verge strips of field weeds; far hedges switch to voxel levels.
- **Clear of boulders** (`VegetationField3D.obstacles`, `GrassField3D.obstacles`): grass, ground plants, tree feet and fallen logs keep out of boulders.
- **Waterfall spray** (`PlantSpecies.spray_range`): plants keep out of the soaked ground in a waterfall's spray zone, where only mosses grow.
- **Plant cover map** (`VegetationField3D.cover_map_enabled`): a colour map of the plant cover baked from the planting rules, for distant terrain.
- **Canopy shade** (`canopy_radius`, `shade_range`; `weather_map`): understorey placed by the shade of placed crowns; crowns feed the weather's canopy map.

## Grass

- **Grass** (`GrassField3D`): blades grow round the camera in clumps, follow `TerrainDataMap3D` rules, thin with distance, bend in the `WeatherMap3D` wind (each blade answers gusts at its own frequency, about 2 Hz, and the sway rolls downwind as waves) and darken in rain; near tall grass at 120 blades per m²; the sward darkens inside from its leaf area (raster AO) and casts its blades' true shadow; path traced. Highlights run along each blade over a folded mid vein, raster and path traced alike (`fibre_shading`).

  ![A dense meadow close up, raster and path traced](media/grass-field.jpg)

  *Grass: the same blades in raster and path tracing; path tracing adds the light bounced between them.*

  ![Grass bending as gusts travel downwind, raster](media/grass-wind.gif)

  *Grass in wind: gusts sweep across the field downwind.*

- **Grass on the GPU** (`GrassField3D.gpu_fill`): grass tiles are filled in compute, about 40% less main-thread cost per tile.
- **Grass edge fade** (always on): grass holds full density over the inner half of the grass radius, then blades thin and shrink across the outer band, and the ground takes their colour beyond (measured from a render of the blades, so any grass look matches), so the grass has no hard edge; it thins evenly with distance by blue-noise blade ranks, with no clumps or gaps.

  ![Grass seen from above its radius: a hard edge before, a fade after](media/grass-outer-band.jpg)

  *Grass edge fade: the band where grass ends blends into the ground.*

- **Far grass in sunlight** (`GrassField3D.sun_light`): distant grass brightens looking away from the sun and darkens looking into it, measured from the blades at four sun headings.
- **Grass edge match** (`GrassField3D.far_match_view`): after each bake the ground past the grass is matched to the blades in view, so there is no brightness step where the blades end.

- **Grass under undergrowth** (`GrassField3D.undergrowth_field`, `PlantSpecies.sward_shade`, `PlantSpecies.litter_color`): grass thins and shortens under bracken and bramble stands, as their shade kills the sward, and the stands lay their own litter beneath; thin margins keep their grass.

  ![A bracken stand in a meadow, grass through it and shaded out, raster](media/grass-undergrowth.jpg)

  *Grass under undergrowth: the grass under the bracken gives way to its litter.*

- **Seasonal grass** (`GrassField3D.season_source`): grass browns in drought and hard cold and stays green in a mild winter, read from the season.

  ![The same meadow in July, a normal year and a summer drought, raster](media/grass-drought.jpg)

  *Seasonal grass: in a summer drought whole blades die back to straw.*

- **Layered meadow** (`GrassField3D.color_variation`, `get_ground_shader_code()`, `canopy_field`): blades vary in colour over the field and per blade, some dead from the root, with midribs and tapered tips; the ground under the grass becomes matching turf, and the sward follows the canopy shade.

  ![A meadow by a lake, before and after the layered meadow](media/meadow-layers.jpg)

  *Layered meadow: finer, denser grass with turf between the blades instead of bare ground.*

- **Trampling** (`grass_trample` group, `GrassField3D.trample_recovery`): bodies lay grass down and it springs back; grass, flowers and herbs lie flat on light ground prints and are gone from deep ruts and puddles. Lying snow presses grass, crops and plants down under its weight, soft stems first, and buries short grass; a river's current lays the plants under it downstream, fully flat in a fast stream. Wet gales lodge tall grass flat in swirled patches that stand again over days (`GrassField3D.lodging`).

  ![A body walking through grass that lies down and springs back, raster](media/grass-trample.gif)

  *Trampling: the grass lies down under the body and springs back behind it.*

- **Ground cover** (`GrassField3D` undercoat fields, `view_facing`; `PlantSpecies` patches): edge-on blades widened, ground tinted under grass, clover and flower drifts.

  ![Sparse grass before, then with ground cover](media/grass-carpet.jpg)

  *Ground cover: bare ground between blades is filled with undercoat, clover and flowers.*

## Plants

- **Plant generator** (`PlantGenerator`, 45 presets): meadow plants including tall flowers (knapweed, field scabious, wild carrot, common sorrel, some in seed), forest floor and wetland plants with seed variations, phyllotaxis, back-lit leaves with generated vein textures, and distance LODs that drop whole leaf cards and enlarge the rest to keep coverage (112k to 70k triangles on a heath), for `PlantSpecies.meshes`.

  ![Generated dandelions and plantains, three seeds each](media/plant-generator.jpg)

  *Plant generator: three seeds each of dandelion and plantain.*

  ![A meadow with tall flowers among the grass, raster](media/tall-meadow.jpg)

  *Meadow flowers: daisies, buttercups, scabious and sorrel standing above the grass.*

- **Plants by season** (`PlantGenerator.season`): 17 flowering herbs open, go to seed and die back to standing stalks by degree days; fern croziers unroll in spring, fronds die back russet and lie flat through winter, and wood anemone dies down after flowering.

  ![A bracken stand on beech litter in July and December, raster](media/plants-season-bracken.jpg)

  *Plants by season: by December the bracken stands dead and russet.*

- **Forest floor** (`PlantGenerator.make_floor(stand, variants)` for 10 stand types, plus `FLOOR_MEDITERRANEAN` for montado and dehesa and `FLOOR_BOREAL_TAIGA` for dry boreal pine; `VegetationField3D.forest_floor_enabled`): herbs, mosses and seedlings by the stand above (seedlings of the stand's own trees, with their leaf forms; dog-violets and herb Robert in the lighter patches), and the ground under closed canopy turns to leaf, needle or moss litter. Under boreal pine, pale reindeer lichen carpets the floor in lumpy cushions, crushed underfoot and slow to return after fire.
- **Stand age** (`PlantSpecies.RULE_STAND_AGE`, `RULE_DISTURBANCE`): plants follow the years since the last fire, windthrow or beetle kill, so pioneers flood a fresh burn or blowdown and shade plants return as the stand closes; the ground is re-placed live when a disturbance moves a species across its range.
- **Deer browsing** (`WorldStand.add_browse()`, `PlantSpecies.RULE_BROWSE`, `PRESET_GRAZED_LAWN`; `GrassField3D.graze_height`): where deer feed, bramble and raspberry are bitten low, palatable herbs thin, the floor turns grassy, the grass is grazed short and clearings become close-cropped deer lawns.
- **Game trails** (`VegetationField3D.make_game_trails()`; `GroundSurface3D.trail_pugging`, `trail_wallows`): animals wear a branching network of trails between cover, feeding lawns and water, routed by least cost over slope and water and merging where traffic runs; grass wears to trodden soil along them, stones stand proud on the tread, the packed ground pools rain first, and wet trails churn into hoof-pocked mud where traffic is heaviest, and animals roll muddy wallows into wet level stretches, more in warm weather.
- **Avalanche tracks** (`PlantGenerator.make_avalanche_track()`): ground an avalanche cleared shows torn turf and debris, then fireweed, ferns and tall forbs.
- **Boar rooting** (`PlantGenerator.make_rooting()`, `ROOTING_WOOD`, `ROOTING_MEADOW`; `WorldStand.DISTURBANCE_ROOTING`): rooted ground shows turned sods and loose stones that dry, slump and green over, loses its plants and takes the first weeds, healing over about two years.
- **Grazing roughs and dung** (`PlantGenerator.make_grazing()`): cattle pats ringed by ungrazed rank tufts, horse latrines and deer pellets on pastures and lawns.
- **Fairy rings** (`GrassField3D.fairy_rings`, `PlantGenerator.make_fairy_rings()`): meadows carry rings of dark and dead grass with champignons along them.
- **Dewy webs** (`PlantGenerator.make_dew_webs()`): sheet and orb spider webs show when dew or hoarfrost beads them.
- **Anthills and molehills** (`PlantGenerator.make_mounds()`, `MOUNDS_MEADOW`, `MOUNDS_WOOD`): anthills in sunny meadow groups, wood ant nests at sunny gaps and edges of conifer stands, molehills in lines and arcs; grass keeps out from under them.
- **Stand margins** (`PlantSpecies.margin_width`, `margin_scale`, `margin_lean`): bracken and bramble stands grow shorter and lean outward toward their edges.
- **Rock foot** (`PlantSpecies.rock_foot`): ferns, mosses, wood sorrel and seedlings gather within about half a metre of a rock's foot.
- **Plants on and between stones** (`PlantSpecies.stone_top`, `PlantSpecies.stone_gap`, `PlantGenerator.make_stones()`): moss grows on the tops of generated stones, green cushions on low, shaded tops and hoary yellow-olive ones on high, sunlit tops (`PlantSpecies.stone_top_damp`), and crevice ferns, stonecrop, saxifrage and thrift only in the gaps of scree, gravel and shingle.

  ![A dense block field from 2 m up, without and with stone plants, raster](media/vegetation-stones.jpg)

  *Plants on and between stones: moss cushions on the block tops, a fern and small plants in the gaps.*

  ![Scree blocks at a low side sun, moss on a shaded low stone and on sunlit high tops, raster](media/vegetation-stone-moss.jpg)

  *Stone-top moss: green cushions on the low stone in shade, yellow-olive ones on the sunlit tops.*

- **Plants on deadwood** (`PlantSpecies.deadwood_rule`): ground plants keep off fallen logs, and moss, wood sorrel, seedlings and ferns grow on and along them.
- **Woodland mushrooms** (`PlantGenerator`, `PlantSpecies.patch_ring`, `PlantSpecies.deadwood_from_wood`): fly agaric, cep, chanterelle, brittlegill, clouded agaric and honey fungus, with stalks, caps, gills or tubes, rings and warts, come up under their host trees from late summer to the first frost; honey fungus grows in clumps out of the side of deadwood, clouded agaric in fairy-ring arcs.
- **Leaf drift** (`PlantSpecies.litter_range`, `VegetationField3D.litter_wind`; `RULE_LITTER_DRIFT`, `PRESET_LEAF_BANK`): fallen leaves drift past the crowns into edges and clearings, farther downwind, pile into hollows and against trunks, logs and stones, and leave ridges swept bare.
- **Falling leaves** (`LeafFall3D`, `creep_speed`, `field_litter`): leaves fall from the crowns through the wind as autumn sheds them, slide and flip along the ground in a breeze to settle in the leaf drifts, and gusts and passing bodies lift them off the ground; on ponds and rivers they float and drift, and on snow they lie until it melts and brings them back out.
- **Wetland plants** (`PlantSpecies.floating`, `water_depth_range`): reed, bulrush, rush and water lilies zoned by water depth, lilies floating on the surface.

  ![A swamp with reeds, bulrushes and water lilies, raster and path traced](media/wetland-plants.jpg)

  *Wetland plants: reeds and bulrushes stand in the shallows, lilies float.*

- **Waterside plants** (`PlantGenerator.make_banks()`, `PlantSpecies.stone_top_water`, `follow_flow`): marsh marigold, meadowsweet and butterbur line streams, lakes and falls by distance to water; river stones carry river moss and liverwort by height over the water, the moss trailing downstream.
- **Bog plants** (`PlantGenerator.make_bog()`, BOG_BLANKET or BOG_VALLEY): sphagnum hummocks, cotton grass, bog asphodel and sundew on wet level ground.
- **Atlantic heath** (`PlantGenerator.make_heath()`): bell heather, cross-leaved heath and crowberry in dry, wet and cliff-top heath, placed by rain, winter cold and wetness.
- **Mediterranean scrub** (`PlantGenerator.make_scrub()`): rosemary, French lavender, thyme and sage-leaved rockrose, as garrigue on thin soil and maquis on deeper soil where winters are mild and summers dry; the rockrose folds and greys its leaves in drought.
- **Dune plants** (`PlantGenerator.make_dunes()`; `GroundSurface3D.wind_shelters`): marram, lyme grass and sea holly on sand; they shelter it from the wind, so blowing sand piles into mounds round them.
- **Salt marsh and shingle plants** (`PlantGenerator.make_saltmarsh()`): glasswort, cord-grass, sea purslane, sea aster and sea lavender zoned by height above the tide on sheltered marsh; sea kale, sea campion and horned-poppy on shingle backshores.
- **Alpine and scree plants** (`PlantGenerator.make_alpine()`): fellfield, alpine grassland, snowbed and scree sets above the treeline, placed by climate, curvature, sunlight, soil depth and talus.
- **Desert plants** (`PlantGenerator.make_desert()`, `DESERT_OASIS`; `PlantSpecies.RULE_SPRING`, `RULE_POOL_DISTANCE`; `VegetationField3D.spring_pools`): oases grow at the terrain's springs with date palms, olives and a ring of shrubs, reeds and rushes edge the spring pools, and each nebkha mound holds a shrub.
- **Beaver ponds and meadows** (`PlantGenerator.make_beaver()`): pond plants by water depth, and after the dam fails a meadow that runs from rush and sedge through tall grass, buttercup, meadowsweet and marsh marigold over the decades.

- **Leaf wear** (`PlantGenerator.leaf_wear`): insect bites, uneven halves, dead tips and older leaves yellowing and browning, per leaf.

  ![A dock plant without and with leaf wear](media/leaf-wear.jpg)

  *Leaf wear: lower leaves yellow and brown, margins bitten.*

- **Bending** (`GrassField3D` trample field, no colliders): plants bend away from bodies and spring back, and lean in the wind.

  ![Nettles standing, then bent away from a body](media/plants-bend.jpg)

  *Bending: the nettles part round the body.*

- **Ground mist** (`VegetationMist3D`, `mist_amount`; needs PhysX Flow): at dawn, low mist pools in hollows and drifts through the grass, set by the weather and sun; bodies part it, and it burns off as the sun climbs.
- **Pollen puffs** (`VegetationPollen3D`; needs PhysX Flow): brushing grasses and other wind-pollinated plants in flower releases a puff of pollen; each plant recharges, and wet plants stay still.

- **Sun and shade growth** (always on): grass and ground plants take the colour and height of the light they grow in, like sun and shade leaves.
- **Per-stem sway** (always on): each frond, cane and stem rocks about its own base at a frequency set by its height and stiffness.
- **Near and far meshes** (`PlantSpecies.near_meshes`, `near_range`): heavier meshes near the camera, crossfaded with dithering a few plants at a time.
- **Far thinning** (`PlantSpecies.far_color`; VegetationField3D `far_reach`, `far_shell`): plants thin out one by one in blue-noise order with distance, out to three times their range, and leave their colour on the ground beyond, with no tile pops; taller cover drapes on to the horizon as a shell.
- **Far plants as volume** (`VegetationField3D.far_volume`): past their range, plants keep a porous voxel shell of their leaf cover out to 150 m.
- **Plant impostors** (`PlantGenerator.make_impostor()`, `add_impostors()`, `impostor_range`): large plants turn into octahedral impostors past a range in raster (brambles and bracken past 15 m), while path tracing keeps the meshes; an oak wood view drops from 3.91M to 2.98M triangles.

## Instances

- **GPU-placed instances** (`RenderingServer.multimesh_mark_buffer_rd_changed()`): MultiMesh records written by a compute shader reach culling, motion vectors and path tracing.

  ![GPU-written MultiMesh records, path traced: before and after the call](media/gpu-multimesh-records.jpg)

  *GPU-placed instances: without the call the GPU-written instances never appear.*

- **Instance bones** (`MultiMesh.bones_per_instance`): each instance is skinned from its own records, so thousands of plants bend in the wind without a Skeleton3D each; raster and path traced. 900 swaying stems: 0.30 ms render CPU, against 5.97 ms as 900 Skeleton3D.

  ![Stems in rest pose, then bent by instance bones in one MultiMesh, path traced](media/instance-bones.jpg)

  *Instance bones: one MultiMesh, every stem bent by its own bones.*
