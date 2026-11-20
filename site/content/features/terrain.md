---
title: "Terrain"
weight: 50
group: "World"
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

- **Terrain guides** (`TerrainGuide3D`: Raise, Lower, Bar, `apron_slope`): paths drawn under the generator raise ridges, cut valleys and place rock bars, read before erosion. With `guide_preview`, a 4x coarser run follows a guide in the editor as it is drawn.

  ![A generated valley from above, without and with a drawn ridge guide](media/terrain-guide.jpg)

  *Terrain guides: a drawn ridge across the valley floor; the rivers route round it and a pond forms upstream.*

- **Coast and beach guides** (`TerrainGuide3D` Coast: `invert`, `roughness`; Beach): draw a bay, a shore or an island, with a wandering shoreline that rivers run into, and set where its beaches go and how wide.

  ![A bay drawn with a coast guide, beaches round it and mountains behind, raster](media/terrain-coast-guide.jpg)

  *Coast guide: a drawn bay with beaches along its shore.*

- **Touch-ups** (`TerrainBrush3D` under `TerrainGenerator3D`): height brushes reshape the generated ground and lakes and rivers re-solve around them, so a raised dam backs up a lake and a dug hollow holds a pond.

  ![A generated valley from above, then with a dam and a pond brushed in](media/terrain-touchup.jpg)

  *Touch-ups: a dam across the valley backs up a lake, and a dug hollow holds a pond.*

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

- **Glacial erosion** (`glacial_enabled`, `glacial_ela`, `glacial_erosion`): ice ages alternate with the rivers and carve troughs, cirques and basins that become lakes. With rock quality varied (`glacial_rock_variation`, `glacial_rock_scale`; 0 is even rock) the ice digs its own basins and bars, so tarns and chains of lakes form with no markers. Glaciers from beyond the map edge flow in with their rivers and carve the main valley's trough and lakes (`glacial_inflow_balance`, m/yr, 0 = off). A coarser ice grid (`glacial_coarse_cell`) is about 3 times faster, with similar but not identical terrain. With `glacial_converged`, the ice settles to a steady state before it carves, conserving mass over cliffs.
- **Valley fill** (`outwash_age`): sediment left after the ice fills glacial troughs to flat floors, while cirque lakes stay open.
- **Moraines** (`moraine_age`, years, 0 = off): debris carried down the last glaciers builds ridges at their margins that dam lakes. Till draws as soil with stones through it, the stones kept above the grass.
- **Ice-worn rock** (`ice_worn_detail`): faces toward the old ice flow are smoothed, faces away are stepped, with striations.
- **Cliff ledges** (`cliff_ledge_spacing`, `cliff_ledge_strength`): rock beds of uneven thickness step faces steeper than 40 degrees into ledges.
- **Scree** (`scree_age`): rock walls shed aprons and cones at their feet, kept out of lakes and river floors. Scree draws as bare blocks; up close, each is an angular stone standing in a bed of gravel and fines.
- **Fallen blocks** (`get_boulders(rect, density)`): blocks shed by cliffs rest below them, the largest furthest out.

## Rivers, coasts and lakes

- **River deposition** (`deposition`): rivers drop sediment where they slow (Yuan et al. 2019), building flat alluvial valley floors; solved exactly per step along the river tree, at about half the earlier cost.

  ![A generated range around a wide flat valley floor that runs down to the beach, raster](media/terrain-deposition.jpg)

  *River deposition: the rivers fill the valley between the ridges with a flat alluvial floor.*

- **Meandering rivers** (`meander_age`, years): bends grow and migrate across flat valley floors; cut-off loops leave oxbows that silt up, and the river water follows the new course.

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

- **Coastal dunes** (TerrainGenerator3D `dunes_enabled`): beaches feed wind-blown sand inland; marram builds foredunes along the plant line, and blowouts and parabolic dunes open where the cover fails.
- **Sand seas** (TerrainGenerator3D `sand_sea_rect`, `sand_sea_kind`): a desert area fills with wind-built transverse, linear, star or barchan dunes that bank against the hills, thin out at their edges and stop at rivers; each has a long gentle windward slope and a steep slip face at the angle of repose.

  ![A desert sand sea at low sun, raster](media/terrain-sand-sea.jpg)

  *Sand seas: dunes with scalloped brinks and sharp slip faces, rippled sand on the windward slopes.*

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

