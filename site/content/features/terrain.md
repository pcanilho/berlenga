---
title: "Terrain"
weight: 50
summary: "Mountain ranges raised and eroded per seed, with geology, river networks, coasts and lakes; non-destructive height brushes; terrain data maps."
---

## Generation

- **Mountain generation** (`TerrainGenerator3D`: `uplift`, `ranges`, `erodibility`, `critical_slope`, `detail_wavelength`, `detail_octaves`, `detail_crest`): raises ranges from an uplift map, Path3D ranges and noise, then erodes them to equilibrium with stream power erosion; branching gullies come from an erosion filter computed per point, so tiles join exactly; on crests gullies keep part of their depth and a ridged noise adds irregular pinnacles; the same for a seed; writes into Terrain3D or a HeightMapShape3D.

  ![Generated mountain range, shaded relief with rivers](media/terrain-generator-map.jpg)

  *Mountain generation: ridges, valleys and the rivers that carved them, from one seed.*

  ![The generated range with erosion detail, raster](media/terrain-detail-wide.jpg)

  *Mountain generation: branching gullies down every slope.*

  ![Close view of the generated peaks, raster and path traced](media/terrain-generator.jpg)

  *Mountain generation up close, raster and path traced.*

  ![Summit crests: a regular comb before, irregular crests now](media/terrain-crest.jpg)

  *Crests: before, a thin regular comb along the summits; now, irregular crests and pinnacles.*

- **Coarse-to-fine erosion** (`coarse_levels`, `refine_steps`): about 3.7 times faster at 4 m cells, with the same relief.

  ![The same range eroded directly and coarse to fine, 8 m cells](media/terrain-coarse-to-fine.jpg)

  *Coarse to fine: the same relief at 8 m cells in a third of the time.*

- **Tile erosion** (`tile_erosion_enabled`): droplet hydraulic and thermal erosion in seamless overlapping tiles.

  ![The same slopes without and with tile erosion](media/terrain-tile-erosion.jpg)

  *Tile erosion: fine branching rills cut into every slope.*

- **Material detail** (`get_detail_shader_code()`): erosion detail below the vertex spacing in the terrain material; path tracing keeps it.

  ![A 4 m terrain mesh alone, then with erosion detail in its material](media/terrain-material-detail.jpg)

  *Material detail: rills finer than the 4 m mesh, drawn by the material.*

- **Rock layers and waterfalls** (`strata_enabled`, `get_waterfalls()`): hard and soft bands that rivers cut into caprock waterfalls.

  ![Rivers and waterfalls on a layered range, and river long profiles before and after](media/terrain-waterfalls.jpg)

  *Rock layers: rivers step down over the hard bands, leaving waterfalls up to 30 m.*

- **Glacial erosion** (`glacial_enabled`, `glacial_ela`, `glacial_erosion`): ice ages alternate with the rivers and carve troughs, cirques and basins that become lakes. With rock quality varied (`glacial_rock_variation`, `glacial_rock_scale`, off by default) the ice digs its own basins and bars, so tarns and chains of lakes form with no markers. Glaciers from beyond the map edge flow in with their rivers and carve the main valley's trough and lakes (`glacial_inflow_balance`, m/yr, 0 = off). A coarser ice grid (`glacial_coarse_cell`) is about 3 times faster, with similar but not identical terrain.
- **Moraines** (`moraine_age`, years, 0 = off): debris carried down the last glaciers builds ridges at their margins that dam lakes. Till draws as soil with stones through it, the stones kept above the grass.
- **Ice-worn rock** (`ice_worn_detail`): faces toward the old ice flow are smoothed, faces away are stepped, with striations.
- **Cliff ledges** (`cliff_ledge_spacing`, `cliff_ledge_strength`): rock beds of uneven thickness step faces steeper than 40 degrees into ledges.
- **Scree** (`scree_age`): rock walls shed aprons and cones at their feet, kept out of lakes and river floors. Scree draws as bare blocks.
- **Fallen blocks** (`get_boulders(rect, density)`): blocks shed by cliffs rest below them, the largest furthest out.

## Rivers, coasts and lakes

- **River deposition** (`deposition`, off by default): rivers drop sediment where they slow (Yuan et al. 2019), building flat alluvial valley floors; solved exactly per step along the river tree, at about half the earlier cost.

  ![A generated range around a wide flat valley floor that runs down to the beach, raster](media/terrain-deposition.jpg)

  *River deposition: the rivers fill the valley between the ridges with a flat alluvial floor.*

- **Meandering rivers** (`meander_age`, years, off by default): bends grow and migrate across flat valley floors; cut-off loops leave oxbows that silt up, and the river water follows the new course.

  ![A river across a valley floor before and after 3000 years of meandering, raster](media/terrain-meanders.jpg)

  *Meandering rivers: after 3000 years the river winds in loops across the floor.*

