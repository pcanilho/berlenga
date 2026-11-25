# World links: how the systems feed each other

The living matrix of every world system pair in the fork: what each system reads from the others, through which
shared source, and what is still missing. Update it with every commit that adds or removes a link. The links bench
(`docs/bench/links`) checks the key ones in one scene.

Status: **linked** (both directions a real world has), **one-way** (only one direction exists where both should),
**missing**, **n/a** (no real interaction). Sources are the shared state a link reads; a system keeping a private
copy of shared state is listed under "Private copies" until it moves onto the source.

## Shared sources

| Source | Owner | What it carries |
| --- | --- | --- |
| `WeatherMap3D` air | weather | `get_air_temperature()`, `get_air_humidity()`, `get_air_dew_point()`: the day's mean (climate for the date + `season_anomaly`, or `temperature`) through the day's cycle; `get_air_temperature_at()`/`get_air_humidity_at()` at height (6.5 K/km) |
| `WeatherMap3D` cells | weather | wind (`get_wind_at`, `weather_wind_map`), rain, cloud cover, surface water film (`get_wetness_at`) and puddles (`get_puddle_at`) |
| `WeatherMap3D` contact wetness | weather | `add_contact_wetness*`, `weather_contact_map`: water any system splashes onto surfaces |
| `WeatherMap3D` shelter | weather | canopy height and closure, lee of obstacles (`get_shelter_at`) |
| `WeatherMap3D` cloud shadow | weather | `get_sun_visibility_at` |
| `WeatherMap3D` season | weather | `get_season()`, `get_season_at_height()`: phenology, snowpack, soil frost |
| `weather_climate` global | weather | x the season's precipitation (mm a year), w 1 while a WeatherMap3D sets it (zeroed when it leaves the tree); read by shaders where no `terrain_climate_map` reaches (24 Nov, 6c0fc3de) |
| `SkyClock` | weather | date, time, sun and moon |
| `GroundSurface3D` | ground | soil per point (`get_soil`), deformation field (`ground_deform_map`), standing water, puddle ice, frost |
| `SnowCover3D` | snow | `snow_cover` maps, depth, air at height (`get_air_temperature_at`) |
| `fire_state_map`, `WorldFire` | fire | burnt share and glow per cell; `heat_at` (W/m2), `burnt_at` (fires in play and WorldBurnScar3D `burnt_share`, whatever the age: readers weigh recovery by `burn_age_at`) |
| `WeatherLightning3D` | weather | `struck` signal and `get_ground_strikes_after()` |
| `WorldWeather.cloud_base_at` | weather (clouds) | cloud base, m above sea level: `WeatherMap3D.get_cloud_base_now`, LCL 125 m x (T - Td), at least ground + 30 m; `cloud_base_height` without weather (20 Nov) |
| Contact groups | each system | `world_contact` (every physics body and wheel) read by all, plus the old `grass_trample`, `tree_touch`, `ground_deform`, `snow_deform` |
| Contracts | world | `WorldContracts.get_weather()`, `get_water()`, `get_ground()`, `get_fire()`, `describe_body()`: see world-contracts.md |

## Pairs

### Weather (air, wind, rain, sun, clouds)

| Pair | Status | Source | Note |
| --- | --- | --- | --- |
| Season → air temperature | linked (18 Nov) | climate + SkyClock | `air_follows_climate` |
| Time of day → air temperature, humidity | linked (18 Nov) | SkyClock | `air_daily_cycle`, Parton and Logan 1981 |
| Sun → surface drying | linked (18 Nov) | sun, cloud shadow, canopy | `sun_drying`, Penman 1948 |
| Air → rain/snow share | linked | air | Jennings 2018 |
| Wind → clouds, storms | linked | cells | |
| Humidity, rain, storms, wind aloft → high cloud (cirrus) | linked (19 Nov) | `high_cloud_cover`, air, storms, `get_wind_aloft` | `get_high_cloud_cover_at` / `WorldWeather.high_cloud_cover_at`: dry air thins it, rain dialled in thickens it ahead of the rain (eases 4x faster), storms spread anvil shields 2.5 radii downwind; shades the ground only by its own optical depth in the cloud shadow bake (docs/ideas/clouds.md) |
| Humidity, rain, storms, wind aloft → mid cloud (altocumulus) | linked (20 Nov) | `mid_cloud_cover`, air, storms, `get_wind_aloft` | `get_mid_cloud_cover_at` / `get_mid_cloud_optical_depth_at` / `WorldWeather.mid_cloud_cover_at`: dry air thins it, rain dialled in thickens it into an altostratus sheet (eases 2x faster than the rain, after the cirrus), storms add a band round their anvils; shades the ground only by its own optical depth in the cloud shadow bake; never starts rain (docs/ideas/clouds.md) |
| Weather → haze | linked | air humidity, precipitation | |
| Clear calm nights → dew, hoarfrost | linked (18 Nov) | air, net radiation | `dew_and_frost`, Monteith 1957; `weather_frost_map` on terrain, grass, plants, rocks |
| Hill cloud (fog) → drip under trees | linked (19 Nov) | cloud extinction at the ground, canopy closure | `fog_drip`: LWC = 6.7 x extinction (r_e 10 um); catch 3.6 LWC u (0.003 + 0.015 closure) mm/h into Rutter's canopy, no drying in cloud; the fog's drip share wets the film and the soil (`fog_water_at`, `fog_drip_at`). Lovett 1984; Bruijnzeel et al. 2005 |
| Lightning ← storms, freezing level | linked | cells, air | |
| Lightning → tall things | linked (18 Nov) | ray + canopy map | rolling sphere |

### Terrain and ground (sand, mud)

| Pair | Status | Source | Note |
| --- | --- | --- | --- |
| Rain, puddles → ground wetness, mud | linked | cells | |
| Air → puddle ice, soil frost, thaw | linked | air, season | |
| Wind → sand transport, dust | linked | wind | |
| Obstacles → blown sand drops in their lee | linked (19 Nov) | WorldWeather `obstacle_lee_at` (0.15 in the cavity to 2 H, recovering over 6 H) | GroundSurface3D `obstacle_lee` scales u* by it with the plants' Raupach ratio: the flux falls and sand lands behind boulders, walls and parked cars (shadow dunes, Hesp 1981) |
| Season → regrowth of ruts | linked | season | |
| Snow → ground | linked | snow cover | |
| Water bodies → wet sand (swash) | linked | contact wetness | |
| Fire → scorched, ashy ground | linked | fire char decal | PhysXFireSpread3D's decal |
| Ground → dust (Flow) | linked | Flow | |
| Tyres, wind → dust off dry silt and sand | linked (20 Nov, mud a4571642) | `GroundSurface3D.add_dust(position, velocity, strength)` (strength = width x speed) | dry silt skins and dry sand answer; wind lifts dust off dry silt |
| Terrain → runoff, rivers | linked | terrain runoff (monthly) | storm runoff to rivers: rain work |
| Climate → bare soil between plants | linked (24 Nov, 6c0fc3de, f0f7455a, 6b0b311f, 3589ba9f) | `WorldContracts::bare_share_for_precipitation(mm)` (0.9 at 200 mm to 0 at 700 mm a year), `climate_bare_share_at(p)`, the patches `bare_patch(p, share, footprint)` / `climate_bare_patch_at(p)`; GLSL `climate_bare_shader_code()` (`wc_climate_bare`, `wc_bare_patch`); shaders read `terrain_climate_map` (g) else `weather_climate` | One field, so every reader leaves the same patches bare: TerrainMaterial3D `climate_bare_soil` (soil and alluvium bare by the share, Rietkerk et al. 2004; `contract_albedo_at` mixes the mean), GrassField3D and VegetationField3D `climate_bare` (no blades or plants in the bare patches), RockField3D rain splash pedestals. No climate: unchanged. Note: terrain-material.md, "Bare soil from the climate" |
| Slope, grazing → terracettes | linked (24 Nov, e8365c6c) | `WorldContracts::terracette_at(p, gradient)` (bands along the contour, `TERRACETTE_SPACING` 3.4 m along the slope, starting at about 25 degrees, full from 35 to 45, gone past about 51; a grazing model, Seleb, Chatterjee and Bhamla 2025); GLSL `terracette_shader_code()` (`wc_terracette`, needs `climate_bare_shader_code()` first) | GrassField3D `terracettes`: the sward follows the steps. Terrain relief to read the same bands (coming) |

### Snow

