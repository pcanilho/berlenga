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
| `precipitation_at(pos)` | mm/h | 0 | Water equivalent falling |
| `rain_share_at(pos)` | 0..1 | 1 | Liquid share (Jennings 2018) at the air there |
| `surface_wetness_at(pos)` | 0..1 | 0 | Film over storage; 1 where puddles stand |
| `puddle_at(pos)` | mm | 0 | |
| `contact_wetness_at(pos)` / `add_contact_wetness(pos, radius, amount)` | 0..1 | 0 / no-op | Water any system splashes |
| `sun_direction()`, `sun_visibility_at(pos)` | unit vector, 0..1 | (0,1,0), 1 | Cloud shadow |
| `canopy_closure_at(pos)`, `shelter_at(pos, h)` | 0..1 | 0, 1 | |
| `season()` | dictionary (weather_season.h State) | 15 May, no snow | |
| `frost_at(pos)` | mm | 0 | Hoarfrost on exposed surfaces (`dew_and_frost`) |
| Events: `ground_strikes_after(serial)` | list | empty | Lightning, from `WeatherLightning3D` |

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

| Member | Unit | Default |
| --- | --- | --- |
| `height_at(pos)` | m | the ray to the physics floor |
| `soil_at(pos)` | Soil (soft, cohesion, friction angle, stickiness) | firm |
| `standing_water_at(pos)` | m | 0 |
| `ice_at(pos)` | (thickness m, broken share) | 0 |
| `frost_at(pos)` | (thawed top m, frost depth m, weakening) | 0 |
| `dent_at(pos)`, `rim_at(pos)` | m | 0 |
| `runoff_at(pos)` | (depth m, speed m/s) | 0 |
| `press(body)` | | through the contact group |

Consumers: vehicles, snow, grass (ruts tear the sward), fire (fuel on bare ground), water (wet sand), ground mud on
legs.

### Snow (`WorldSnow`; producers `SnowCover3D`, `SnowSurface3D`)

| Member | Unit | Default |
| --- | --- | --- |
| `depth_at(pos)` | m | 0 |
| `swe_at(pos)` | mm | 0 |
| `density_at(pos)`, `wetness_at(pos)`, `crust_strength_at(pos)` | kg/m3, 0..1, kPa | 0 |
| `surface_height_at(pos)` | m | ground height |
| `runoff_at(pos)` | mm/h | 0 |
| `canopy_load_at(pos)` | kg/m2 | 0 |
| shader globals `snow_cover`, `snow_rect`, `snow_depth` | | |

Consumers: ground, plants (burial), trees (load), vehicles (grip, ruts), cloth and hair, weather (runoff into the
surface water).

### Water (`WorldWater`; producers `PhysXWaterSurface3D` (sea, lakes, pools), `PhysXRiver3D`, `PhysXLiveWater3D`, `TerrainDataMap3D` (baked lakes and streams), `GroundSurface3D` (standing water))

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

### Fire (`WorldFire`; producer `PhysXFireSpread3D`)

| Member | Unit | Default |
| --- | --- | --- |
| `burnt_at(pos)`, `glow_at(pos)` | 0..1 | 0 |
| `heat_at(pos)` | W/m2 | 0 |
| shader globals `fire_state_map`, `fire_char_rect` | | |

Consumers: plants (char), ground (scorch), snow (melt), weather (drying), water (steam).

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
   burnables, cloth, hair, wetness, vehicles, waterfalls, cave mist, rock dust) and rockfall. Left: ground, snow,
   vegetation, which also read richer WeatherMap3D state (season, snapshots, shader code).
3. The contact reader replaces the copies of the body-size code: done 18 Nov for grass, the plants' trample field,
   pollen and water (`WorldContracts::body_extent`); the ground keeps its own reader (mass, wheels) until mud and
   sand move it onto `describe_body`.
4. Ground, snow, water, canopy and fire contracts, then their consumers.
5. Each bench gets its stubs.