- **Ground material** (`TerrainMaterial3D`, `use_generator()`): rock, scree, soil, alluvium and sand from the generator's geology, height-blended, biplanar cliffs, erosion detail, wetness and snow; layer textures are hex-tiled (Mikkelsen 2022), so no tiling repeat shows.

  ![A generated range with its ground material under the cloud layer: rock, scree, soil, forest and a sand beach, raster vs path traced](media/terrain-ground.jpg)

  *Ground material: rock on crests and cliffs, grass and soil on the slopes, a sand beach at the coast, all from the generator's geology.*

- **Far sun shadows** (`TerrainMaterial3D.far_shadow_start`): past the shadow maps' reach, ridges cast the low sun's shadow across distant terrain in raster.

- **Sky occlusion** (`TerrainGenerator3D.sky_occlusion`, `TerrainMaterial3D.sky_occlusion`): raster ground is lit only by the sky its hills leave open, from a horizon bake and the live sky, matching path tracing at dusk.

- **Rock detail** (`rock_detail_enabled`): steep rock shows bedding planes and breaks into irregular jointed blocks, and scree and till show loose stones that stand out of the gravel in parallax with their own shadows, also path traced; no textures and no stretching on any face.

- **Cliff relief** (Landscape3D `cliff_relief`, metres, 0.6 m by default): steep faces near the camera are refined without cracks and displaced into real bed ledges and joint blocks, path traced too. In raster, ledges shadow each other per pixel where the shadow map is too coarse (TerrainMaterial3D `cliff_self_shadow`).

  ![A cliff face in raking sun, smooth and with cliff relief, raster](media/terrain-cliff-relief.jpg)

  *Cliff relief: ledges and joint steps cast shadows across the face.*

- **Far blend** (TerrainMaterial3D `far_blend_enabled`, `far_start`, `far_fade`): far ground takes the colour of the plant cover map. With `far_start_from_vegetation` the blend starts where the streamed plants end, else at `far_start`.

- **Landscape3D** (`source`, `spacing`, `lod_error`): heightfield ground from a TerrainGenerator3D or a height image, as chunk meshes with crack-free LODs and one BLAS per chunk level, so nothing rebuilds while the camera moves. Heights stream in coarse first and chunk meshes build on worker threads (`stream_heights`); baked heights load 4 km at 1 m in under a second.

  ![A valley drawn by Landscape3D at 1 m spacing, raster vs path traced](media/landscape3d.jpg)

  *Landscape3D: a 1 m heightfield with sharp ridges and a narrow valley floor, the same in raster and path tracing.*

- **Landscape3D collision** (`collision_enabled`, `collision_radius`, `collision_target`): full-detail heightfield tiles follow the player and match the drawn triangles.

- **Footprints in ground** (`GroundSurface3D`, `BaseMaterial3D.ground_deform_enabled`): prints in sand, soil, mud and leaf litter; moisture from the weather map, walls slump to the angle of repose, a step on a dry dune slip face sets off a grain flow down it (`slip_avalanches`), and wind fills prints from the upwind side (`wind_fills_prints`). The prints simulate on the GPU (`gpu_simulation`). Within 8 m of the camera a 1.6 cm field (`near_field`) keeps print rims and lips crisp and matches the ground beyond its edge; boot prints show heel, toe and tread, and wet ground carries on past the deform patch.

  ![Footprints and tracks in dry sand, damp sand, soil, mud and leaf litter, raster and path traced](media/ground-footprints.jpg)

  *Footprints: deepest in mud, shallow in dry sand where the walls slump.*

  ![Boot prints in mud about 1.5 m away, without and with the near field, raster](media/ground-near-field.jpg)

  *Near field: faceted holes become boot soles with a rounded heel, a waist and a toe.*