| Pair | Status | Source | Note |
| --- | --- | --- | --- |
| Air → snowfall, melt | linked (18 Nov) | air | live air with snow's own lapse rate |
| Sun, cloud shadow → melt | linked | sun, cloud shadow | |
| Wind → drifts, cornices | linked | wind | |
| Obstacles (rocks, walls, buildings, vehicles) → snow drifts in their lee | linked (19 Nov) | WorldWeather `obstacle_height_at` (WeatherMap3D shelter: the `weather_shelter` group, `get_wind_obstacles(rect)` on RockField3D boulders over 0.5 m and PhysXVehicle3D chassis) | SnowCover3D `drift_obstacles` counts them as terrain in Winstral's Sx (Winstral and Marks 2002): tops stripped, lee filled to about 15 H (Tabler 1991); a moved vehicle rebakes within 2 s |
| Rain → wet snow, crust | linked | cells | |
| Canopy → interception | linked | shelter | |
| Snow → plants (burial, pressing) | linked (18 Nov) | snow cover (snow map) | grass and plant shaders: load ρ g d against stem stiffness lays plants down, deep snow hides them |
| Fire → snow melt | linked (18 Nov) | WorldFire `heat_at` | SnowCover3D melts H dt / L_f under the fire, whatever the air |
| Smoke, burnt forest, dust → dirty snow | linked (19 Nov) | WorldWeather `smoke_at` (haze), WorldFire `burn_age_at`, WorldSnow `add_impurity` (GroundDust3D settling, PhysX blown ash 3862f22e, rockfall dust) | SnowCover3D keeps light-absorbing particles as mg/m2 black carbon equivalent: albedo drop 0.35 (1 - e^(-L/15)), fresh snow buries them, the melt brings them back up; shows grey and melts sooner (Warren and Wiscombe 1980; Painter et al. 2007; Gleason et al. 2013). Blowing ash and rockfall fines add theirs through `add_impurity` |
| Vehicles → snow ruts | linked (18 Nov) | world_contact | snow checks the wheel stamp |
| Bodies → snow prints | linked | `snow_deform`, `grass_trample` groups | |
| Stones → snow round them | linked (24 Nov, 3d025184) | new `WorldGround.stones_near(rect)` (RockField3D via `WorldCoverProvider.contract_stones_near`) | SnowSurface3D `set_stone_list`: 7 floats a stone (x, z, ground_y, top_y, half x, half z, yaw); main thread only |
| Snow depth → stones buried | linked (24 Nov, 3d025184) | snow cover depth | RockField3D `snow_burial`: stones sink under the snow, cells re-laid on a change of about 1 cm |

### Vegetation (grass, shrubs, crops) and trees

| Pair | Status | Source | Note |
| --- | --- | --- | --- |
| Wind → grass, shrubs, trees | linked | wind | |
| Season → leaf colour, fall, flowering | linked | season | |
| Rain, wetness → wet leaves | linked | cells | |
| Fire → burnt plants | linked | fire_state_map | |
| Plants → fire fuel | linked | VegetationField3D | |
| Canopy → shelter (wind, rain, snow) | linked | shelter | |
| Ground ruts, water → grass torn, pressed | linked | ground deform | |
| Mud spatter → grass | linked | ground | |
| Bodies → grass bend, trample | linked | `grass_trample` group | |
| Bodies → shrubs, crops, hedges, ferns | linked (18 Nov) | contact trample field | VegetationField3D keeps a field where no grass does |
| Bodies → branches | linked | `tree_touch` group | |
| Bodies → fallen leaves kicked up | linked (19 Nov) | world_contact bodies (position, speed) | LeafFall3D throws the lying leaves a moving body touches at 0.4-0.9 of its speed with a small lift; leaf litter spawns leaves across the swath; a still body moves none |
| Bodies out of water, mud, puddles → wet prints, drips | linked (19 Nov) | world_contact bodies, WorldWater.depth_at, WorldGround mud/standing water, puddles | WeatherMap3D `wet_tracks`: a load on the feet or tread (1 out of water, 0.8 wet mud, 0.6 puddles); each print a stride apart takes 15% (tyres 3% per 15 cm), so trails fade over a dozen strides; drips round the body e-fold over 40 s; all into the contact wetness square, dried by the weather's drying time (Penman: air, wind, sun) |
| Bodies out of mud → muddy prints | linked (19 Nov) | world_contact bodies, `WorldGround.mud_depth_at`, new `WorldGround.track_mud` (GroundSurface3D tracked mud) | WeatherMap3D `wet_tracks` keeps a mud film on each body: 1 mm on a sole, 2 mm in a tread out of mud; each print on firm ground hands a share to `track_mud` (same depleting load as the wet prints), so the clay trail thins over a dozen strides; water washes the film off. Bodies that print their own mud (vehicles, wheels, GroundLegMud3D) are skipped. `get_body_mud` |
| Wheels → worn-in tracks (packed soil, puddles, worn grass) | linked (19 Nov) | GroundSurface3D `worn_tracks` (mud, bab375e1): wheel passes per 25 cm cell | Passes pack the soil (stiffer Bekker k, less infiltration, so puddles stand in used tracks) and wear the grass flat then bare through the `ground_wear_map` / `ground_wear_rect` globals (GrassField3D reads them); fades over `wear_regrowth_days` |
| Ground colour, snow → light into gas from below | linked (21 Nov) | new `WorldGround.albedo_at` (TerrainMaterial3D layers, wet, under `WorldSnow.albedo_at`) | PhysXFlow3D `ground_from_world` (on): five points over the gas footprint every 0.5 s or 4 m moved, blended over `ground_albedo` x `ground_color` by the share known. Smoke over snow is lit blue-white from below, over red rock warm, darker over wet ground. Most of the light inside a low puff comes up off the ground (physx.md, 21 Nov) |
| Sea spray salt → salt bloom on stone | linked (rocks f54f88b6; terrain cliffs 22 Nov) | `weather_spray_map` G (WeatherMap3D sea spray salt), weather and ground wetness | Halite efflorescence where the film dries (Mustoe 1982; Goudie and Viles 1997; Huinink et al. 2004): rocks' own shader by cavity and shelter; TerrainMaterial3D `salt_bloom` (on) on rock and scree through `weather_salt_bloom_apply` (material.cpp): the load sets the share (at most 0.4) filled from 2.5 m, 0.9 m and 0.35 m patches, a 4 cm crystal grain, kept on steep faces, gone under any film of water. Same in the path tracer |
| Sea spray → damp sand holds sharper prints | linked (spray in the surface wetness, 19 Nov; checked 22 Nov) | `WorldWeather.surface_wetness_at` (WeatherMap3D takes the spray's film with the rain's) | GroundSurface3D's sand moisture: damp sand gains capillary cohesion (Namikas and Sherman 1995; Davidson-Arnott et al. 2008), steeper repose and stiffer kphi, so prints in the spray zone keep their walls; dry sand inland slumps. Links check: 1.30 kPa at the waterline under a 12 m/s onshore wind, 1.04 inland |
| Gravel and pebbles → no sand ripples, less blown sand | contract (19 Nov) | `WorldGround.coarse_cover_at` from RockField3D's cover maps | A gravel lag armours the sand under it (Bagnold 1941; Sharp 1963): sand scales its blown share and ripples by 1 - smoothstep(0.1, 0.35, pebble + gravel) (sand thread builds the read) |
| Water, snow → fallen leaves | linked (19 Nov) | WorldWater (height, flow), WorldSnow (depth, surface, lake ice), weather wind | LeafFall3D leaves land on the top surface: on water they float with the current plus 3% leeway in the wind (Breivik et al. 2011), on snow they lie on top, are buried by more snow and set down when it melts; lake ice holds them |
| Vehicles → grass, branches | linked (18 Nov) | world_contact | wheels and bodies join |
| Lying snow → plants | linked (18 Nov) | snow cover (snow map) | as above (GrassField3D, PlantGenerator) |
| Flowing water → reeds, grass | linked (18 Nov) | WorldWater flow and surface | the trample fields take the current's drag (Cauchy number, Luhar and Nepf 2011): grass and plants lean downstream, flat in a fast current |
| Springs → oasis plants and grass | linked (19 Nov) | DesertSprings (vegetation, from TerrainGenerator3D sand seas): `VegetationField3D.get_springs()`, `get_spring_wetness_at(pos)` (positions m, wetness 0-1, -1 off the sand seas) | readers VegetationField3D RULE_SPRING and GrassField3D; Valence adds the pools next; the spring finder should move to the terrain generator later. Anything cycling species meshes by index skips meshes with `tree_fire_years` meta |
| Nebkhas → shrubs | linked (19 Nov) | TerrainGenerator3D `get_nebkhas(rect)` (x, top y, z, height m), `get_nebkha_height(pos)` (mound sand alone); `sand_sea_mask` (Image, red 0..1) shapes the ergs | VegetationField3D places a shrub on each mound top; the mounds are sand for the sand thread, no deformation hook needed (terrain-gen cf579682) |
| Lightning → tree | linked (18 Nov) | lightning | attachment; fire ignition below |
| Lightning → struck tree variant | linked (20 Nov) | `WorldStand.strike_age_at` | WeatherLightning3D records canopy strikes; VegetationField3D swaps a tree within 3 m of a strike under 8 years old to its `tree_lightning_years` variant, from `get_strikes_in` copies (trees d898414194) |
| Avalanches → young stands on their paths | linked (22 Nov) | `WorldStand` `DISTURBANCE_AVALANCHE` (6) via `add_disturbances` (one batch; repeats replace their discs) | SnowAvalanche3D `stand_damage` (on): when the dense flow stops, every 16 m block whose peak dynamic pressure passed `stand_damage_pressure` (20 kPa; Bartelt and Stockli 2001, Feistl et al. 2015) restarts the stand (Johnson 1987; Bebi, Kulakowski and Rixen 2009). Readers through stand age: vegetation and tree succession (pioneers in the track); avalanche legacies (snapped stems, alder thickets tagged kind 6) are for the trees and vegetation threads. Snow bench check: 18 of 18 track points reset at 32 kPa, none past the run-out |
| Beaver dam fails → beaver meadow | linked (20 Nov) | `WorldStand` `DISTURBANCE_BEAVER` (4), age since the pond drained | WorldBeaverDam3D (world module): `breach()` or `state` Breached adds the disc over `pond_radius`, `years_drained` old; trees' STAND_BEAVER_POND follows it; PlantGenerator.make_beaver meadow reads it by RULE_DISTURBANCE (4,4) |
| Deer browsing → understorey, saplings | linked (20 Nov) | `WorldStand.browse_at`, `browse_reach_at` | browse discs (pressure, reach); vegetation RULE_BROWSE 31, trees' browsed variants (trees.md 5aw) |
| Game trails → ground detail, mud, snow, grass | linked (20 Nov) | `WorldStand.set_trails` (VegetationField3D.make_game_trails), `trail_at` | one network; readers wear their own surface by passes per year and tread share |
| Boar rooting → field layer, turned soil | linked (20 Nov) | `WorldStand` `DISTURBANCE_ROOTING` (5) discs, `rooting_age_at` | stand age untouched; vegetation and ground read the turned patches |
| Wet ground → mist | linked (18 Nov) | surface wetness, surface cooling | VegetationMist3D: wet ground mists more and sooner |

