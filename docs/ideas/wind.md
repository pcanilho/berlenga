# Wind: one field for the whole world (18 Nov 2026)

Wind, rain and snow work as one system. There is one wind source, `WeatherMap3D`. Everything that moves
in air reads it:

- On the CPU: `get_wind_at(position, height_above_ground)`, `get_mean_wind_at(position)`,
  `get_wind_parts()` and `get_gust_waves()`.
- In shaders: `weather_wind(world_pos, height)` and `weather_height_above_ground(world_pos)`, from
  `WeatherMap3D.get_wind_shader_code()`.

The field is built in three layers, each covered in `weather.md`:
- the 32 m weather cells, which hold the mean wind at 10 m
- the shelter square, for canopies and obstacles near the ground (step 1b)
- the gust patches and Kaimal gust waves

This page covers the audit and the layers added after it.

## Audit (18 Nov)

| System | Reads the shared wind | Gap |
| --- | --- | --- |
| Grass, crops, plants | per blade, `weather_wind()` in the shader | |
| Trees and hedges | `TreeWind3D` via `get_wind_parts()` | |
| Cloth, hair | `get_wind_at()`, `weather_wind` on by default | |
| Clouds, storm cells | drift with the wind aloft (log law to 1 km) | no shear or turning with height (Valence, after ocean round 3) |
| Falling snow | `weather_wind()` per flake | |
| Lying snow | `get_mean_wind_at()`, SnowTran-3D transport, cornices | own Liston curvature and Sx, own climate wind (snow is moving both onto the shared field) |
| Sand, ground dust, rock dust, mists | `get_wind_at()` | |
| Fire spread, firebrands, pollen | `get_wind_at()` | |
| Wet-ground drying | Penman wind function | |
| Ocean | `follow_weather` | off by default (Valence) |
| Lakes, pools, rivers | none | no gust ripples or cat's paws (Valence) |
| Plain smoke (`PhysXFlow3D`) | none | no ambient wind |
| Godot particles | none | no wind |
| Falling rain | not built yet | the rain thread reads `weather_wind()` the same way snow does |
| Leaves | none | none fall or blow, and litter does not lift |
| Terrain | none | no ridge speed-up, lee calm or valley turning |

## Wind over terrain

### Published methods

- **MicroMet** (Liston and Elder 2006, J. Hydrometeorology 7). It is used by SnowModel and by MeteoIO's
  `ListonWindAlgorithm`.
  - Speed is scaled by `W = 1 + gs Ws + gc Wc`.
    - `Ws` is the terrain slope along the wind.
    - `Wc` is the curvature over a length scale `eta`, averaged over four directions (W-E, S-N and both
      diagonals).
    - Both are scaled to +-0.5, and `gs` and `gc` are 0.5 each.
  - Direction is turned by a diverting angle, `-0.5 slope sin(2 (aspect - direction))`, after Ryan
    (1977). This turns the wind toward the contours.
  - `eta` is about half the wavelength of the terrain's features.
- **Sx** (Winstral, Marks and Gurney 2002) gives the shelter of upwind terrain.
  - It is the steepest rise upwind within a search distance of about 100 m.
  - It is positive in a ridge's lee, and sorts terrain into scoured ground and drift zones.
- **Flow over low hills** (Jackson and Hunt 1975). The speed-up and the turning live in a thin inner
  layer whose depth scales with the hill's half-length. Higher up, the wind is the open wind again.
- **Other engines**:
  - Unreal's and Unity's wind zones are uniform volumes, so they have no terrain effect.
  - Ghost of Tsushima steers its wind by hand-painted maps.
  - Microsoft Flight Simulator (2020 onward) adds ridge lift from the terrain mesh for gliders. It uses
    the same idea as the vertical part below: the air follows the surface, `w = u . grad z`.

### As built (`WeatherTerrainWind`, modules/weather)

- **The square**:
  - It is 2048 m wide with 8 m cells (`terrain_wind_extent`, `terrain_wind_resolution`) and follows
    the camera.
  - Heights come in at about 8,000 a tick, so a 256 square fills in 8 ticks.
  - Heights resample when the camera has moved an eighth of the square. The derived field rebuilds
    when the wind turns by 3 degrees.
- **Height sources**:
  - `terrain_source`, if set.
  - Otherwise the first `TerrainDeformer3D`, `Landscape3D`, `TerrainGenerator3D` or `Terrain3D` in
    the scene, found by class name so weather does not link the terrain modules.
  - Otherwise rays on `terrain_collision_mask`.
- **Sea level**: a source with a `sea_level` sets `terrain_floor`, so the seabed does not steer the
  wind over the sea.
- **Per cell**:
  - `W`, using MicroMet's form with fixed scales instead of the domain's extremes, so that a small
    scene's bumps stay small:
    - the slope along the wind is +-0.5 at 45 degrees
    - the curvature is +-0.5 at 0.125 (a hill 20 m proud over `eta` = 40 m)
  - The diverting angle. It is built as the wind less its climbing part, which is MicroMet's
    `0.5 slope sin 2 phi` for small slopes.
  - The rise along the turned wind, for the vertical part.
  - The ground height.
  - Sx goes in a CPU array. It also slows `W` by up to `terrain_lee_strength` (0.4) between 5 and 25
    degrees.
- **Applied** in `get_wind_at`, `get_wind_parts`, `get_mean_wind_at` (at 10 m) and `weather_wind()`.
  - The change fades as `exp(-h / eta)` above the ground (the inner layer).
  - The gusts scale with the mean.
  - Near the ground the air follows the surface: `w = |u| dz/ds`.
