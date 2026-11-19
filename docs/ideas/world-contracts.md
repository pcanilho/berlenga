# World contracts: the interfaces between world systems

Every independent world system sits behind a contract: a small, documented interface that names what it provides,
in which units, with which defaults and guarantees. Consumers read the contract, never the producer's internals,
so each system can be tested alone against a stub of every contract it consumes, and the links bench
(`docs/bench/links`) tests them together. The living link matrix is [world-links.md](world-links.md).

## How it works

- `modules/world` holds the contracts: one abstract class per contract (`WorldWeather`, `WorldGround`, ...), each
  with script-overridable virtuals (`GDVIRTUAL`), so a stub can be written in GDScript.
- `WorldContracts` (an engine singleton) hands out the current provider of each contract:
  `WorldContracts.get_weather()`, `get_ground()`, ... The provider is the active producer node by default
  (`WeatherMap3D`, `GroundSurface3D`, ...), which registers itself on entering the tree. A test sets a stub with
  `WorldContracts.set_weather(stub)`; `clear_overrides()` puts the producers back.
- With no provider, every query returns the documented default, so a consumer never needs a null check and never
  needs the producer's module in the build (no `#ifdef MODULE_X_ENABLED` around a link).
- Producers keep their own richer APIs for their own tools and benches; only the contract is shared.
- A contract changes only additively. Removing or renaming a member, or changing a unit, needs every consumer
  moved in the same commit.

## Contracts

Units: metres, seconds, kg, degrees Celsius, mm of water, m/s. Positions are world space. "Default" is what the
contract returns with no provider.

### Weather (`WorldWeather`; producer `WeatherMap3D`)

| Member | Unit | Default | Guarantee |
| --- | --- | --- | --- |
| `air_temperature_at(pos)` | C | 15 | The live air (season, day's cycle, 6.5 K/km above the map's ground height). Without a clock and a climate it equals the `temperature` setting, eased |
| `air_humidity_at(pos)` | 0..1 | 0.7 | Relative, at the live air temperature |
| `dew_point_at(pos)` | C | from the two above | |
| `wind_at(pos, height_above_ground)` | m/s, Vector3 | 0 | Mean plus gusts, sheltered by canopy and obstacles, with the terrain's speed-up, turning and updraught; height < 0 is measured from the ground under pos |
| `mean_wind_at(pos)` | m/s, Vector3 (y 0) | 0 | 10 m mean, terrain included, no gusts |
| `wind_aloft_at(pos, altitude)` | m/s, Vector3 (y 0) | 0 | Free wind: log law and Ekman veer to `boundary_layer_height`, then `wind_shear_aloft` |
| `terrain_lee_at(pos)` | rad | NaN | Winstral's Sx from the terrain wind (positive in a lee; NaN outside it); fire's lee-slope whirls |
| `precipitation_at(pos)` | mm/h | 0 | Water equivalent falling |
| `rain_share_at(pos)` | 0..1 | 1 | Liquid share (Jennings 2018) at the air there |
| `rainfall_at(pos)`, `snowfall_at(pos)` | mm/h of water | 0 | The precipitation split by `rain_share_at` |
| `smoke_at(pos)` | g/m2 | 0 | Fires' smoke particulate in the column (the haze layer) |
| `washout_at(pos, coarse)`, static `washout_rate(rain, snow, coarse)` | 1/s | 0 | Below-cloud scavenging of particles, a R^b: fine smoke about 1e-5 at 1 mm/h, coarse dust about 3e-4, snow five times rain |
| `surface_wetness_at(pos)` | 0..1 | 0 | Film over storage; 1 where puddles stand |
| `puddle_at(pos)` | mm | 0 | |
| `contact_wetness_at(pos)` / `add_contact_wetness(pos, radius, amount)` | 0..1 | 0 / no-op | Water any system splashes |
| `sun_direction()`, `sun_visibility_at(pos)` | unit vector, 0..1 | (0,1,0), 1 | Cloud shadow |
| `canopy_closure_at(pos)`, `shelter_at(pos, h)` | 0..1 | 0, 1 | |
| `season()` | dictionary (weather_season.h State) | 15 May, no snow | |
| `frost_at(pos)` | mm | 0 | Hoarfrost on exposed surfaces (`dew_and_frost`) |
| Events: `ground_strikes_after(serial)` | list | empty | Lightning, from `WeatherLightning3D` |
| `day_of_year()` | 1..365 | 135 | The clock's date, else the map's season day |
| `season_at(pos, day = -1)` | dictionary (season State) | the default climate at pos.y | At the point's height, the live departure included; day < 1 is today |
| `season_climate(climate)` (C++) | `WeatherSeason::Climate` | false | The place's climate, for consumers that integrate over days (ground regrowth, frost seed); the math is `modules/world/world_season.h` |
| `cloud_cover_at(pos)` | 0..1 | 0 | |
| `surface_temperature_at(pos)` | C | the air | Exposed ground under a clear night sky runs below the air (`dew_and_frost`) |
| `water_temperature()` | C | 15 | Open water, lagging the air by about a month |
| `ground_height_at(pos)` | m | 0 | The terrain the weather sees under the point (its wind terrain), else the map's ground height |
| `time_scale()` | | 1 | The weather's time per real second |