### Water (ocean, rivers, pools, falls)

| Pair | Status | Source | Note |
| --- | --- | --- | --- |
| Wind → waves | linked (18 Nov) | wind | `follow_weather` on (wind work) |
| Rain → ripples | linked | weather contract | live water's rain rings (`rain_from_weather`) |
| Rain, wind → rain on faces (glass, walls, roofs) | one-way (22 Nov) | rain, state and wind maps (raster globals; PT rt_params 39–43, 79, 89) | `weather_surface_rain()` (material.cpp) and `pt_weather_surface_rain()` (raygen) from one include, `rain_surface_catch()`: ISO 15927-3 driven rain plus the vertical share and drip, the film, the wind's push along the face. Readers: BaseMaterial3D wetness (walls run in streaks by the catch, lee walls drier; vehicles' `rain_slipstream`). `weather_surface_puddle()` / `pt_weather_surface_puddle()` (`rain_surface_puddle()`): the weather's pooled water (state map B, mm) on faces level enough to hold it, for `standing_water` to fill a relief. To come: glass drops and runners The shelter map's lee (rt_params 90–91) next |
| Rain, wind, storm cells → rain curtains in the haze | linked (22 Nov) | `set_precipitation_extinction(..., fall_speed)` (RainFall3D 7 m/s, SnowFall3D 1 m/s), `weather_rain_map` R | WeatherMap3D `rain_curtains` (on): 0.6 of falling precipitation's haze in a FogVolume round the camera, times the rain field over the camera's rain, times a lognormal (sigma 1.3, mean 1) of two octaves of noise in 16 m by 53 m shafts carried by the 10 m wind (Taylor), leaning downwind by wind over fall speed; 0.4 stays in the exponential fog. Froxels, so lamps light it and the path tracer's camera rays read it. Links check: mean 0.97 of the even haze, moving on the wind |
| Player → camera cut-out (walls, roofs, trees) | one-way (22 Nov) | globals `camera_cutout` (target, radius), `camera_cutout_floor`; PT rt_params 92–96 | Every BaseMaterial3D discards inside the cylinder from the camera to the target (along the view when orthographic), more than 0.5 m before it and above the floor, with a 4x4 Bayer dither; shadows keep it. The path tracer starts camera rays past the same span (bisection against a random threshold, the dither's average), passing only surfaces whose material has the cut-out (closest-hit probe); its hit shaders never cut (`camera_cutout_discard()` is false under IN_PATH_TRACER). Custom shaders add it with `get_camera_cutout_shader_code()` and the guarded call `if (!IN_SHADOW_PASS && camera_cutout.w > 0.0 && camera_cutout_discard(...)) { discard; }`, verbatim: a shader whose only discards are that call keeps the shared shadow material in raster and stays opaque (no any-hit) in the path tracer; any other discard makes it alpha-tested in both. Trees, rocks, plants and terrain use it. The game sets the globals from its player |
| Lights → shadow atlas by importance | linked (22 Nov) | RendererSceneCull positional shadow pass | Positional lights take atlas space by energy x screen coverage, most first (UE shadow priority, HDRP max shadows on screen); `max_shadowed_lights` (0 = as many as fit) releases the rest's space (`shadow_atlas_release_light`). Far lamps' moving casters already redraw every 2nd to 4th frame. Raster only: the path tracer traces every light |
| Cloud shadow → water | linked | cloud shadow | |
| Water → wet shore | linked | contact wetness | |
| Falls → mist → wet rock | linked | mist | |
| Falls → bodies in them (wet, pushed down) | linked (19 Nov) | WorldWater (PhysXWaterfalls3D) | the sheets and free columns answer the contract inside their water, above where they land: depth q/v, the water's velocity |
| Bodies with buoyancy → wakes | linked | buoyancy, wake nodes | |
| Bodies without buoyancy → ripples, splash | linked (18 Nov) | contact | world_contact bodies disturb sea, river |
| Wading → wet legs, clothes | linked (18 Nov) | WorldWater | ClothWetness (PhysX, d90fa990) |
| Cold air over water → steam fog | linked (18 Nov) | air, WorldWater, `get_water_temperature()` | VegetationMist3D cells over water |
| Rain, melt → river level | linked (18 Nov) | WeatherRunoff3D (SCS curve number, D8 routing) | rain thread: rivers add the discharge reaching them; snowmelt runs off too |
| Floodwater → ground soaked, silted | linked (19 Nov) | WorldWater `height_except` (every producer but the ground's own standing water), `flow_at`, `sediment_at` | GroundSurface3D soaks the ground any water covers (Green-Ampt) and still or slow water (under 0.2-0.5 m/s) lays its load as fresh mud, D = C w_s dt with w_s 3e-4 m/s (Pizzuto 1987; Asselman and Middelkoop 1995) |
| Sea → sea caves (surge, currents, blowholes) | linked (19 Nov) | CaveNetwork3D is a WorldWater producer inside each sea cave: the sea at the mouth (`height_except`, any producer; the still sea when none) run in as a long wave, lag sqrt(g h), Green's law growth (b^-1/2 h^-1/4), e-folding `surge_damping` 40 m; u = eta sqrt(g/h) | Blowholes: a crest that seals the passage traps the chamber's air, pushed up the shaft at rise x chamber/shaft area (oscillating water column; Falcao and Henriques 2016); `get_blowhole_spout`, signal `blowhole_spout`. Drawing (surface in the cave, spray) is Valence's: `get_sea_cave_water(cave)` |
| Sea → spray on the shore (wets, salts) | linked (19 Nov) | `WorldWater.breaking_at` (surf 0.78 d, whitecap cover x Hs) into WeatherMap3D `sea_spray` | Coarse spray L = u (1 + H) / 1 m/s, fine salt L = 50 s x u, swept along the wind on a 512 m square; production 1 mm/h (coarse) and 0.25 g/m2/h (fine) per m of breaking, x ((u - 2) / 8)^2. Rising ground catches the coarse spray (cliff faces). Wets the film, contact map and soil; salt 35 g/kg, washed off by rain (e-folding 5 mm), 2 weeks otherwise; `weather_spray_map` global (salt burn on grass, shrubs and tree leaves, leaf by leaf, from 0.15 to 0.85 of the salt index). `sea_spray_at`, `salt_at`. Andreas 1998; de Leeuw et al. 2000; Gustafsson and Franzen 1996; Boyce 1954 |
| Cold → lake ice | linked | air, snow | SnowCover3D lake ice, Stefan's law under the snow (Leppaeranta 1993) |
| Sea salt → shore snow and lake ice melt | linked (19 Nov) | `WorldWeather.salt_at` into SnowCover3D `salt_melt` | The pack keeps the salt it is given (new salt only); its brine lowers the melting point 0.0593 K per g/kg (to the 21.1 K eutectic), so it melts snow until diluted to the air's freezing point, and draining meltwater takes its share of the salt. Bare lake ice melts by the surface salt. `get_salt_depression_at`. Road-salt melting (Ketcham et al. 1996) |
| Bodies → lake ice (break through) | linked (19 Nov) | world_contact bodies (mass), SnowCover3D lake ice | a load past Gold's P = A h^2 (A 10, white ice half, thawing ice half) breaks a hole the body's size; it stops bearing at once and refreezes by Stefan's law |
| Fire → steam on water | linked (18 Nov) | WorldFire, WorldWater | water landing on fire and fire at the water's edge (PhysX, ebb5f254) |

### Fire, gas and lightning

| Pair | Status | Source | Note |
| --- | --- | --- | --- |
| Wind → fire spread, smoke | linked | wind | |
| Air, rain → fuel moisture | linked | air, cells | |
| Wet ground → fuel moisture | linked (18 Nov) | weather contract | fuel on wet or dewy ground holds the film's water |
| Lightning → fire | linked (18 Nov) | ground strikes | continuing current, fuel moisture |
| Fire → cloth | linked | cloth burn | |
| Fire → firebrands → spot fires | linked | wind | |
| Fire → drying | linked (18 Nov) | WorldFire | the weather boils the surface film off under a fire |
| Fire → snow melt | linked (18 Nov) | WorldFire `heat_at` | as above (snow) |
| Fire → heat shimmer | linked (18 Nov) | Flow temperature, weather air | `PhysXFlow3D.heat_shimmer`: raster screen pass and bent PT camera rays (heat-shimmer.md) |
| Fire indraft → grass, plants, cloth, hair, leaves, ash, smoke, dust | linked (18 Nov) | WorldFire `wind_at` (fire_wind_map in shaders) | PhysX e141399d (grass, cloth); plants, hair, LeafFall3D, firebrands, other Flows and rock dust add it to their wind, fading over the first few metres for airborne things |
| Fire → trees (charred stems snap in later storms) | linked (18 Nov) | WorldFire `stem_char_at` | PhysX f7510da2: TreeWind3D weakens the stem by its char depth |
| Fire → trees (crowns killed stand as snags, which weaken with the years) | linked (19 Nov) | WorldFire `crown_scorch_at`, `burn_age_at` | PhysX c15a10b7: TreeWind3D kills trees scorched to the crown; snags weaken by burn age |
| Fire, windthrow, beetles → stand age (forests that age) | contract (20 Nov) | `WorldStand.stand_age_at`, `disturbance_at` (kept by the contract: WorldFire burn age unless a surface fire, events from trees) | The youngest stand-replacing disturbance; change hook like the burn changes. Vegetation and trees re-place by it (succession stages from each species' age ranges) |
| Snow in crowns ↔ trees (droop, caps, breakage; a jolted crown drops its snow) | contract (19 Nov) | `WorldSnow.canopy_load_at`, `canopy_capacity_at`, `shake_canopy` (SnowCover3D, baf901d4) | Hedstrom and Pomeroy interception load and capacity per m2 of ground; a shake moves the share of the held snow in the radius onto the pack below at the next update as 150 kg/m3 snow, mass and water kept (Pomeroy and Gray 1995). Trees read the load for branch droop, crown caps and breakage (TreeWind3D) |
| Terrain wind (lee of a ridge) → fire whirls | linked (18 Nov) | WorldWeather `terrain_lee_at` (Winstral Sx) | PhysX c3c529f4: lee-slope whirls form where the wind has separated behind a crest, driven by the ridge's free wind |
| Fire → ash on burnt grass, litter | linked (18 Nov) | fire_char_map (`VegetationFire::ASH_CODE`) | PhysX c3c529f4: white ash where it burnt out, black char where not, thinned by wind and darkened by rain |
| Fire → fire clouds (pyrocumulus over a big fire) | linked (19 Nov) | WorldFire `plumes()` (base, convective W) | PhysX ebdad64b: WeatherMap3D builds the cloud from the plumes |
| Fire → light in the clouds | linked (20 Nov) | WorldFire `plumes()` (base, heat release) | WeatherMap3D registers a cloud light per plume (flame colour, a third of the heat as light); lights the cloud bases at night; listed in `get_cloud_lights()` (docs/ideas/clouds.md round 4) |
| Lightning → cloud lights list | linked (20 Nov) | WeatherLightning3D flashes | Listed in `WeatherMap3D.get_cloud_lights()` for readers; the sky keeps its own full-resolution flash glow |
| Cloud base → lightning strike height | linked (20 Nov) | `cloud_base_at` | WeatherLightning3D starts the leader at the local base |
| Cloud base → hill fog | linked (20 Nov) | `cloud_base_at`, WorldGround `height_at` | VegetationMist3D: ground within 40 m under the base (cover over 0.3) stands in cloud; wind, overcast and sun do not clear it. The base never drops under ground + 30 m from the day's air alone; an authored base below a ridge stays (fog drip) |
| Terrain climate → upslope rain, rain shadow; air at height → snow line | linked (22 Nov) | `WorldContracts.climate_precipitation_factor_at` (TerrainDataMap3D, a `WorldClimateProvider`: its baked precipitation over the recipe's, Smith and Barstad 2004); `WorldWeather.orographic_factor_at` | WeatherMap3D `orographic_rain` (on): each cell's rain times the climate's factor (refreshed every 10 s and as the square moves; heights read once per cell), and rain or snow by the air at the cell's own ground height (Jennings 2018 with the lapse rate), so the summit snows while the foot rains. SnowCover3D, which reads the climate itself, divides out the weather's factor so it is not counted twice. Links check: a 600 m ridge, 1.05 windward and 1.34 in the lee (spillover over a 1 km ridge), rain share 0.80 at the foot and 0.08 on top |
| Any system → cloud density | linked (20 Nov) | `add_cloud_effector` callers | `WorldWeather.cloud_effector_at`; shadow bake and sky both see it |
| Terrain, wind aloft, temperature, humidity, mid/high layers → wave clouds (lenticulars) | linked (20 Nov) | terrain source, `get_wind_aloft`, air, `mid/high_cloud_cover` | `WeatherMap3D.get_wave_cloud_density_at` / `WorldWeather.wave_cloud_density_at`; shadow bake (docs/ideas/clouds.md round 5) |
| Time of day, wind, humidity, rain → low cloud type (stratus, stratocumulus, cumulus) | linked (20 Nov) | clock, `cur_*` air | `WeatherMap3D.cloud_regime_from_weather`, `get_convective_heating` (docs/ideas/clouds.md round 6) |
| Wind shear aloft, air temperature → cirrocumulus; high cover (humidity, coming rain) → cirrostratus veil | linked (20 Nov) | `get_wind_aloft`, air | `WeatherMap3D.get_cirrocumulus_share` (docs/ideas/clouds.md round 7) |
| Storm rain, outflow, steering wind → rain shafts and shelf cloud | linked (20 Nov) | `add_storm` storms | sky rain shafts; `get_cloud_base_at` / `WorldWeather.cloud_base_at` lowered by the shelf (docs/ideas/clouds.md round 8) |
| Clear calm nights → cold air pools in hollows (frost, valley fog) | linked (22 Nov) | `WorldWeather.air_temperature_at` / `air_humidity_at` (WeatherMap3D `cold_pools`, `get_cold_pool_at`) | Night x clear x calm forcing builds a deficit up to `cold_pool_strength` (8 C) over about 2 h where the cell lies below the mean height within 600 m (topographic position, Lundquist et al. 2008; Whiteman 2000), faded to nothing at the pool's top; breaks up after sunrise or in a wind. Dew, hoarfrost and evaporation take the pooled air per cell; mist, snow and every air reader see it through the contract. Links check: a 200 m valley, 6.9 C pool, -0.9 C on the floor under 5.3 C up the slope, humidity 1.00 vs 0.85, frost only on the floor |
| Supercooled cloud, wind → rime on windward twigs, rocks and cliffs | linked (22 Nov) | `WorldWeather.rime_at` (Vector3: mm, into the wind); `weather_frost_map` B, A (mm along x, z) | WeatherMap3D `rime` (on): per cell, the fog water at canopy height below 0 C grows 0.3 x LWC x wind at 300 kg/m3 (Makkonen 2000, ISO 12494) into the wind; sheds in a thaw (0.7/h per K to 3 K), sun and rain. `weather_rime_apply` (material.cpp): the faces turned into the wind first, wrapping round as it thickens past 3 cm, feathers drawn out along the wind, thin rime as separate feathers; plants, plant impostors, rocks and steep terrain. SnowRime3D still grows the near-field feather geometry. Links check: 66 mm in 6 h of 0.38 g/m3 at 10 m/s and -6 C, gone after a 6 h thaw. Raster and PT match (project files fork/world-links/rime-raster-pt.png) |
| Clear night surface cooling → needle ice, frozen ground, puddle ice | linked (22 Nov) | `WorldWeather.surface_temperature_at` (air + WeatherMap3D's radiative cooling) | GroundSurface3D's frost (needle ice, Stefan front) and puddle ice read the surface temperature, not the air: needle ice is a radiation frost, clear calm nights with the air a degree or two above 0 C and the bare surface some degrees below (Outcalt 1971; Lawler 1988). With no sky clock or dew_and_frost off the cooling is 0 and the surface is the air. Ground check (--test=frost): air +1.5 C, 2.4 cm of needles under a clear sky (surface -4.6 C), none overcast (surface 1.6 C) |
| Clear calm night, slope → drainage (katabatic) wind | linked (22 Nov) | `WorldWeather.wind_at` (WeatherMap3D `drainage_winds`, `get_drainage_wind_at`); the wind map the shaders read | Prandtl's slope flow per cell: u(z) = C exp(-z/l) sin(z/l), peak 0.32 C at (pi/4) l, C = dT sqrt(g / (T gamma)) with dT the cell's radiative cooling, gamma 0.03 K/m, l = sqrt(2K / (N sin a)), K 0.2 m2/s (Prandtl 1942; Mahrt 1982; Horst and Doran 1986). Along the fall line of the weather's cell heights; none under 2 degrees; builds over 0.5 h with night x clear x calm. Every wind reader sees it: smoke, mist, snow drift, grass and trees, fire spread, firebrands. Links check: 2.1 m/s down a 14 degree slope at 6 m, none at 80 m or on the floor, none overcast or by day |
| Valley fog → fog water at the canopy (drip, rime) | linked (22 Nov) | `WorldWeather.fog_water_at` (WeatherMap3D fog drip pass) | The fog sea's extinction under its top (`valley_fog_density`, as its FogVolume draws it) joins the sky clouds' at each cell's canopy height, 6.7 g/m3 per 1/m (Stephens 1978): 0.13 g/m3 at the default 0.02/m, a radiation fog's 0.05 to 0.3 (Gultepe et al. 2007). Fog drip wets the ground under trees in it and, below 0 C, rime grows on them (the hoarfrost forests of inversion valleys) through the existing readers. Links check: 0.13 g/m3 on the valley floor in the fog, 0 on the shoulder |
| Cold pool → smoke held near the ground | linked (22 Nov) | `WorldWeather.smoke_concentration_at` (g/m3; WeatherMap3D `smoke_in_pools`, `get_smoke_concentration_at`) | Smoke let out in a pooled cell goes, by the pool's share (its deficit over 0.5 to 3 C, times night), into a held part of the column that stays out of the layer's wind aloft, drains down the slopes with the drainage wind (0.6 of the jet's peak) and is read through the pool's depth instead of 2 km; the morning's mixing layer (sun sine 0.1 to 0.4) releases it into the layer over half an hour (Lavdas 1996; Achtemeier 2005; Whiteman 2000). Readers: the haze where the camera stands, dirty snow (SnowCover3D's black carbon deposition now reads the concentration). Links check: 1.57 g/m3 at 3 m under the pool for a 50 mg/m3 2 km mix, nothing at 400 m; under 1/100 of it by 14:00 |
| Land and water temperature → sea and lake breezes | linked (22 Nov) | `WorldWeather.wind_at` (WeatherMap3D `sea_breezes`, `get_sea_breeze_at`) | The lake-effect square's water mask (2 km cells, 128 km) blurred over about 25 km gives each place its way onshore and the breeze's reach; the speed is the gravity current's, 0.4 sqrt(g dT 500 m / T) with dT the air over the water's temperature (Simpson 1994; Steyn 2003; Miller et al. 2003), half and offshore at night, faded by an ambient wind from 4 to 10 m/s, relaxed over an hour. Added to the cell's mean wind, so every wind reader and the shaders see it. Links check: air 24 C over a 17 C sea, 4.2 m/s onshore 3 km in, 2.6 m/s 15 km in, none 60 km in; a land breeze at night; gone in a 12 m/s wind |
| Sun on slopes → upslope winds and ridge cumulus | linked (22 Nov) | `WorldWeather.wind_at`, the clouds (WeatherMap3D `upslope_winds`, `get_ridge_cloud_count`) | The drainage wind's day side: Prandtl's slope flow driven by a quarter of each cell's surface heating over the air (capped at 5 K), K = 2 m2/s so the layer is tens of metres deep, weakened under cloud and in a wind, relaxed over an hour (Prandtl 1942; Whiteman 2000). Crests 60 m or more above the ground within the cold-pool radius (the same box mean) gather a fair-weather cumulus at the condensation level, 125 m per K of dew-point spread, when that is under about 2 km (Banta 1990); held over the crest, not blown off, as storms with the ridge flag (cover, no rain or lightning). Links check: 1.6 m/s upslope at 15 m by 13:00; one cumulus over a 250 m hill, cover 0.59 on top, none 1.8 km off; none in dry air |
| Cold pools → late frost on new leaves | linked (22 Nov) | `WorldWeather.season_at` keys `leaf_frost`, `leaf_frost_forcing`, `leaf_frost_days` (WeatherMap3D `late_frost`, `get_leaf_frost_at`) → TreeGenerator leaf season `frost_browned` | Per cell the worst spring air frost (the free air, lapse and the cold pool's deficit), severity 0 at -1.5 C to 1 at -4.5 C, with the season's forcing at the cell's height that night. A species browns by its leaf-out then: none in bud, full while unfolding, 0.4 once grown; the share fades from 21 to 42 days as the second flush grows (Augspurger 2009; Vitasse, Lenz and Koerner 2014). Browned leaves are a separate hash from the autumn order; the shrub leaf paths draw their random only when frosted, so unfrosted trees are unchanged. Links check: 5 April, -4.9 C pooled on a valley floor: birch (out 0.2) all browned there, none on the shoulder at 0.7 C; ash still in bud unharmed; gone at 45 days |
| Wind over ranges → foehn (lee warming, drying, cloud gap) | linked (22 Nov) | `WorldWeather.air_temperature_at`, `air_humidity_at`, the clouds (WeatherMap3D `foehn_winds`, `get_foehn_at`) | Per cell the highest ground up to 20 km upwind (the crest) and the lowest beyond it (the foot). Dry drawdown warms by (9.8 - 6.5) K/km of descent. Moist: a parcel from the foot, dry to its condensation level, then the moist adiabat at 850 hPa, all rained out, then down dry; the dew point is the crest's. Weighted by the rise past the condensation level and the air's humidity (0.7 to 0.9). Faded by the crest wind (1.5 x the 10 m wind, 5 to 12 m/s), the descent (150 to 500 m) and 30 km past the crest; relaxed over an hour. Cloud gap 0.8 x the moist share. Readers through the contract: snow melt, fire spread, ground. Links check: 1500 m ridge, 10 m/s, 95 %: lee 14.8 C at 54 % vs windward 9.7 C at 96 %, cover 0.31 vs 0.80; dry 3.7 K; 3 m/s none |
| Valley heating and cooling → along-valley winds | linked (22 Nov) | `WorldWeather.wind_at` (WeatherMap3D `valley_winds`, `get_valley_wind_at`) | Per cell the depth below the 2 km box mean of the heights (about 0.6 of the true depth) and the up-valley direction from that mean's gradient (the sides cancel, the floor's rise stays; none under 0.2 % grade). Forcing: the slope winds' own (sun, cloud, calm 3 to 8 m/s), the night's two thirds of the day's, relaxed over two hours (half an hour in a wind). Speed 4.5 m/s x forcing x smoothstep(30, 180 m) of depth, added to the cell's mean wind (Whiteman 2000 ch. 10; Zardi and Whiteman 2013). Links check: a 250 m valley rising 2 %: 4.1 m/s up it at 13:00, 2.5 m/s down it at 02:00, none on the plain, none in a 12 m/s wind |
| Flow emitters' smoke → regional smoke | linked (22 Nov) | `WorldFire.smoke_sources` (PhysXFlowEmitter3D `regional_smoke`, `get_regional_smoke_rate`) → WeatherMap3D smoke cells | One provider for all hot emitters (temperature over 0.05, not colliders), on the fire contract while any is in the tree, no heat. Rate = smoke x 3 mg/m3 x area x max(exit speed, 0.5 m/s): 4.7 mg/s for a 0.5 m campfire at 2 m/s (1 to 2 kg of wood an hour at about 10 g/kg PM2.5; Andreae 2019). Emitters made by systems that report their own smoke or none (fire spread, smoulder, building fire, steam, dust, mist, ash, haboob, dust devils, river mist) set it off; burnables keep it (their smoulder was unreported). The weather pools it at night (smoke in pools). Links check: one campfire on a valley floor at night, 1.5 ug/m3 at 3 m after 3 h, none at 400 m; a cold emitter none; off none; the provider leaves with the last emitter |
| Flood stage → high-water marks (wrack, washed grass) | linked (22 Nov) | `WorldWater.high_water_at` (PhysXRiver3D per section, PhysXWaterSurface3D lakes with `level_from_flows`; the sea keeps none) | The median annual flood's stage or a higher storm's in the last year; the highest producer answers. Readers: vegetation wrack lines, GrassField3D flood wash. Links check: a 5 m3/s river 2 m over its surface, a weir-fed lake 1 m over its level, the sea none; bank grass under the river's mark washed, none above |
| Drawn ground relief → placement | contract (22 Nov) | `WorldGround.relief_at`; `terrain_relief_map` / `terrain_relief_rect` | Landscape3D (terrain 57b253b5) answers from its cliff and bank relief: x the height over the heightfield at a fixed xz, y the share. Readers asked to adopt it 22 Nov: vegetation, rocks, ground detail, mud. Links check: a 45 degree bank answers up to 0.25 m, share 0.47; the flat none |
| Drawn relief changes → props re-placed | linked (ground detail 310a14ee, 22 Nov) | `WorldGround.relief_changed(area)` signal | Landscape3D raises it through `notify_relief_changed(area)` on chunk height stream-in, a relief setting change and register; RockField3D's detail cells rebuild in that area |
| Cliff shelves → rockfall debris | one-way (terrain f1f38845, 22 Nov) | `terrain_cliff_map` / `terrain_cliff_rect` (R8, 512 m round the camera; terrain_material register_types) | TerrainMaterial3D turns cliff shelves to rockfall debris. Readers to come: rocks (scree props on the shelves), snow and rain (shelves hold water and snow) |
| Saturated cold pool → valley fog sea | linked (22 Nov) | `WorldWeather.valley_fog_top_at` (WeatherMap3D `valley_fog`, `get_valley_fog_top_at`, `get_valley_fog_depth_at`) | Per cell, the fog top is the highest level in the cold pool where air temperature is at or below the dew point; the basin's level is taken from its lowest fogged cell within 600 m, so the top lies flat across the valley and thins out on the slopes (Whiteman 2000; Gultepe et al. 2007). Forms over about 1 h, lifts in 0.5 h / (1 + wind/3) after sunrise. Drawn as one box FogVolume over the map, density `valley_fog_density` (0.02/m) under the top, white droplets plus a multiple-scattering emission so the sea reads white, not grey; PT reads the same froxels, raster and PT match (project files fork/world-links/valley-fog-raster-pt.png). Links check: 28 m deep in a 200 m valley, top 29 m, 30 m 150 m up the valley, clear at 300 m; a 6 m/s wind lifts it with the pool |
| Air temperature, dew point → cloud base | linked (20 Nov) | `get_air_dew_point` | `WorldWeather.cloud_base_at`; readers mist, lightning, snow line, orographic rain |
| Mountain waves → wind aloft | linked (20 Nov) | WeatherMountainWaves | `get_wind_at` vertical part above the boundary layer; `WorldWeather.mountain_wave_velocity_at` alone; ground wind unchanged |
| Fire → smoke haze downwind | linked (19 Nov) | WorldFire `smoke_sources()` (base, kg/s PM2.5) | PhysX eabdcd98: WeatherMap3D carries the smoke into its haze |
| Rain, snow → smoke and dust washed out of the air | linked (19 Nov) | WorldWeather `washout_at` (`rainfall_at`, `snowfall_at`) | below-cloud scavenging Lambda = a R^b: the map's smoke haze thins over hours of rain; Flow gas adds it to its smoke damping (coarse when the gas is heavy dust) |
| Fire age → trees cast dead needles | linked (19 Nov) | shader global `fire_age_map` | PhysX ebdad64b |
| Fire → regrowth (burnt ground greens again) | linked (19 Nov) | WorldFire `burn_age_at` (fires in play, WorldBurnScar3D) | VegetationField3D: RULE_BURN_AGE, PlantSpecies.burn_recovery (38c29f47); GrassField3D green-up (30b7085a, a7c6a8fb); both refresh through the burn change log (VegetationField3D 0197e5b2, vegetation bench `--burn=live`) |
| Fire → trees regrow (resprouts, seedlings on the ash) | linked (19 Nov) | WorldFire `burn_age_at` at each tree's foot (VegetationField3D's burn grid) | Trees supply variants built at TreeGenerator.fire_years with mesh meta `tree_fire_years` (years) and `tree_fire_band` (Vector2 years from, to: bands 0-1, 1-3, 3-6, 6-10, 10-20); VegetationField3D places the variant whose band holds the age, unburnt variants off burns and from 20 years; vegetation --test checks it. Also the regrowth flush (cadd0e98) |
| Burnt ground → runoff (water-repellent soil, ash washed off) | linked (19 Nov) | WorldFire `water_repellency_at`, WorldGround `runoff_at` | PhysX 837fea90 |
| Ash, silt runoff → stream colour | linked (19 Nov) | WorldGround `add_runoff_load` → runoff (WeatherRunoff3D) → WorldWater `add_sediment`, `sediment_at` | PhysX puts washed ash on the runoff (5f14b542); WeatherRunoff3D routes it into open water (Valence #393); PhysXRiver3D and PhysXWaterSurface3D carry, settle and shade it (Valence eab1d857) |
| Rain on burnt slopes → debris flow | linked (19 Nov) | WeatherRunoff3D `debris_flow` signal (outlet, volume, path) | Valence fires it, mud's GroundDebrisFlow3D runs the mud body, PhysXDebrisSurge3D carries the bouldery front (Rickenmann 1999 speed) |
| Ground → loose stones; debris flows findable | linked (20 Nov, mud 20f319f4) | `GroundSurface3D.add_loose_stone(position, radius, velocity)`; group `ground_debris_flows` | any system can drop a loose stone onto the ground; readers find running GroundDebrisFlow3D nodes by group |
| Weather wind → free-standing Flow smoke | linked (18 Nov) | wind | `PhysXFlow3D.weather_wind` (wind work) |

### Cloth, hair, bodies

| Pair | Status | Source | Note |
| --- | --- | --- | --- |
| Wind → cloth, hair | linked | wind | |
| Rain → wet cloth, hair | linked | cells, shelter | |
| Air → drying | linked | air | |
| Flow gas → cloth | linked | Flow velocity | |
| Bodies → ground prints | linked | `ground_deform` group | |
| Mud → legs | linked | GroundLegMud3D | |
| Ground, snow, mud surface → rigged feet; feet → prints | linked (22 Nov) | `WorldGround.known_height_at` (the deformed surface), `WorldSnow.depth_at`, a short physics ray otherwise; `WorldFootfalls` | FootPlacementModifier3D stands each foot on the deformed ground plus the snow on it, tilts it to the slope and lowers the pelvis to the lower foot; each plant is published as a footfall, and with meta `world_contact_feet` the deformers' WorldWalkerSteps use those instead of guessed strides, so prints land where the animated feet did |

### Vehicles

| Pair | Status | Source | Note |
| --- | --- | --- | --- |
| Soft soil → sinkage, grip, ruts | linked | ground terramechanics | |
| Puddles → spray, washed tread | linked | ground | |
| Wet, flooded, icy firm ground → grip, aquaplaning | linked (18 Nov) | weather contract, ground ice | |
| Snow, lake ice → tyre grip | linked (18 Nov; snow's own friction 21 Nov) | WorldSnow (depth, density, wetness, crust, lake ice; `grip_at` from SnowCover3D traffic packing and glaze) | the snow's own friction where it keeps one (loose 0.3, packed road 0.25, polished to ice 0.05-0.15, which chains bite as ice), else packed snow 0.2-0.35, slush lower, crust near ice; tread_firm_grip scales by tread (chains, lugs) |
| Vehicles → water wakes | linked | buoyancy | |
| Vehicles → grass, branches, snow | linked (18 Nov) | world_contact | snow ruts: snow checks the wheel stamp |
| Vehicles → Flow (bodies as solids) | linked | Flow | |

## Private copies of shared state

- `VegetationField3D` placement skew reads the wind setting: placement time only, harmless.

## Techniques for the planned links

- Dew and hoarfrost: radiative cooling of exposed surfaces under a clear sky below the dew point (Monteith 1957;
  Garratt and Segal 1988), rime below 0 C; sheltered and clouded spots stay dry.
- Mist over wet ground: radiation fog strength from the surface's moisture (wet film, puddles); steam fog where air
  much colder than water lies over it (sea smoke).
- Fire on the ground: the fire grid's heat dries the surface film, melts snow (energy balance), steams where it
  meets water; fuel moisture starts from the surface film.
- Heat shimmer: built 18 Nov, see heat-shimmer.md (Gladstone-Dale index, ray deflection integrated through Flow's
  temperature plus sub-grid plume turbulence).
- One contact group: every system reads `world_contact` as well as its own group; vehicles, characters and PhysX
  bodies join it.

## Next round candidates (19 Nov, card to Paulo)

Every pair above is linked or in work. Three visible gaps remain; each reads a contract that already exists.

1. Bodies break through lake ice (built 19 Nov). Source: `WorldSnow.lake_ice_at` (thickness, white share) and `world_contact` bodies
   (mass from the physics body). Bearing capacity of floating ice: Gold (1971), P = A h^2 (P in kg, h in cm), A about
   3.5 for routine safe loads and about 17.5 at failure; white (snow) ice carries about half of clear ice, and
   thawing ice less again (USACE EM 1110-2-1612; Masterson 2009 review). A body over A_fail h^2 breaks a hole
   about its own size plus the radial cracks' reach (characteristic length l = (E h^3 / 12 (1 - nu^2) rho_w g)^(1/4),
   Wyman 1950), which the lake ice keeps open till it refreezes (Stefan's law already in SnowCover3D); the body then
   meets the water through WorldWater (buoyancy, wet clothes).
2. Bodies kick up fallen leaves (built 19 Nov). Source: `world_contact` bodies (position, velocity) into LeafFall3D's lying leaves.
   A foot or wheel moving through litter throws the leaves it touches at about the contact's speed plus a small
   lift, and its wake lifts the ones a little beyond: the air behind a walker moves at about 0.3-0.5 of walking
   speed close to the ground (Edge et al. 2005, human wake measurements), well under the gust lift threshold, so only
   leaves the body touches, plus a wheel's wake at speed, should fly.
3. Rain washes smoke and dust out of the air (below-cloud scavenging; built 19 Nov). Source: `WorldWeather.precipitation_at` into
   PhysXFlow3D smoke and dust. Scavenging coefficient Lambda = a R^b: for PM2.5-sized smoke about 1e-5 to 1e-4 1/s at
   1-10 mm/h (Laakso et al. 2003; Wang et al. 2010), so a smoke plume barely thins in a shower; for coarse dust
   (> 10 um) about 1e-3 1/s, so a dust cloud clears within minutes in rain. Applied as a density decay per Flow
   step, split by the gas's kind.

## Round 4 candidates (19 Nov, card to Paulo)

Two gaps found while building round 3; both read contracts that exist.

1. Fallen leaves land on water and snow (built 19 Nov). LeafFall3D settles every leaf on the ground under it, through a pond or
   a snow pack. A leaf reaching `WorldWater.height_at` floats: it drifts at the surface current
   (`WorldWater.flow_at`) plus a small leeway in the wind (about 2-3% of the 10 m wind for flat floating debris,
   Breivik et al. 2011), turns in eddies, and waterlogs and sinks over days (leaf breakdown, Webster and Benfield
   1986), sooner in a fast current. A leaf landing on `WorldSnow` depth lies on top and is buried as new snow
   falls; a dark leaf in sun sinks into the pack a little (it absorbs about 3x the sun the snow does).
2. Soot, ash and dust darken snow and speed its melt (built 19 Nov). SnowCover3D ages albedo (Verseghy 1991) but knows no
   impurities. Light-absorbing particles lower snow albedo: black carbon about 0.01-0.02 in the visible for 100 ppb
   (Warren and Wiscombe 1980; Hadley and Kirchstetter 2012); mineral dust 0.1-0.3, bringing melt a month earlier
   (Painter et al. 2007); burnt forests shed charred bark on the snow, cutting albedo by about 0.4 and melting the
   pack about 5 days earlier (Gleason et al. 2013). Sources: the weather map's smoke haze (dry deposition at about
   0.1-0.5 cm/s for PM2.5), WorldFire burn scars (charred debris under burnt trees, `burn_age_at`), ground and rock
   dust events. Impurities gather at the surface as the snow melts (melt amplification, Doherty et al. 2013) and are
   buried by new snow; the snow material shows them grey.


## Round 5 candidates (19 Nov, card to Paulo)

Gaps found by reading the readers, each on a contract that exists.

1. Floodwater soaks and silts the ground it covers (built 19 Nov). GroundSurface3D reads its own `water_sources` list for water over
   the ground (sand and water, stage 4), not WorldWater, so a river rising over its banks (WeatherRunoff3D), live
   water or a waterfall's pool never wets the ground they cover. Moving that read to `WorldWater.height_at`,
   `flow_at` and `sediment_at` (19 Nov) makes every water producer soak the ground (Green-Ampt as now), and the
   water's suspended load settles on the ground it floods: overbank deposition D = C w_s t per event, a few mm next to
   the channel falling off over tens to hundreds of metres (Pizzuto 1987; Asselman and Middelkoop 1995). When the
   water goes it leaves a wet silt or ash veneer that dries and cracks (mud's cracks), and the plants it laid stay
   laid.
2. Sand and snow drift in the lee of rocks, walls, buildings and vehicles (built 19 Nov: the contract carries the
   obstacles' height and their lee rather than `wind_at` at a height, since Sx wants heights and the sand wants the
   near-ground ratio). The snow cover's drift reads Winstral's Sx
   from the terrain alone; the sand's flux is sheltered by plants alone (Raupach). The weather map already bakes
   obstacles into its near-ground wind (WeatherShelter). Reading that through `WorldWeather.wind_at` at 0.1-2 m lets
   both settle where the wind slows: a snow fence's drift reaches about 12-15 H downwind and 1.2 H high at
   equilibrium (Tabler 1980, 1991); sand fills a lee shadow a few H long behind a boulder or a parked car (Hesp 1981,
   shadow dunes). Rocks (RockField3D) and vehicles join the shelter as obstacles.
3. The sea surges into sea caves (built 19 Nov, the water and the blowhole air; drawing with Valence). CaveNetwork3D sea caves have floors at or below low tide but no water. A WorldWater
   producer for the cave passages (the sea level at the mouth, wave set-up carried in and damped with distance) lets
   bodies wade and swim there, and Valence's surface draws it; where a roof opening meets the surface the trapped air
   drives a spout on the wave crests (blowholes; the spout's pressure scales with the wave's rho g H at the mouth).
   Needs Valence for the drawing.

Built 19 Nov (round 6, below): fog drip (cloud water caught by leaves and needles wets the ground under trees on foggy ridges, about 0.1-1
mm a day; Bruijnzeel et al. 2005).

## Round 6 (19 Nov, Paulo picked all three, in this order)

1. Fog drip under trees (built 19 Nov). Where cloud sits on the ground, leaves and needles comb its droplets out:
   catch = LWC u E per leaf area, about 0.05-0.5 mm/h on exposed ridges (Lovett 1984; Bruijnzeel et al. 2005).
   The weather map takes the liquid water content from the cloud's extinction at canopy height
   (LWC = 2/3 rho_w r_e beta, r_e 10 um), adds the catch to Rutter's canopy store and keeps the store from drying
   while in cloud; the drip's fog share goes to the surface film and the soil water.
2. Sea spray wets and salts the shore (built 19 Nov). Breaking waves make spray in proportion to the breaker height and
   the wind's square (Andreas 1998; surf zones much more than open water, de Leeuw et al. 2000). Coarse drops (0.1-1
   mm) settle about 1 m/s from about one breaker height, so they wet the first tens of metres; fine film and jet drops
   carry salt hundreds of metres, chloride deposition falling about tenfold over the first kilometre (Gustafsson and
   Franzen 1996). Salt spray browns the windward leaves of shore plants (Boyce 1954). Producers report breaking
   (`WorldWater.breaking_at`; PhysXWaterSurface3D: its surf zone and whitecaps); the weather map carries both sizes
   downwind on a fine square, steady along the wind, wets surfaces and soil and keeps the salt until rain takes it.
   Later readers: snow (salt lowers the melting point), rocks (salt bloom, a drawing job).
3. Wet tracks out of water and mud (built 19 Nov). A body out of water, wet mud or a puddle carries a film on its
   feet or tread and presses a share of it off with each print, the depleting-load trail games use for wet and
   bloody footprint decals (e.g. Red Dead Redemption 2), and drips round itself as its clothes drain. The prints go
   into the weather's contact wetness square (already read by grass, ground, rocks and the path tracer), which dries
   them at the weather's rate. Muddy prints followed in round 7.

## Round 7 (19 Nov, Paulo picked all three, in this order)

1. Salt burn on shrubs and trees (built 19 Nov). The plant and tree leaf shaders read the same `weather_spray_map`
   salt as grass: leaves brown one by one (by a hash per leaf cell) as the salt index rises from 0.15 to 0.85, and a
   browned tree leaf lets less light through (Boyce 1954; Griffiths 2006).
2. Salt melts shore snow and ice (built 19 Nov). Salt dissolved in the pack's water lowers its freezing point about
   0.0593 K per g/kg, so at -2 C a brine holds about 34 g/kg: a given salt load melts the snow it takes to dilute it
   that far and no more (the road-salt rule of thumb, Ketcham et al. 1996). The pack keeps the salt it is given and
   loses it with the meltwater that drains; bare lake ice melts by the surface salt.
3. Muddy prints leave mud (built 19 Nov). Bodies out of mud carry a clay film as well as water; each print on firm
   ground hands a share of it to the ground's tracked mud (`WorldGround.track_mud`, the contract's first write), the
   same depleting load as the wet prints. Vehicles and legs that already print their own mud are left to it.

## Round 8 candidates (21 Nov, card to Paulo)

Tyre grip on snow, offered on the 20 Nov card, was already linked (71387ca8): that card is withdrawn.

1. Salt bloom on sea rocks (built 22 Nov). Source: `WorldWeather.salt_at` (sea spray salt, g/m2), rain wash-off, the sun's
   drying. Reader: rocks (the stone material) and cliff faces. Spray-zone rock dries to a white crust of halite where
   the salt load is high and the surface has dried; rain dissolves it within minutes. Salt weathering in the spray
   zone (haloclasty, tafoni and honeycomb hollows) concentrates where wetting and drying alternate most (Mustoe
   1982; Matsukura and Tanaka 2000; Urosevic et al. 2013 on salt crystallisation in porous stone). Shading: a
   crust share = smoothstep of salt load times dryness, raising albedo and roughness in hollows first.
2. Spray-wet sand holds sharper prints (checked 22 Nov). Source: the weather's spray wetness and the swash's pore water (ground
   wet map). Reader: sand's print deformation. Surface moisture above about 4-6 % by mass gives sand apparent
   cohesion from capillary bridges, so prints keep steep walls and fine edges and wind lifts less of it (Namikas and
   Sherman 1995; Namikas et al. 2010; Davidson-Arnott et al. 2008 on beach surface moisture); dry sand slumps
   to its angle of repose. Sand already has moisture-driven cohesion from ground water: the link adds the spray's
   wetting above the swash.
3. Snow line and upslope rain from the cloud base (built 22 Nov, see the matrix). Source: `WorldWeather.cloud_base_at`, air temperature at
   height, wind, terrain slope. Readers: rain, snow, vegetation. Linear theory of orographic precipitation (Smith
   and Barstad 2004): uplift w = U . grad h condenses water at a rate set by the moist lapse rate, carried
   downwind over a fall time of 1000-2000 s, so windward slopes get more and lee slopes a rain shadow; the snow
   line sits where the wet-bulb freezing level meets the ground, about 300 m below the 0 C level in steady
   precipitation (melting layer). An open "later" row of the matrix since 20 Nov.

Sources: Smith and Barstad 2004 (J. Atmos. Sci. 61); Roe 2005 (Annu. Rev. Earth Planet. Sci.); Namikas and Sherman
1995; Davidson-Arnott, Yang, Ollerhead, Hesp and Walker 2008; Urosevic et al. 2013; Matsukura and Tanaka 2000.

## Round 9 candidates (22 Nov, card to Paulo)

1. Cold air pools in hollows (built 22 Nov). Source: the weather's air temperature, wind, cloud cover and the terrain heights it
   already reads for the snow line. Readers: dew and hoarfrost, mist (valley fog), snow melt, plant frost damage.
   On clear calm nights the cooled air at the ground drains downslope (katabatic flow, about 1-3 m/s) and pools in
   basins and valley floors, 5-10 C colder than the slopes above, with a warm belt part way up (Whiteman 2000;
   Clements et al. 2003 on Peter Sinks; Lundquist et al. 2008 on cold pools in mountain basins). Model: a
   per-cell drainage depth from a flow-accumulation pass over the weather's height grid, scaled by the night's
   radiative cooling and gated by wind and cloud; the contract's air temperature at a point reads it. Frost and
   fog then sit in hollows first, as they do.
2. Avalanche paths keep the forest young (built 22 Nov). Source: SnowCover3D's avalanche runouts. Reader: `WorldStand`
   (a new disturbance kind, 4 is taken by beaver: avalanche as 6), trees (snapped stems, flagged crowns), vegetation
   (shrub and alder thickets in the track). Frequent avalanches hold paths in shrubs and young broadleaves while
   the forest stands either side (Bebi, Kulakowski and Rixen 2009; Patten and Knight 1994; Johnson 1987 on
   track frequency). A runout records a disturbance over its deposit; return period sets the stand age.
3. Rime on windward trees and rocks (built 22 Nov). Source: `cloud_base_at`, air temperature below 0 C, wind, liquid water
   content of the cloud (fog drip already computes it). Readers: trees (crown load, bending with their wind),
   rocks and terrain material (white feathers growing into the wind on ridges in winter cloud).
   Accretion rate = collision efficiency x LWC x wind speed x area (Makkonen 2000 ISO 12494 model); hard rime at
   -5 to -15 C grows upwind as feathers. A per-cell rime load on WorldSnow (or a new WorldIce read) that sun and
   wind above 0 C shed.

Sources: Whiteman 2000 (Mountain Meteorology); Clements, Whiteman and Horel 2003 (J. Appl. Meteor.); Lundquist,
Pepin and Rochford 2008 (JGR); Bebi, Kulakowski and Rixen 2009 (Forest Ecol. Manage.); Patten and Knight 1994;
Makkonen 2000 (Phil. Trans. R. Soc. A); ISO 12494.

## Round 10 candidates (22 Nov, card to Paulo)

1. Valley fog seas from the cold pools. Source: WeatherMap3D's cold pool deficit and depth (`get_cold_pool_at`),
   the air's dew point. Reader: the clouds (a stratus layer whose top is the pool's top, seen from ridges as a sea
   of cloud), mist (near field already reads the humidity). Radiation fog fills a basin when the pooled air cools to
   its dew point and thickens upward to the inversion's base; it lifts and burns off from the edges after sunrise
   (Whiteman 2000; Gultepe et al. 2007 review of fog; Price 2011 on valley fog in the UK). Per cell: fog top =
   ground + pool depth where the floor air is saturated; the sky's volumetric clouds draw it as a flat-topped layer.
2. Needle ice and frost heave on bare wet ground (22 Nov: GroundSurface3D already grew it, mud round 9; the link built is its surface temperature). Source: the weather's surface temperature (cooling term), soil
   water (ground wetness, rain), night length. Readers: GroundSurface3D (a lifted, crumbly surface that keeps
   prints sharp until it collapses, pebbles lifted), vegetation (seedlings heaved out on bare soil). Needle ice grows
   where the surface sits a few degrees below 0 C over moist, fine soil, columns a few centimetres a night (Outcalt
   1971; Lawler 1988; Grab 2001 on needle ice in mountains). Shading: grey-white columns on wet silt and mud, gone
   by mid morning.
3. Fire builds its own cloud (pyrocumulus) (22 Nov: already built in ebdad64b and linked end to end: the cap at the plume's own condensation level, pyroCb lightning and rain through the storm list, its downdraft in the wind the fire spread reads, fire light on the cloud; nothing added). Source: `WorldFire.heat_at` integrated over a burning area, the air's
   temperature and humidity at height. Reader: the clouds (a convective tower over the plume when the fire's heat
   flux lifts the plume past the lifting condensation level; Briggs plume rise, Luderer et al. 2006; Fromm et al.
   2010 on pyroCb), lightning (a pyroCb can strike ahead of the fire), rain (rarely). Flow already carries the
   smoke near the fire; this adds the cloud seen from far.

Sources: Whiteman 2000; Gultepe et al. 2007 (Pure Appl. Geophys.); Price 2011 (Boundary-Layer Meteorol.); Outcalt
1971 (Water Resour. Res.); Lawler 1988 (Prog. Phys. Geogr.); Grab 2001; Briggs 1975; Luderer et al. 2006 (Atmos.
Chem. Phys.); Fromm et al. 2010 (BAMS).

## Round 11 candidates (22 Nov, card to Paulo)

1. Night drainage winds (katabatic flow) (built 22 Nov). Source: the cold pools' forcing (night, clear, calm) and the terrain slope
   WeatherMap3D already reads. Reader: `WorldWeather.wind_at` near the ground, so smoke, mist, snow drift, grass and
   tree wind and the fire spread (night fires back down slopes) all see it. Air cooled on a slope runs down it at 1 to
   3 m/s in a layer some 10 to 50 m deep, strongest a few metres up, and gathers into a down-valley wind
   (Prandtl 1942 slope flow; Mahrt 1982; Whiteman 2000, ch. 13). Per cell: speed from the radiative cooling and
   the slope sine (Prandtl's balance), along the fall line, faded by the ambient wind; zero by day (the upslope
   anabatic flow is weaker and later).
2. Valley fog wets and rimes what stands in it (built 22 Nov). Source: the valley fog's depth (built this round). Reader: the
   fog water at the canopy (`fog_water_at`), so fog drip wets the ground under trees in a fog sea and, below 0 C,
   rime grows on the trees in it (the hoarfrost forests of inversion valleys). Radiation fog holds 0.05 to 0.3 g/m3
   of liquid water (Gultepe et al. 2007); today `fog_water_at` reads only the sky's clouds.
3. Cold pools trap smoke near the ground (built 22 Nov). Source: the cold pool depth and the smoke layer's column (WeatherMap3D
   smoke). Reader: the near-ground haze and the smoke a camera sees in a valley at dawn. A night inversion caps the
   mixing: smoke from a fire or chimneys stays within the pool's depth instead of the 2 km day layer, tens of times
   denser near the ground, and clears as the pool breaks up mid-morning (Lavdas 1996, smoke in the southern US
   valleys at night; Achtemeier 2005, PB-Piedmont). Per cell: the mixing depth becomes the pool depth while a pool
   lies there.

Sources: Prandtl 1942; Mahrt 1982 (J. Atmos. Sci.); Whiteman 2000 (Mountain Meteorology); Gultepe et al. 2007;
Lavdas 1996 (USDA Forest Service GTR SRS-6); Achtemeier 2005 (Int. J. Wildland Fire).

## Round 12 candidates (22 Nov, card to Paulo)

1. Sea and lake breezes (built 22 Nov). Source: the land's surface temperature (the weather's radiative heating by day) against
   the water's (`WorldWater` and `get_water_temperature`), the coast from the water contract. Reader:
   `WorldWeather.wind_at` near the coast, so waves on lakes, spray, sand, smoke, grass and trees see it. By day the
   land warms past the water and a shallow onshore wind of 3 to 6 m/s sets in a few hours after sunrise, reaching
   10 to 40 km inland with a line of cumulus along its front; at night a weaker land breeze blows off the shore
   (Simpson 1994, "Sea Breeze and Local Winds"; Miller et al. 2003 review). Per cell: onshore component from the
   land-water temperature difference, faded by distance from the shore and by the ambient wind.
2. Day upslope (anabatic) winds and ridge cumulus (built 22 Nov). Source: the sun on slopes (the weather's shortwave and slope
   per cell), the drainage wind's counterpart. Reader: `wind_at` (1 to 3 m/s upslope by midday), the clouds
   (cumulus forming first over ridges and peaks where the upslope flows meet; Whiteman 2000, ch. 13; Banta 1990).
   Per cell: Prandtl's flow with the sign of the surface's heating; the clouds' cover raised over ridgelines.
3. Late frost in the hollows browns new leaves (built 22 Nov). Source: the air temperature at the canopy (now with the cold pools)
   against the trees' and plants' leaf-out stage (season forcing). Reader: TreeGenerator leaf colour and the
   vegetation's leaf season, a frost-damage share that lasts weeks until a second flush (Augspurger 2009 on false
   springs; Vitasse et al. 2014 on frost in valleys). Below about -2 C after budburst the young leaves die; the
   hollows' oaks and beeches show it while the slopes above stay green.

Later: Flow's local plumes (campfires, building fires) feed the regional smoke field (PhysX's note, 22 Nov), so a
campfire's smoke pools in a valley at night.