- **Soft ground** (PhysXVehicle3D `soft_soil`; PhysXVehicleWheel3D `tire_pressure`; GroundSurface3D `ploughing`): vehicles sink into soft ground, print ruts and feel its grip and resistance (Bekker-Wong); tyres aired down flatten into a longer contact patch and sink less in sand and mud, but roll harder on firm ground; bodies sliding through sand slow as they plough it; `get_footing_at()` gives walkers effort, speed and slide; old ruts steer the wheels, which slide down a rut wall into it and shove the wall when steering out; wheels sliding sideways across a slope dig in.
- **Deep mud** (`GroundSurface3D.mud_layer`, `mud_map`, `data_map`): wet ground, painted or wherever water gathers, gets a soft clay layer that flows as a Bingham fluid (O'Brien and Julien), holds ruts when stiff and lets a car sink to its axles as slurry. It lies on a firm base of varying depth that wheels can dig down to for grip (`subsoil`), and logs and branches settled in it become corduroy that spreads wheel load and snaps when too thin (`mats`). Liquid mud a wheel shoves aside runs on as a wave and the rut closes behind it (`mud_waves`), and hidden stones and roots lift a sinking wheel and show as ruts deepen (`buried_obstacles`, `buried_density`); tyres press the mud out round a stone, and can knock an uncovered one loose (`buried_kick`).
- **Tyre clogging** (`PhysXVehicle3D.tyre_clogging`): sticky clay packs the tread until it grips like a slick, and spinning throws it off.
- **Treads and chains** (`PhysXVehicleWheel3D.tread`): road, all-terrain, mud or chains per wheel; mud tyres and chains bite deeper and clog less in sticky clay, and each prints its own pattern in the rut.
- **Belly drag** (`PhysXVehicle3D.belly_drag`): a car sunk past its clearance rides on its belly, is dragged by the soil and can high-centre.
- **Winch** (`PhysXWinch3D`: `rated_pull`, `line_speed`, `brake_hold`): a cable winch that slows under load, stalls at its rated pull and slips its brake; the cable sags and lies on the ground.
- **Worn tracks** (`GroundSurface3D.worn_tracks`, `wear_regrowth_days`): repeated wheel passes pack the soil firmer so it sheds rain, flatten the grass and then wear it bare, and it all regrows over weeks.
- **Stream fords** (`GroundSurface3D.stream_fords`): wheels crossing a stream scour its bed and its soft banks give way under them.
- **Mud on vehicles** (`PhysXVehicle3D.body_mud`; GroundSurface3D `mud_color`, `mud_color_map`): tyres, arches and bellies cake with the clay they drive through, in the ground's own clay colour, wet at first; it dries with the weather, cracks into curling plates and shakes off on bumps.
- **Mud spray** (`PhysXGroundGrains3D.mud_spray`): spinning wheels fling clods of wet clay that land back in the ground.
- **Mud splats** (`PhysXVehicle3D.add_mud_splat()`): clods that hit a vehicle stick as domed splats that drip, dry and wash off in water.
- **Mud on plants** (`GroundSurface3D.plant_mud`): thrown clay and muddy splashes spatter grass and plants, and rain washes it off.
- **Muddy water** (`GroundSurface3D.muddy_water`): tyres and boots churn clay into standing water, which turns milky brown, drifts with the flow and settles out over minutes; tyre spray flies brown and stains the wheel arches.
- **Tracked mud** (`GroundSurface3D.tracked_mud`): tyres, boots and other bodies leaving clay lay a film of mud on firm ground, printed with the tyre's chevron lugs or the boot's heel and forefoot, that fades as they clean and dries pale in place; thrown clods land as cakes of wet clay that dry where they lie.
- **Ruts that stay** (`GroundSurface3D.keep_ruts`, `ruts_time_scale`, `ruts_away_rain`, `ruts_regrowth_time`): prints, ruts, worn tracks and puddles are saved per tile as the ground streams out and are still there on return, in the same session or the next, aged by the time away: mud dries, rain softens ruts and puddles evaporate; grass grows back over them with the growing season, stalling in winter and drought; kept puddles keep the clay in their water and the cracks a dried pool left.
- **Flood silt** (`GroundSurface3D.flood_silt`): rivers over their banks and other floodwater soak the ground and leave their silt behind as fresh mud: a thin skin, slick while wet, that dries pale and cracks.
- **Mud slides** (`GroundSurface3D.mud_slides`): soaked mud runs off slopes it can no longer hold and fills the ruts below.
- **Mud on legs** (`GroundLegMud3D`, `boot_suction`): each leg is coated to the depth its foot sank and flecked by the stride; the mud dries over minutes and rinses off in standing water; sticky mud grips a lifting foot, slows the stride and can keep the boot.
- **Standing water** (`GroundSurface3D.standing_water`, `world_puddles`, `add_puddle()`): rain the ground cannot take and ground water gather level in ruts and prints, then soak in and dry, as real geometry in raster and path tracing; puddles form in hollows and wet flats and stay as dry hollows in dry weather; `add_puddle()` digs and fills an irregular clay puddle that stays level on slopes; below 0 C puddles freeze (`puddle_ice`), bearing a foot and breaking into plates under a wheel.
- **Frozen ground** (`GroundSurface3D.frozen_ground`, `needle_ice`): soil freezes from the top, holds ruts rigid and stands water; after the thaw it stays soft for days. Frosty nights lift wet bare soil on ice needles, and the thaw leaves loose crumbs that print deep.
- **Desiccation cracks** (`GroundSurface3D.desiccation_cracks`): soaked clay dries to a pale matte crust that cracks into plates; rain closes the cracks, and each wet-dry cycle makes the pattern more regular.
- **Thrown sand** (`PhysXGroundGrains3D`: `tread_depth`, `kick()`): spinning wheels and kicks throw sand that flies and lands back in the ground as fans, with the volume kept; a drifting car throws sand out of the turn, heaps a berm on the outside and scuffs a wide track, and the sand pushes back on the sliding tyre.
- **Wind ripples** (`GroundSurface3D.wind_ripples`; `ripple_extent`): the wind grows ripples about 11 cm apart on dry sand, as geometry in raster and path tracing near the camera and as surface detail beyond it that varies in warp and height with no repeating tile, fading in and out over fields some 16 m across with smooth sand between; past that, the ripples carry on as shading over all sand terrain, leaving steep slip faces bare (`far_ripples`); prints wipe them and they form again; the calm lee behind obstacles stays smooth (`lee_calm_ripples`), and pebbles and gravel stop sand blowing and rippling (`coarse_armour`).
- **Blowing sand** (`GroundStreamers3D`; `GroundSurface3D.obstacle_lee`): grains hop over dry sand as densely as the wind carries it, in streamers near the threshold wind, and drop in the lee of boulders, walls and parked vehicles.
- **Dust** (`GroundDust3D`, `GroundSurface3D.dust_emission`): the wind lifts dust off dry sand, and sliding tyres lift billowing clouds of it, carried on the wind until it settles; needs volumetric fog.
- **Sand glints** (`GroundSurface3D.sand_glints`): up close, dry sand shows the colours of its grains and the odd grain flashing in the sun; in low sun, sun-aligned grain facets glitter across dry sand.

  ![Dry sand 40 cm from the camera at full resolution, without and with sand glints, raster](media/sand-glints.jpg)

  *Sand glints: the blurred sand surface resolves into single grains up close.*

- **Wet sand** (`GroundSurface3D.water_coupling`; `water_sources`): water fills the pores of sand below it, wets a margin up to 15 cm above still water (`water_fringe`), topped in a dry climate by a pale, patchy salt crust (`salt_crusts`), and drains after, darkening and glossing it, drying back up the beach; damp sand holds steep walls; water faster than the Shields threshold rounds off prints as bed load, keeping the sand volume.

  ![A beach at the swash line with a row of footprints, before and after wet sand, raster](media/sand-wet.jpg)

  *Wet sand: the band the sea has soaked turns glossy and catches the sun.*

  ![Footprints running from dry sand into the wet band at the waterline, raster](media/sand-shore.jpg)

  *Wet sand: dry prints keep sharp walls, the drained band holds a sheen, and prints at the waterline wash out to shallow dips.*

- **Swash marks** (`GroundSurface3D.swash_marks`): each wave leaves a thin scalloped ridge of sand at its highest reach, and higher waves smooth older lines.
- **Runoff channels** (`GroundSurface3D.runoff_channels`): water seeping out of a beach after the tide, and rain the ground cannot take, cuts a braided net of rills from the top of the seepage face and lays the sand down at their foot on a low-tide terrace; on soil it cuts gullies along ruts and spreads a fan below, faster on softer ground.

- **Terrain deformation** (`TerrainDeformer3D`, `TerrainBrush3D`): non-destructive height brushes (rivers, craters, plots) over Terrain3D or a height map; moving a brush restores the ground.
