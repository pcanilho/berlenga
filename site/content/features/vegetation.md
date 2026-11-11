---
title: "Vegetation"
weight: 60
summary: "Biome plant placement from terrain maps; grass in the weather's wind; GPU-placed instances seen by path tracing; per-instance bones."
---

## Placement

- **Plant placement** (`VegetationField3D`, `Biome`, `PlantSpecies`): places species by spacing, footprint and rules on slope, wetness and distance to water over a `TerrainDataMap3D`; evenly spread, the same for a seed. 12,899 plants over 256 m place once in about 45 ms, then draw as ordinary MultiMeshes.

  ![A valley with trees, bushes and reeds placed by biome rules, raster and path traced](media/vegetation-placement.jpg)

  *Plant placement: trees on dry ground, bushes between them, reeds along the banks (placeholder meshes).*

- **Placement by geology** (`TerrainDataMap3D.geology_source`; `PlantSpecies` `soil_depth_range`, `rock_range`, `talus_range`): plants follow the generated soil, rock and scree; trees stay off bare rock and thin soil, grass thins on rock.
- **Placement by climate** (`temperature_range`, `january_temperature_range`, `precipitation_range`, `aridity_range`, `sunlight_range`): species by temperature, winter frost, rain, aridity and sunlight.
- **Density masks** (`density_mask`, `density_mask_rect`; per species `mask_channel`, `mask_invert`): a painted texture thins or clears any species per channel, for clearings and paths.
- **Canopy shade** (`canopy_radius`, `shade_range`; `weather_map`): understorey placed by the shade of placed crowns; crowns feed the weather's canopy map.

## Grass

- **Grass** (`GrassField3D`): blades grow round the camera in clumps, follow `TerrainDataMap3D` rules, thin with distance, bend in the `WeatherMap3D` wind and darken in rain; path traced.

  ![A grass carpet with undercoat, clover and flowers, raster and path traced](media/grass-field.jpg)

  *Grass: a full carpet, raster and path traced.*

  ![Grass bending as gusts travel downwind, raster](media/grass-wind.gif)

  *Grass in wind: gusts sweep across the field downwind.*

- **Grass edge fade** (always on): blades thin and shrink across the outer band of the grass radius, and the ground takes their colour beyond, so the grass has no hard edge.

  ![Grass seen from above its radius: a hard edge before, a fade after](media/grass-outer-band.jpg)

  *Grass edge fade: the band where grass ends blends into the ground.*

- **Trampling** (`grass_trample` group, `GrassField3D.trample_recovery`): bodies lay grass down and it springs back.

  ![A body walking through grass that lies down and springs back, raster](media/grass-trample.gif)

  *Trampling: the grass lies down under the body and springs back behind it.*

- **Ground cover** (`GrassField3D` undercoat fields, `view_facing`; `PlantSpecies` patches): edge-on blades widened, ground tinted under grass, clover and flower drifts.

  ![Sparse grass before, then with ground cover](media/grass-carpet.jpg)

  *Ground cover: bare ground between blades is filled with undercoat, clover and flowers.*

## Instances

- **GPU-placed instances** (`RenderingServer.multimesh_mark_buffer_rd_changed()`): MultiMesh records written by a compute shader reach culling, motion vectors and path tracing.

  ![GPU-written MultiMesh records, path traced: before and after the call](media/gpu-multimesh-records.jpg)

  *GPU-placed instances: without the call the GPU-written instances never appear.*

- **Instance bones** (`MultiMesh.bones_per_instance`): each instance is skinned from its own records, so thousands of plants bend in the wind without a Skeleton3D each; raster and path traced. 900 swaying stems: 0.30 ms render CPU, against 5.97 ms as 900 Skeleton3D.

  ![Stems in rest pose, then bent by instance bones in one MultiMesh, path traced](media/instance-bones.jpg)

  *Instance bones: one MultiMesh, every stem bent by its own bones.*