## Round 13 candidates (22 Nov, card to Paulo)

1. Foehn winds in the lee of ranges (built 22 Nov). Source: the orographic rain already taken from the air on the windward side
   (`cell_oro`), the wind across the ridge and the mountain waves' stability. Reader: `air_temperature_at` and
   `air_humidity_at` in the lee (several K warmer, humidity under 30 %), so snow melts and sublimates fast, the
   ground and fuels dry (fire), and a cloud gap opens over the lee with a cloud wall on the crest. Isentropic
   drawdown and the latent heat left behind upwind (Elvidge and Renfrew 2016, BAMS; Wuersch and Sprenger 2015;
   Seibert 1990). Per cell: dT = the dry/moist lapse difference over the height descended, scaled by the share of
   the moisture rained out.
2. Along-valley winds (valley and mountain-plain winds) (built 22 Nov). Source: the valley's air heated and cooled more than the
   plain's over the same height (the topographic amplification factor, the valley's volume per area). Reader:
   `wind_at` along valley axes: up-valley by day, 3 to 6 m/s, down-valley at night, deeper and longer-lived than
   the slope flows; smoke and fog carried along the valley (Whiteman 2000 ch. 10; Zardi and Whiteman 2013).
   Per cell: the valley axis from the smoothed height field, the pressure gradient from the heating difference.
3. Flow sources feed the regional smoke (PhysX's note) (built 22 Nov). Source: Flow emitters (campfires, PhysXBuildingFire3D)
   and their heat release. Reader: WeatherMap3D smoke cells through `add_smoke`, so a campfire's smoke pools in
   a valley at night, drifts with the drainage and valley winds, and dims the sun (Briggs plume rise for the
   injection height; the local plume stays Flow's within its domain, the regional field takes what leaves it).


Sources: Simpson 1994; Miller et al. 2003 (Rev. Geophys.); Whiteman 2000; Banta 1990 (Atmospheric Processes over
Complex Terrain); Augspurger 2009 (Funct. Ecol.); Vitasse et al. 2014 (Agric. For. Meteorol.).