Shader globals (the GPU side of the same contract): `weather_wind_map`, `weather_state_map`, `weather_map_rect`,
`weather_contact_map`, `weather_shelter_map`, `weather_frost_map` (R frost mm, G surface cooling K),
`weather_precip_phase`; formats in weather.md. Wind in shaders: `weather_wind(world_pos, height)`,
`weather_height_above_ground(world_pos)`, `weather_terrain(xz)` from `WeatherMap3D.get_wind_shader_code()`; all
globals are declared at start, so a shader compiles with no weather and has no wind.

The full wind side (gust parts and waves for solvers, terrain getters `get_terrain_speed_factor_at`,
`get_terrain_divert_at`, `get_terrain_lee_at`, `get_terrain_ground_at`, consumers' settings) is in wind.md, "The
wind contract"; solvers that filter the turbulence themselves still read `WeatherMap3D` for `get_wind_parts` and
`get_gust_waves`. Stub for the terrain under the wind: `docs/bench/weather/hill.gd` (any node with
`get_height(position)`).

Consumers: ground, snow, rocks, grass, plants, trees, pollen, mist, fire, firebrands, Flow dust, cloth, hair,
water (sea wind, mist, falls), vehicles, lightning.

### Contact bodies (`WorldContact`; producers: every moving body)

| Member | Unit | Guarantee |
| --- | --- | --- |
| group `world_contact` | | Every CharacterBody3D, RigidBody3D, PhysicalBone3D (project setting `physics/3d/world_contact/auto_join`) and vehicle wheel joins it; meta `world_contact_exclude` stays out |
| `describe(node)` | Body | Centre, radius, half height, shape (round, box, wheel), mass on the ground, contact area, velocity; one reader for every consumer (metas `trample_radius`, `touch_radius`, `ground_*`, `snow_*`, `deform_wheel_*`, else the first collision shape) |
| meta `ground_self_pressing` | | The body ruts the ground its own way (car wheels): the generic ground press skips it |

Consumers: ground, snow, grass, plants, trees, pollen, water. Each reads the group once per step; a body in two
groups counts once.

### Ground (`WorldGround`; producer `GroundSurface3D`)

Built 18 Nov (mud thread; sand adds its own reads on top). `WorldContracts::ground()` in C++,
`WorldContracts.get_ground()` in scripts; `set_ground(stub)` for a test, where the stub's unset methods fall back to
the live ground.

| Member | Unit | Default |
| --- | --- | --- |
| `height_at(pos)` | m, the deformed surface | the first physics surface under the point, else -INF |
| `soil_at(pos)` | soft, cohesion kPa, friction angle degrees, stickiness 0..1 | firm (soft false) |
| `standing_water_at(pos)` | m | 0 |
| `ice_at(pos)` | (thickness m, broken share) | 0 |
| `frost_at(pos)` | (thawed top m, frost depth m, weakening 0..1) | 0 |
| `dent_at(pos)`, `rim_at(pos)` | m | 0 |
| `runoff_at(pos)` | (depth m, speed m/s) | 0 |
| `add_runoff_load(pos, mineral_kg, ash_kg)` | kg since the caller's last call | dropped with no runoff producer | Loose material put on the runoff there; the catchment carries it down its D8 path into `WorldWater.add_sediment` where the flow reaches a river or lake. A separate producer slot (`WorldRunoffProvider`, `WorldContracts::register_runoff`; WeatherRunoff3D), so the near ground need not implement it. `has_runoff()`. Callers: burnt ground's ash (PhysX), soil erosion |
| `mud_depth_at(pos)` | m of slurry | 0 |
| `needle_ice_at(pos)` | (height m, crumbs 0..1) | 0 |
| `rut_guidance_at(contact, lateral, half_width)` | (cross slope along lateral, wall m on -lateral, wall m on +lateral) | 0 |
| `sand_at(pos)` | loose sand's share of the top, 0..1 | 0 |
| `ripple_at(pos)` | wind ripples' height about their mean, m | 0 |
| `blown_sand_at(pos)` | share of the sand the wind moves there now (dry, out of shelter), 0..1 | 0 |
| pressing | | through the `world_contact` group |

Consumers: vehicles, snow, grass (ruts tear the sward), fire (fuel on bare ground), water (wet sand), ground mud on
legs. Moving the vehicle's direct GroundSurface3D calls onto it is the next step (they also need the richer
terramechanics and pressing calls, which stay on the producer for now).

