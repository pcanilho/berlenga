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
| Weather → haze | linked | air humidity, precipitation | |
| Clear calm nights → dew, hoarfrost | linked (18 Nov) | air, net radiation | `dew_and_frost`, Monteith 1957; `weather_frost_map` on terrain, grass, plants, rocks |
| Lightning ← storms, freezing level | linked | cells, air | |
| Lightning → tall things | linked (18 Nov) | ray + canopy map | rolling sphere |

### Terrain and ground (sand, mud)

| Pair | Status | Source | Note |
| --- | --- | --- | --- |
| Rain, puddles → ground wetness, mud | linked | cells | |
| Air → puddle ice, soil frost, thaw | linked | air, season | |
| Wind → sand transport, dust | linked | wind | |
| Season → regrowth of ruts | linked | season | |
| Snow → ground | linked | snow cover | |
| Water bodies → wet sand (swash) | linked | contact wetness | |
| Fire → scorched, ashy ground | linked | fire char decal | PhysXFireSpread3D's decal |
| Ground → dust (Flow) | linked | Flow | |
| Terrain → runoff, rivers | linked | terrain runoff (monthly) | storm runoff to rivers: rain work |

### Snow

| Pair | Status | Source | Note |
| --- | --- | --- | --- |
| Air → snowfall, melt | linked (18 Nov) | air | live air with snow's own lapse rate |
| Sun, cloud shadow → melt | linked | sun, cloud shadow | |
| Wind → drifts, cornices | linked | wind | |
| Rain → wet snow, crust | linked | cells | |
| Canopy → interception | linked | shelter | |
| Snow → plants (burial, pressing) | linked (18 Nov) | snow cover (snow map) | grass and plant shaders: load ρ g d against stem stiffness lays plants down, deep snow hides them |
| Fire → snow melt | linked (18 Nov) | WorldFire `heat_at` | SnowCover3D melts H dt / L_f under the fire, whatever the air |
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
| Vehicles → grass, branches | linked (18 Nov) | world_contact | wheels and bodies join |
| Lying snow → plants | linked (18 Nov) | snow cover (snow map) | as above (GrassField3D, PlantGenerator) |
| Flowing water → reeds, grass | linked (18 Nov) | WorldWater flow and surface | the trample fields take the current's drag (Cauchy number, Luhar and Nepf 2011): grass and plants lean downstream, flat in a fast current |
| Lightning → tree | linked (18 Nov) | lightning | attachment; fire ignition below |
| Wet ground → mist | linked (18 Nov) | surface wetness, surface cooling | VegetationMist3D: wet ground mists more and sooner |

### Water (ocean, rivers, pools, falls)

| Pair | Status | Source | Note |
| --- | --- | --- | --- |
| Wind → waves | linked (18 Nov) | wind | `follow_weather` on (wind work) |
| Rain → ripples | linked | weather contract | live water's rain rings (`rain_from_weather`) |
| Cloud shadow → water | linked | cloud shadow | |
| Water → wet shore | linked | contact wetness | |
| Falls → mist → wet rock | linked | mist | |
| Bodies with buoyancy → wakes | linked | buoyancy, wake nodes | |
| Bodies without buoyancy → ripples, splash | linked (18 Nov) | contact | world_contact bodies disturb sea, river |
| Wading → wet legs, clothes | linked (18 Nov) | WorldWater | ClothWetness (PhysX, d90fa990) |
| Cold air over water → steam fog | linked (18 Nov) | air, WorldWater, `get_water_temperature()` | VegetationMist3D cells over water |
| Rain, melt → river level | linked (18 Nov) | WeatherRunoff3D (SCS curve number, D8 routing) | rain thread: rivers add the discharge reaching them; snowmelt runs off too |
| Cold → lake ice | linked | air, snow | SnowCover3D lake ice, Stefan's law under the snow (Leppaeranta 1993) |
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
