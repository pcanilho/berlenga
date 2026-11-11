---
title: "Terrain"
weight: 50
summary: "Mountain ranges raised and eroded per seed, with geology, river networks, coasts and lakes; non-destructive height brushes; terrain data maps."
---

- **Mountain generation** (`TerrainGenerator3D`: `uplift`, `ranges`, `erodibility`, `critical_slope`, `detail_wavelength`, `detail_octaves`, `detail_crest`): raises ranges from an uplift map, Path3D ranges and noise, then erodes them to equilibrium with stream power erosion; branching gullies come from an erosion filter computed per point, so tiles join exactly; on crests gullies keep part of their depth and a ridged noise adds irregular pinnacles; the same for a seed; writes into Terrain3D or a HeightMapShape3D.
- **River deposition** (`deposition`, off by default): rivers drop sediment where they slow (Yuan et al. 2019), building flat alluvial valley floors.
- **Coarse-to-fine erosion** (`coarse_levels`, `refine_steps`): about 3.7 times faster at 4 m cells, with the same relief.
- **Tile erosion** (`tile_erosion_enabled`): droplet hydraulic and thermal erosion in seamless overlapping tiles.
- **Material detail** (`get_detail_shader_code()`): erosion detail below the vertex spacing in the terrain material; path tracing keeps it.
- **Rock layers and waterfalls** (`strata_enabled`, `get_waterfalls()`): hard and soft bands that rivers cut into caprock waterfalls.
- **Rivers fed by rain and snowmelt** (`climate_recipe`, `river_month`): a monthly water balance from the climate sets each river's flow, month by month.
- **Geology maps** (`terrain_geology()` in shaders): soil, bare rock and scree from the erosion run.
- **River network** (`build_river_paths()`): rivers with widths and discharges, made into joined `PhysXRiver3D` paths.
- **Coast and lakes** (`ocean_edges` or `sea_map`; `lake_count`, `lakes`; `build_water()`): sea with a Dean-profile seabed and beaches; lakes behind natural dams or rock lips that spill as waterfalls; builds the sea and lake surfaces, and rivers end in them.
- **Climate maps** (`TerrainDataMap3D.bake_climate()`, `ClimateRecipe`): temperature, orographic rain and rain shadows, snow share, aridity, winter and annual sunlight with terrain shadows, and wind exposure.
- **Ground material** (`TerrainMaterial3D`, `use_generator()`): rock, scree, soil, alluvium and sand from the generator's geology, height-blended, biplanar cliffs, erosion detail, wetness and snow.
- **Landscape3D** (`source`, `spacing`, `lod_error`): heightfield ground from a TerrainGenerator3D or a height image, as chunk meshes with crack-free LODs and one BLAS per chunk level, so nothing rebuilds while the camera moves.
- **Footprints in ground** (`GroundSurface3D`, `BaseMaterial3D.ground_deform_enabled`): prints in sand, soil, mud and leaf litter; moisture from the weather map, walls slump to the angle of repose.
- **Terrain deformation** (`TerrainDeformer3D`, `TerrainBrush3D`): non-destructive height brushes (rivers, craters, plots) over Terrain3D or a height map; moving a brush restores the ground.
- **Terrain data maps** (`TerrainDataMap3D`): bakes slope, curvature, water flow, wetness, water depth and distance to water around a point, for placing plants and for materials; a 512 map bakes once in about 0.2 s.

## Images

![Generated mountain range, shaded relief with rivers](media/terrain-generator-map.jpg)

*Mountain generation: ridges, valleys and the rivers that carved them, from one seed.*

![The same range eroded directly and coarse to fine, 8 m cells](media/terrain-coarse-to-fine.jpg)

*Coarse to fine: the same relief at 8 m cells in a third of the time.*

![The generated range with erosion detail, raster](media/terrain-detail-wide.jpg)

*Mountain generation: branching gullies down every slope.*

![Close view of the generated peaks, raster and path traced](media/terrain-generator.jpg)

*Mountain generation up close, raster and path traced.*

![Summit crests: a regular comb before, irregular crests now](media/terrain-crest.jpg)

*Crests: before, a thin regular comb along the summits; now, irregular crests and pinnacles.*

![The same slopes without and with tile erosion](media/terrain-tile-erosion.jpg)

*Tile erosion: fine branching rills cut into every slope.*

![A 4 m terrain mesh alone, then with erosion detail in its material](media/terrain-material-detail.jpg)

*Material detail: rills finer than the 4 m mesh, drawn by the material.*

![Rivers and waterfalls on a layered range, and river long profiles before and after](media/terrain-waterfalls.jpg)

*Rock layers: rivers step down over the hard bands, leaving waterfalls up to 30 m.*

![A generated range around a wide flat valley floor that runs down to the beach, raster](media/terrain-deposition.jpg)

*River deposition: the rivers fill the valley between the ridges with a flat alluvial floor.*

![River flow by month in three climates, and snow lying by height](media/terrain-runoff.jpg)

*Rivers by month: a spring snowmelt flood in cold mountains, winter or summer high water in mild climates.*

![Geology map with soil, rock, scree and rivers, and the range path traced and coloured from it](media/terrain-geology.jpg)

*Geology: soil green, bare rock grey, scree ochre, rivers blue; the shader colours the range from the map.*

![A generated island map: sea, beaches, rivers and two dammed lakes](media/terrain-coast-map.jpg)

![The island, raster and path traced](media/terrain-coast.jpg)

*Coast and lakes: beaches, a shelving seabed and lakes behind dams, from one seed.*

![Climate maps of a range: temperature, precipitation, winter sun, snow share](media/terrain-climate.jpg)

*Climate maps: cold high ridges, rain over the range, shaded faces in winter, snow on the high crest.*

![A generated range with its ground material under the cloud layer: rock, scree, soil, forest and a sand beach, raster vs path traced](media/terrain-ground.jpg)

*Ground material: rock on crests and cliffs, grass and soil on the slopes, a sand beach at the coast, all from the generator's geology.*

![A valley drawn by Landscape3D at 1 m spacing, raster vs path traced](media/landscape3d.jpg)

*Landscape3D: a 1 m heightfield with sharp ridges and a narrow valley floor, the same in raster and path tracing.*

![Footprints and tracks in dry sand, damp sand, soil, mud and leaf litter, raster and path traced](media/ground-footprints.jpg)

*Footprints: deepest in mud, shallow in dry sand where the walls slump.*

![Slope, curvature, flow, wetness and distance-to-water maps of a valley, and the valley coloured from them](media/terrain-data-maps.jpg)

*Terrain data maps: the stream bed, wet ground and banks come from the maps alone.*
