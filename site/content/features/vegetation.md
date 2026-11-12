---
title: "Vegetation"
weight: 60
summary: "Biome plant placement from terrain maps; grass and generated meadow, forest floor and wetland plants in the weather's wind; GPU-placed instances seen by path tracing; per-instance bones."
---

## Placement

- **Plant placement** (`VegetationField3D`, `Biome`, `PlantSpecies`): places species by spacing, footprint and rules on slope, wetness and distance to water over a `TerrainDataMap3D`; evenly spread, the same for a seed. 12,899 plants over 256 m place once in about 45 ms, then draw as ordinary MultiMeshes.

  ![A valley with trees, bushes and reeds placed by biome rules, raster and path traced](media/vegetation-placement.jpg)

  *Plant placement: trees on dry ground, bushes between them, reeds along the banks (placeholder meshes).*

- **Streamed placement** (`VegetationField3D.streaming`, `stream_radius`, 200 m default): tiles are placed on worker threads around the camera, identical to a full placement; 2 to 3 ms of main-thread time a frame through 1 km of oak wood.
- **Placement by geology** (`TerrainDataMap3D.geology_source`; `PlantSpecies` `soil_depth_range`, `rock_range`, `talus_range`): plants follow the generated soil, rock and scree; trees stay off bare rock and thin soil, grass thins on rock.
- **Placement by climate** (`temperature_range`, `january_temperature_range`, `precipitation_range`, `aridity_range`, `sunlight_range`): species by temperature, winter frost, rain, aridity and sunlight.
- **Density masks** (`density_mask`, `density_mask_rect`; per species `mask_channel`, `mask_invert`): a painted texture thins or clears any species per channel, for clearings and paths.
- **Waterfall spray** (`PlantSpecies.spray_range`): plants keep out of the soaked ground in a waterfall's spray zone, where only mosses grow.
- **Plant cover map** (`VegetationField3D.cover_map_enabled`): a colour map of the plant cover baked from the planting rules, for distant terrain.
- **Canopy shade** (`canopy_radius`, `shade_range`; `weather_map`): understorey placed by the shade of placed crowns; crowns feed the weather's canopy map.

## Grass

- **Grass** (`GrassField3D`): blades grow round the camera in clumps, follow `TerrainDataMap3D` rules, thin with distance, bend in the `WeatherMap3D` wind (each blade answers gusts at its own frequency, about 2 Hz, and the sway rolls downwind as waves) and darken in rain; near tall grass at 120 blades per m²; the sward darkens inside from its leaf area (raster AO) and casts its blades' true shadow; path traced.

  ![A dense meadow close up, raster and path traced](media/grass-field.jpg)

  *Grass: the same blades in raster and path tracing; path tracing adds the light bounced between them.*

  ![Grass bending as gusts travel downwind, raster](media/grass-wind.gif)

  *Grass in wind: gusts sweep across the field downwind.*

- **Grass on the GPU** (`GrassField3D.gpu_fill`, on by default): grass tiles are filled in compute, about 40% less main-thread cost per tile.
- **Grass edge fade** (always on): blades thin and shrink across the outer band of the grass radius, and the ground takes their colour beyond (measured from a render of the blades, so any grass look matches), so the grass has no hard edge; it thins evenly with distance by blue-noise blade ranks, with no clumps or gaps.

  ![Grass seen from above its radius: a hard edge before, a fade after](media/grass-outer-band.jpg)

  *Grass edge fade: the band where grass ends blends into the ground.*

- **Layered meadow** (`GrassField3D.color_variation`, `get_ground_shader_code()`, `canopy_field`): blades vary in colour over the field and per blade, some dead from the root, with midribs and tapered tips; the ground under the grass becomes matching turf, and the sward follows the canopy shade.

  ![A meadow by a lake, before and after the layered meadow](media/meadow-layers.jpg)

  *Layered meadow: finer, denser grass with turf between the blades instead of bare ground.*

- **Trampling** (`grass_trample` group, `GrassField3D.trample_recovery`): bodies lay grass down and it springs back.

  ![A body walking through grass that lies down and springs back, raster](media/grass-trample.gif)

  *Trampling: the grass lies down under the body and springs back behind it.*