### Snow (`WorldSnow`; producers `SnowCover3D`, `SnowSurface3D`)

| Member | Unit | Default |
| --- | --- | --- |
| `depth_at(pos)` | m | 0 |
| `swe_at(pos)` | mm | 0 |
| `density_at(pos)`, `wetness_at(pos)`, `crust_strength_at(pos)` | kg/m3, 0..1, kPa | 0 |
| `surface_height_at(pos)` | m | ground height |
| `runoff_at(pos)` | mm/h | 0 |
| `canopy_load_at(pos)` | kg/m2 | 0 |
| `lake_ice_at(pos)` | (m, white share) | (0, 0) |
| `add_impurity(pos, radius, mg_per_m2)`, `impurity_at(pos)` | mg/m2 black carbon equivalent (dust 0.013 of its mass) | no-op, 0 |
| shader globals `snow_cover`, `snow_rect`, `snow_depth` | | |

Consumers: ground, plants (burial), trees (load), vehicles (grip, ruts), cloth and hair, weather (runoff into the
surface water).

### Water (`WorldWater`; producers `PhysXWaterSurface3D` (sea, lakes, pools), `PhysXRiver3D`, `PhysXLiveWater3D`, `TerrainDataMap3D` (baked lakes and streams), `GroundSurface3D` (standing water), `PhysXWaterfalls3D` (inside the falling sheets and columns, above where they land: thin, fast water))

Every producer is live at once and answers only where it really has water: the sea where its depth over the seabed
is above 0, a river inside its ribbon, live water and pools in cells with depth, the terrain map where its baked
water depth is above 5 mm, the ground where water stands over 2 mm. Of the producers that answer, the point reads
the surface nearest above it, else the nearest below it, so a cave stream, a rimstone pool or a pool in a hollow
below the sea's level reads its own water, and a body above the water reads the water under it.

| Member | Unit | Default | Guarantee |
| --- | --- | --- | --- |
| `height_at(pos)` | m | -INF (no water) | The rule above |
| `depth_at(pos)` | m | 0 | How far the point lies under that surface |
| `flow_at(pos)` | m/s, Vector3 | 0 | From the producer that answered `height_at` |
| `wake_depth_at(pos)` | m | 0 | |
| `disturb(owner, pos, radius, strength)`, `clear(owner)`, `impulse(pos, radius, strength)` | | no-op | Sent to every producer; each acts only where it has water |
| `sediment_at(pos)` | kg/m3 (x mineral, y ash and char) | (0, 0) | Suspended load of the water that answers; river and lake shading cloud and darken by it |
| `add_sediment(pos, mineral_kg_s, ash_kg_s)` | kg/s | dropped with no water | A load entering the water that answers there (runoff off burnt or bare ground, banks, slides); it carries it downstream |

Tests: `WorldContracts.set_water(stub)` replaces the whole contract; `WorldContracts.add_water_stub(stub)` adds a
script producer beside the real ones (a pool or a sea for a buoyancy bench without one).

Consumers: buoyancy, bodies in world_contact, plants (lean with the current), cloth (wading wetness), mist, ground
(wet shore), snow (lake ice).

### Canopy and crowns (`WorldCanopy`; producers `VegetationField3D`, `TreeWind3D`)

| Member | Unit | Default |
| --- | --- | --- |
| `crowns_near(pos, radius)` | list of (position, height, crown base, crown radius, conifer) | empty |
| `canopy_water_at(pos)` | mm | 0 |
| `trample_at(pos)` | Vector2, 0..1 | 0 |
| shader globals `grass_trample_map`, `grass_trample_rect` | | |

Consumers: fire (crown fire), lightning (attachment), weather (shelter), snow (interception), rain (drip).

### Fire (`WorldFire`; producer `PhysXFireSpread3D`, every live one)