- **Height above the ground**: `get_wind_at(p)` with no height, and `weather_height_above_ground()` in
  shaders, now measure from the sampled terrain instead of the flat `ground_height`.
- **Getters**:
  - `get_terrain_speed_factor_at`, `get_terrain_divert_at`, `get_terrain_ground_at` and
    `get_terrain_lee_at` (Sx).
  - `get_mean_wind_at(p, false)` gives the flat-ground wind, for systems that apply their own terrain
    terms until they switch over.

## Particles in the wind

### Published methods

- **Houdini's POP Wind** is "a drag pulling particles to the ambient wind speed", with an air resistance
  rate. Unreal Niagara's Wind Force and Unity's External Forces with Wind Zones do the same.
- **Stokes drag**: `dv/dt = k (u_air - v)`. Its rate `k` is gravity over the terminal velocity:
  - about 1.5 per second for a 2 mm raindrop (6.5 m/s)
  - about 10 per second for a snowflake (1 m/s)
  - about 1 per second for a hot metal spark

### As built

- **`ParticleProcessMaterial` and `CPUParticles3D`**:
  - `wind_drag`: the rate, 1 per second by default.
  - `wind_height`: the height above the ground, where -1 means from the particle's own position.
  - The drag acts across gravity only, so a particle's own fall is unchanged. The gusts' vertical part
    still lifts it.
- **Without a `WeatherMap3D`**, nothing changes.
- **Hookup**: the weather module hands its shader code and a CPU sampler to the scene classes when it
  starts. Scenes do not depend on modules.

## Next (owners agreed through the coordinator)

- **Waves (Valence)**:
  - `follow_weather` on by default. Explicit settings still win, and the spectrum eases over minutes.
  - Cat's paws as a moving gust field that scales the amplitude and roughness of the existing ripple
    layer.
- **Clouds (Valence)**: shear and turning with height.
- **Plain smoke (this thread)**: an ambient weather wind in `PhysXFlow3D`, with `PhysXFireSpread3D`'s
  own wind box switched off when a Flow has one.
- **Leaves (this thread; trees supplies the meshes and physics)**:
  - Falling leaves use the quasi-steady fluttering and tumbling plate model of Andersen, Pesavento and
    Wang (2005, JFM 541). Broad leaves flutter and needles drop almost straight.
  - Litter lifts when the near-ground wind passes a threshold.

## The wind contract (for docs/ideas/world-contracts.md)

The provider is `WeatherMap3D`, found with `WeatherMap3D.get_active()`. Units:
- positions are world space, in m
- velocities are in m/s, with (x, z) horizontal
- heights are in m above the ground

**CPU**
- `get_wind_at(position, height_above_ground = -1) -> Vector3`
  - The air velocity: log law, shelter, gust patches, Kaimal gusts, and the terrain's speed-up, turning
    and updraught.
  - With -1, the height is measured from the sampled terrain.
- `get_mean_wind_at(position, terrain = true) -> Vector2`
  - The mean at 10 m, with no gusts.
  - With `terrain = false`, the flat-ground value.
- `get_wind_parts(position, height, &mean, &along, &sigma)` and `get_gust_waves()`: for solvers that
  filter the turbulence themselves.
- `get_wind_aloft(position, altitude) -> Vector2`: the mean wind for clouds.
  - The log law and the Ekman veer up to `boundary_layer_height` (m).
  - Above that, growth by `wind_shear_aloft` (m/s per km).
- Terrain getters:
  - `get_terrain_speed_factor_at(position)`: W, 1 on flat ground.
  - `get_terrain_divert_at(position)`: the turn, in rad.
  - `get_terrain_lee_at(position)`: Sx, in rad, positive in a lee.
  - `get_terrain_ground_at(position)`: in m, NaN outside the square.
- `get_shelter_at(position, height)`: 0..1.
- `get_base_wind_direction()` and `get_base_wind_speed()`.

**Shaders** (`WeatherMap3D.get_wind_shader_code()`)
- `vec3 weather_wind(vec3 world_pos, float height_above_ground)`: m/s.
- `float weather_height_above_ground(vec3 world_pos)`: m.
- `vec4 weather_terrain(vec2 xz)`: W, the turn in rad, the rise along the wind, and the ground height in m.
- Globals: `weather_wind_map`, `weather_gusts`, `weather_shelter_map`, `weather_terrain_map`,
  `weather_map_rect`, `weather_wind_info`, `weather_shelter_rect`, `weather_patch_info`,
  `weather_terrain_rect`, `weather_terrain_info` and `weather_ground_height`.
  - All are declared at start, so a shader compiles with no `WeatherMap3D` present, and then has no wind.

**Consumers' settings**
- `ParticleProcessMaterial` and `CPUParticles3D`:
  - `wind_drag`: 1/s, 1 by default.
  - `wind_height`: m, where -1 means from the particle's position.
- `PhysXFlow3D`: `weather_wind` (on) and `weather_wind_coupling` (1/s).
- `PhysXWaterSurface3D`:
  - `follow_weather`: on by default.
  - The `cats_paws` uniform: on by default.
- `LeafFall3D` reads `get_wind_at` and `get_terrain_ground_at`.

**Stubs**: the weather bench's terrain stub is `docs/bench/weather/hill.gd`, which is any node with
`get_height(position)`.