- **Rivers fed by rain and snowmelt** (`climate_recipe`, `river_month`): a monthly water balance from the climate sets each river's flow, month by month.

  ![River flow by month in three climates, and snow lying by height](media/terrain-runoff.jpg)

  *Rivers by month: a spring snowmelt flood in cold mountains, winter or summer high water in mild climates.*

- **River network** (`build_river_paths()`): rivers with widths and discharges, made into joined `PhysXRiver3D` paths.
- **River sites** (`get_river_sites(min_area, min_discharge, run)`): position, discharge, catchment, drop and run along every river, for placing lakes and falls.
- **Waterfall records** (`get_waterfalls()`): each fall's type, lip, face profiles, tiers, power, impact speed and plunge pool, for water effects.
- **Plunge pools and gorges** (`plunge_pools`, `gorge_age`, `gorge_rate`): falls of 4 m or more scour a pool that holds water, and lips over hard rock wear back upstream into slot gorges.
- **Spray map** (`terrain_spray_potential`, `get_spray_potential()`): where fall spray keeps the ground wet, from each fall's power and distance.
- **Coast and lakes** (`ocean_edges` or `sea_map`; `lake_count`, `lakes`; `build_water()`): sea with a Dean-profile seabed and beaches; lakes behind natural dams or rock lips that spill as waterfalls; builds the sea and lake surfaces, and rivers end in them.

  ![A generated island map: sea, beaches, rivers and two dammed lakes](media/terrain-coast-map.jpg)

  ![The island, raster and path traced](media/terrain-coast.jpg)

  *Coast and lakes: beaches, a shelving seabed and lakes behind dams, from one seed.*

## Maps

- **Geology maps** (`terrain_geology()` in shaders): soil, bare rock and scree from the erosion run.

  ![Geology map with soil, rock, scree and rivers, and the range path traced and coloured from it](media/terrain-geology.jpg)

  *Geology: soil green, bare rock grey, scree ochre, rivers blue; the shader colours the range from the map.*

- **Climate maps** (`TerrainDataMap3D.bake_climate()`, `ClimateRecipe`): temperature, orographic rain and rain shadows, snow share, aridity, winter and annual sunlight with terrain shadows, and wind exposure.

  ![Climate maps of a range: temperature, precipitation, winter sun, snow share](media/terrain-climate.jpg)

  *Climate maps: cold high ridges, rain over the range, shaded faces in winter, snow on the high crest.*

- **Terrain data maps** (`TerrainDataMap3D`): bakes slope, curvature, water flow, wetness, water depth and distance to water around a point, for placing plants and for materials; a 512 map bakes once in about 0.2 s.

  ![Slope, curvature, flow, wetness and distance-to-water maps of a valley, and the valley coloured from them](media/terrain-data-maps.jpg)

  *Terrain data maps: the stream bed, wet ground and banks come from the maps alone.*

## Ground

- **Ground material** (`TerrainMaterial3D`, `use_generator()`): rock, scree, soil, alluvium and sand from the generator's geology, height-blended, biplanar cliffs, erosion detail, wetness and snow.

  ![A generated range with its ground material under the cloud layer: rock, scree, soil, forest and a sand beach, raster vs path traced](media/terrain-ground.jpg)

  *Ground material: rock on crests and cliffs, grass and soil on the slopes, a sand beach at the coast, all from the generator's geology.*

- **Sky occlusion** (`TerrainGenerator3D.sky_occlusion`, `TerrainMaterial3D.sky_occlusion`): raster ground is lit only by the sky its hills leave open, from a horizon bake and the live sky, matching path tracing at dusk.

- **Far blend** (TerrainMaterial3D `far_blend_enabled`, `far_start`, `far_fade`, off by default): far ground takes the colour of the plant cover map.

- **Landscape3D** (`source`, `spacing`, `lod_error`): heightfield ground from a TerrainGenerator3D or a height image, as chunk meshes with crack-free LODs and one BLAS per chunk level, so nothing rebuilds while the camera moves. Heights stream in coarse first and chunk meshes build on worker threads (`stream_heights`); baked heights load 4 km at 1 m in under a second.

  ![A valley drawn by Landscape3D at 1 m spacing, raster vs path traced](media/landscape3d.jpg)

  *Landscape3D: a 1 m heightfield with sharp ridges and a narrow valley floor, the same in raster and path tracing.*

- **Landscape3D collision** (`collision_enabled`, `collision_radius`, `collision_target`): full-detail heightfield tiles follow the player and match the drawn triangles.

- **Footprints in ground** (`GroundSurface3D`, `BaseMaterial3D.ground_deform_enabled`): prints in sand, soil, mud and leaf litter; moisture from the weather map, walls slump to the angle of repose.

  ![Footprints and tracks in dry sand, damp sand, soil, mud and leaf litter, raster and path traced](media/ground-footprints.jpg)

  *Footprints: deepest in mud, shallow in dry sand where the walls slump.*

- **Terrain deformation** (`TerrainDeformer3D`, `TerrainBrush3D`): non-destructive height brushes (rivers, craters, plots) over Terrain3D or a height map; moving a brush restores the ground.