- **Ground cover** (`GrassField3D` undercoat fields, `view_facing`; `PlantSpecies` patches): edge-on blades widened, ground tinted under grass, clover and flower drifts.

  ![Sparse grass before, then with ground cover](media/grass-carpet.jpg)

  *Ground cover: bare ground between blades is filled with undercoat, clover and flowers.*

## Plants

- **Plant generator** (`PlantGenerator`, 39 presets): meadow plants including tall flowers (knapweed, field scabious, wild carrot, common sorrel, some in seed), forest floor and wetland plants with seed variations, phyllotaxis, back-lit leaves with generated vein textures, and distance LODs that drop whole leaf cards and enlarge the rest to keep coverage (112k to 70k triangles on a heath), for `PlantSpecies.meshes`.

  ![Generated dandelions and plantains, three seeds each](media/plant-generator.jpg)

  *Plant generator: three seeds each of dandelion and plantain.*

- **Forest floor** (`PlantGenerator.make_floor(stand, variants)` for 10 stand types; `VegetationField3D.forest_floor_enabled`): herbs, mosses and seedlings by the stand above (seedlings of the stand's own trees, with their leaf forms; dog-violets and herb Robert in the lighter patches), and the ground under closed canopy turns to leaf, needle or moss litter.
- **Leaf drift** (`PlantSpecies.litter_range`, `VegetationField3D.litter_wind`): fallen leaves drift past the crowns into edges and clearings, farther downwind.
- **Wetland plants** (`PlantSpecies.floating`, `water_depth_range`): reed, bulrush, rush and water lilies zoned by water depth, lilies floating on the surface.

  ![A swamp with reeds, bulrushes and water lilies, raster and path traced](media/wetland-plants.jpg)

  *Wetland plants: reeds and bulrushes stand in the shallows, lilies float.*

- **Leaf wear** (`PlantGenerator.leaf_wear`): insect bites, uneven halves, dead tips and older leaves yellowing and browning, per leaf.

  ![A dock plant without and with leaf wear](media/leaf-wear.jpg)

  *Leaf wear: lower leaves yellow and brown, margins bitten.*

- **Bending** (`GrassField3D` trample field, no colliders): plants bend away from bodies and spring back, and lean in the wind.

  ![Nettles standing, then bent away from a body](media/plants-bend.jpg)

  *Bending: the nettles part round the body.*

- **Sun and shade growth** (always on): grass and ground plants take the colour and height of the light they grow in, like sun and shade leaves.
- **Per-stem sway** (always on): each frond, cane and stem rocks about its own base at a frequency set by its height and stiffness.
- **Near and far meshes** (`PlantSpecies.near_meshes`, `near_range`): heavier meshes near the camera, crossfaded with dithering a few plants at a time.
- **Far thinning** (`PlantSpecies.far_color`): plants thin out one by one in blue-noise order with distance and leave their colour on the ground beyond, with no tile pops.
- **Plant impostors** (`PlantGenerator.make_impostor()`, `add_impostors()`, `impostor_range`): large plants turn into octahedral impostors past a range in raster (brambles and bracken past 15 m), while path tracing keeps the meshes; an oak wood view drops from 3.91M to 2.98M triangles.

## Instances

- **GPU-placed instances** (`RenderingServer.multimesh_mark_buffer_rd_changed()`): MultiMesh records written by a compute shader reach culling, motion vectors and path tracing.

  ![GPU-written MultiMesh records, path traced: before and after the call](media/gpu-multimesh-records.jpg)

  *GPU-placed instances: without the call the GPU-written instances never appear.*

- **Instance bones** (`MultiMesh.bones_per_instance`): each instance is skinned from its own records, so thousands of plants bend in the wind without a Skeleton3D each; raster and path traced. 900 swaying stems: 0.30 ms render CPU, against 5.97 ms as 900 Skeleton3D.

  ![Stems in rest pose, then bent by instance bones in one MultiMesh, path traced](media/instance-bones.jpg)

  *Instance bones: one MultiMesh, every stem bent by its own bones.*