| Member | Unit | Default | Guarantee |
| --- | --- | --- | --- |
| `heat_at(pos)` | W/m2 | 0 | The most heat release of any fire over the point, now |
| `burnt_at(pos)` | 0..1 | 0 | |
| `wind_at(pos)` | m/s, horizontal | 0 | The fires' indraft near the ground, summed over fires; adds to the weather's wind |
| `stem_char_at(pos)` | m | 0 | Char a stem foot there took (bark and wood), the most of any fire; trees weaken by it |
| `burn_age_at(pos)` | years (game time) | -1 (never burnt) | Since the ground last burnt, the most recent of any producer; vegetation's regrowth stage |
| `water_repellency_at(pos)` | 0..1 | 0 | Burnt soil shedding rain (fades over 1-2 game years); the ground cuts its infiltration by up to 0.9 |
| `burn_changed(area)`, `get_burn_generation()`, `burn_changes_since(gen, &area)` (scripts: `get_burn_changes_since(gen)`) | Rect2 in x, z (m) | generation 0 | Change log for `burn_age_at`: producers report where ages changed (`WorldContracts::fire_burn_changed(area)` reaches the built-in contract and any stub); readers keep the generation they read at and refresh only the merged area. The log keeps 64 reports; false: read everything again |
| `smoke_sources()` | (x, y, z, kg/s) | empty | Every fire's smoke particulate (PM2.5) per second of its own time; the weather carries it as a smoke layer (haze, sun, cooler day) |
| `plumes()` | (x, y, z, W) | empty | Every fire's convection column: its base and convective heat; the weather makes fire clouds from them |
| `crown_scorch_at(pos)` | m | 0 | Height crowns were killed to (lethal scorch, 1000 for a burnt crown), the most of any fire; trees die and stand as snags |
| shader globals `fire_wind_map`, `fire_wind_rect` | m/s | rect width 0 | The same indraft over the fire's area (RG: x, z), for grass |
| shader globals `fire_state_map`, `fire_char_rect` | | | burnt, glow, flame length, scorch height |
| shader global `fire_age_map` (over `fire_char_rect`) | years / 2 (R8) | 0 (never burnt) | Game years since the cell caught over 2, about 3-day steps; trees cast dead needles by it |

Producers: PhysXFireSpread3D (fires in play; `burn_age_at` in the weather's game time; `crown_scorch_at`), WorldBurnScar3D (an
authored old burn: a rect or polygon and `age_years`, modules/world; reports its old and new area on entering, moving and
leaving). Every producer of `burn_age_at` reports its changes with `fire_burn_changed`.

Tests: `WorldContracts.set_fire(stub)` (`_burn_age_at` in a stub), or WorldBurnScar3D nodes.

Consumers: weather (the surface film boils off under a fire), plants (char; grass leans into `fire_wind_map`), cloth
(`wind_at` adds to its wind), trees (TreeWind3D weakens stem feet by `stem_char_at`; trees killed by `crown_scorch_at` stand as snags that weaken by `burn_age_at`), snow (melt), water (steam), vegetation (VegetationField3D and GrassField3D regrowth by `burn_age_at`, refreshed through the change log).

## Testing

- Each system's bench runs alone against stubs: `docs/bench/<system>` sets a GDScript stub for each contract it
  consumes (a still, 10 C, dry weather; flat firm ground; no snow) through `WorldContracts.set_*()`, so its numbers do
  not move when another system changes.
- `docs/bench/links --test` runs the real producers together and checks the links listed in world-links.md.
- A contract test per contract (`docs/bench/links --test=contracts`) checks each producer against its table:
  units, defaults with the producer absent, and guarantees.

## Migration

Today systems reach into each other through `X::get_active()`, `#include "modules/x/..."` under
`#ifdef MODULE_X_ENABLED`, `ClassDB` names and shader globals. Cross-module includes found (18 Nov): PhysX → weather
(15 files), vegetation → weather (8), vegetation → terrain data map (5), snow → weather (4), rocks → terrain
generator (3), PhysX → ground (3), snow → ground deform (3), and about 15 single ones. The order:

1. `modules/world` with the registry, the weather and contact contracts, and stubs. Producers register. Done 18 Nov
   (weather, water, contacts).
2. Weather consumers move to `WorldContracts::weather()`, module by module. Done 18 Nov: PhysX (fire, firebrands,
   burnables, cloth, hair, wetness, vehicles, waterfalls, cave mist, rock dust), rockfall, ground (all reads; no
   longer depends on the weather module), snow (point reads: air with its own lapse rate from the ground under the
   point, snowfall, canopy, icicles, falling and blowing snow), vegetation (grass season, mist, pollen, leaf fall).
   Kept on the producer as its richer API: snow's whole-map snapshot and its writes into the weather (precipitation
   extinction, melt water), the cornice's cell sampling, the tree wind solver's gust parts and waves, the wind
   shader code (static), the placement skew. A `set_weather(stub)` script stub counts as a provider.
3. The contact reader replaces the copies of the body-size code: done 18 Nov for grass, the plants' trample field,
   pollen and water (`WorldContracts::body_extent`); the ground keeps its own reader (mass, wheels) until mud and
   sand move it onto `describe_body`.
4. Ground, snow, water, canopy and fire contracts, then their consumers.
5. Each bench gets its stubs.
