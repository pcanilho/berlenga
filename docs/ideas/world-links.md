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
| `SkyClock` | weather | date, time, sun and moon |
| `GroundSurface3D` | ground | soil per point (`get_soil`), deformation field (`ground_deform_map`), standing water, puddle ice, frost |
| `SnowCover3D` | snow | `snow_cover` maps, depth, air at height (`get_air_temperature_at`) |
| `fire_state_map`, `WorldFire` | fire | burnt share and glow per cell; `heat_at` (W/m2), `burnt_at` |
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
| Gravel and pebbles → no sand ripples, less blown sand | contract (19 Nov) | `WorldGround.coarse_cover_at` from RockField3D's cover maps | A gravel lag armours the sand under it (Bagnold 1941; Sharp 1963): sand scales its blown share and ripples by 1 - smoothstep(0.1, 0.35, pebble + gravel) (sand thread builds the read) |
| Water, snow → fallen leaves | linked (19 Nov) | WorldWater (height, flow), WorldSnow (depth, surface, lake ice), weather wind | LeafFall3D leaves land on the top surface: on water they float with the current plus 3% leeway in the wind (Breivik et al. 2011), on snow they lie on top, are buried by more snow and set down when it melts; lake ice holds them |
| Vehicles → grass, branches | linked (18 Nov) | world_contact | wheels and bodies join |
| Lying snow → plants | linked (18 Nov) | snow cover (snow map) | as above (GrassField3D, PlantGenerator) |
| Flowing water → reeds, grass | linked (18 Nov) | WorldWater flow and surface | the trample fields take the current's drag (Cauchy number, Luhar and Nepf 2011): grass and plants lean downstream, flat in a fast current |
| Springs → oasis plants and grass | linked (19 Nov) | DesertSprings (vegetation, from TerrainGenerator3D sand seas): `VegetationField3D.get_springs()`, `get_spring_wetness_at(pos)` (positions m, wetness 0-1, -1 off the sand seas) | readers VegetationField3D RULE_SPRING and GrassField3D; Valence adds the pools next; the spring finder should move to the terrain generator later. Anything cycling species meshes by index skips meshes with `tree_fire_years` meta |
| Nebkhas → shrubs | linked (19 Nov) | TerrainGenerator3D `get_nebkhas(rect)` (x, top y, z, height m), `get_nebkha_height(pos)` (mound sand alone); `sand_sea_mask` (Image, red 0..1) shapes the ergs | VegetationField3D places a shrub on each mound top; the mounds are sand for the sand thread, no deformation hook needed (terrain-gen cf579682) |
| Lightning → tree | linked (18 Nov) | lightning | attachment; fire ignition below |
| Lightning → struck tree variant | linked (20 Nov) | `WorldStand.strike_age_at` | WeatherLightning3D records canopy strikes; VegetationField3D swaps to a `tree_lightning_years` variant (field side: trees) |
| Beaver dam fails → beaver meadow | linked (20 Nov) | `WorldStand` `DISTURBANCE_BEAVER` (4), age since the pond drained | trees report it when a STAND_BEAVER_POND dam fails; PlantGenerator.make_beaver meadow reads it by RULE_DISTURBANCE (4,4) |
| Deer browsing → understorey, saplings | linked (20 Nov) | `WorldStand.browse_at`, `browse_reach_at` | browse discs (pressure, reach); vegetation RULE_BROWSE 31, trees' browsed variants (trees.md 5aw) |
| Wet ground → mist | linked (18 Nov) | surface wetness, surface cooling | VegetationMist3D: wet ground mists more and sooner |

### Water (ocean, rivers, pools, falls)

| Pair | Status | Source | Note |
| --- | --- | --- | --- |
| Wind → waves | linked (18 Nov) | wind | `follow_weather` on (wind work) |
| Rain → ripples | linked | weather contract | live water's rain rings (`rain_from_weather`) |
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
| Cloud base → snow line, orographic rain | later | `cloud_base_at` | |
| Any system → cloud density | linked (20 Nov) | `add_cloud_effector` callers | `WorldWeather.cloud_effector_at`; shadow bake and sky both see it |
| Terrain, wind aloft, temperature, humidity, mid/high layers → wave clouds (lenticulars) | linked (20 Nov) | terrain source, `get_wind_aloft`, air, `mid/high_cloud_cover` | `WeatherMap3D.get_wave_cloud_density_at` / `WorldWeather.wave_cloud_density_at`; shadow bake (docs/ideas/clouds.md round 5) |
| Time of day, wind, humidity, rain → low cloud type (stratus, stratocumulus, cumulus) | linked (20 Nov) | clock, `cur_*` air | `WeatherMap3D.cloud_regime_from_weather`, `get_convective_heating` (docs/ideas/clouds.md round 6) |
| Wind shear aloft, air temperature → cirrocumulus; high cover (humidity, coming rain) → cirrostratus veil | linked (20 Nov) | `get_wind_aloft`, air | `WeatherMap3D.get_cirrocumulus_share` (docs/ideas/clouds.md round 7) |
| Storm rain, outflow, steering wind → rain shafts and shelf cloud | linked (20 Nov) | `add_storm` storms | sky rain shafts; `get_cloud_base_at` / `WorldWeather.cloud_base_at` lowered by the shelf (docs/ideas/clouds.md round 8) |
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

### Vehicles

| Pair | Status | Source | Note |
| --- | --- | --- | --- |
| Soft soil → sinkage, grip, ruts | linked | ground terramechanics | |
| Puddles → spray, washed tread | linked | ground | |
| Wet, flooded, icy firm ground → grip, aquaplaning | linked (18 Nov) | weather contract, ground ice | |
| Snow, lake ice → tyre grip | linked (18 Nov) | WorldSnow (depth, density, wetness, crust, lake ice) | packed snow 0.2-0.35, slush lower, crust near ice; tread_firm_grip scales by tread (chains, lugs) |
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
