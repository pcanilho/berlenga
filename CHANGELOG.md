# Changelog

What changed in this fork, newest first. Upstream Godot's own changes are in its release notes.

Every pull request adds its entry as a file in `changelog.d/` (see [changelog.d/README.md](changelog.d/README.md)), in one of: Added, Changed, Fixed, Removed. Write what changes for someone using the engine, in one line, with the PR number. The release workflow publishes those entries as the release notes and moves them here under that release's heading.

## Unreleased

## build-7c77cee2 (2026-11-10)

### Fixed
- Clouds no longer repeat in a grid: the shape noise is read twice at an irrational ratio of scales and turned against the world's axes, at 128³ instead of 64³. Cover still means share of sky.

### Changed
- README: coarse-to-fine erosion line and image.
- README: footprints in ground line and image.
- README: melt-freeze crusts line and image.
- README: terrain ground material line; snow and wetness shader code for ShaderMaterials.

### Added
- `TerrainMaterial3D`: a ground material that writes its own shader: rock, scree, soil, alluvium and sand layers chosen by the terrain generator's geology (or by slope without it), height-blended, biplanar on cliffs, with the erosion detail normal, grass ground tint, wetness and snow.
- `BaseMaterial3D.get_snow_shader_code()` and `get_wetness_shader_code()`: the engine's snow and wetness lines for any `ShaderMaterial`, the same text `BaseMaterial3D` uses.

## build-dabf251a (2026-11-10)

### Changed
- Weather: precipitation haze layers are lit by the sky as well as the sun, so blowing snow reads white. A source given a radius now sets the layer's ground at its centre's height, so the layer follows the terrain instead of sitting at `ground_height`.
- README: terrain climate maps line and image.
- README: vegetation climate placement and canopy shade lines.

### Added
- `PlantSpecies` climate rules: `temperature_range`, `january_temperature_range`, `precipitation_range`, `aridity_range` and `sunlight_range`, read from `TerrainDataMap3D.bake_climate()`.

## build-bc9b8f27 (2026-11-10)

### Changed
- Weather: precipitation haze layers are lit by the sky as well as the sun, so blowing snow reads white. A source given a radius now sets the layer's ground at its centre's height, so the layer follows the terrain instead of sitting at `ground_height`.
- README: terrain climate maps line and image.
- README: indirect light reuse (ReSTIR GI) line.
- README: snowstorms line and blizzard image.
- README: vegetation climate placement and canopy shade lines.

### Added
- Path tracing: indirect light reuse (ReSTIR GI), `rendering/pathtracing/indirect_reuse`, off by default. The first bounce of each pixel's diffuse samples is resampled across frames and neighbouring pixels. A room lit through one window goes from 22.8 to 30.0 dB at 4 spp, better than 64 spp without it. Uses 104 bytes per internal pixel.
- `PlantSpecies` climate rules: `temperature_range`, `january_temperature_range`, `precipitation_range`, `aridity_range` and `sunlight_range`, read from `TerrainDataMap3D.bake_climate()`.

## build-943d6ac9 (2026-11-10)

### Fixed
- Weather: `WeatherLightning3D.get_last_channel()` keeps the newest ground flash's channel after its light fades.
- Weather: lightning casts shadows in raster. A far ground flash now lights the raster as a directional light from the channel's direction, with sharp cascaded shadows, instead of an omni light whose shadows were lost over kilometres. `raster_shadow_distance` sets how far its shadows reach.
- Weather: `snap_to_settings()` refreshes the map's cells at once (so followers read the snapped state, not the old one), and the first update after it eases from there instead of snapping again.
- Water: `follow_weather` no longer starts from the weather map's stale wind (it snaps on the first cells after a snap), and `WeatherMap3D.advance()` grows the sea along with the rest of the weather.
- `SnowCover3D` could crash in its parallel update when another node held a copy of its runoff map.
- `TerrainDataMap3D`: a water surface with a `surface_mesh` outline (a lake) is water only inside its outline again; the open sea still covers the whole map.

### Added
- Weather: `WeatherMap3D.get_cloud_top_at()`, the top of the cloud the sky draws over a point. Storm flash rates use it: a storm's core reaches the base plus three times `cloud_thickness`.
- Weather: `WeatherLightning3D` makes lightning from the weather map's storms. The rate follows the cloud top (Price and Rind 1992), and the share that reaches the ground follows the cold cloud depth (Price and Rind 1993). A ground flash is a tortuous, branching channel with return strokes, lit under path tracing as an emissive light and in raster by a light at the channel. Flashes glow inside `WeatherSkyMaterial` clouds. Thunder is synthesized from the channel itself and arrives late by its distance. Scripts can call `strike()` and listen for `struck`.
- Weather: `WeatherMap3D.set_precipitation_extinction()` lets snow and rain nodes thin the air. Uniform haze drives the environment's fog, coloured by the sky. A layer near the ground becomes lit volumetric fog. Both renderers read it, and the environment is put back as it was when the last source clears.
- `SnowClimate`: a place's winter (a WGEN-style weather generator) baked over the whole terrain by `SnowCover3D.bake_climate()` through the snowpack model: snow lines where the lapse rate puts them, shaded faces keeping their snow, loaded lee slopes. `SnowCover3D.climate` keeps it as a world tier carried on by the weather; it seeds the near square and is drawn beyond it by materials and the path tracer (`snow_world_map`, `snow_world_rect`).
- `SnowCornice3D`: the cornices the world tier grows on lee crests in the right wind, drawn near the camera with collision.
- `SnowCover3D.precipitation_gradient`: more precipitation higher up.
- `SnowFall3D`: falling and blowing snow around the camera from the `SnowCover3D` and the weather, in raster and under path tracing. Flake sizes and fall speeds follow measurements of snowfall, carried by the weather map's wind with its gusts and shelter; flakes stop under roofs; each is drawn as a streak of its motion during `streak_time`. Above the threshold wind of dry snow, snow blows off the pack in a saltation layer and a drifting layer above it. `get_extinction()` and `get_visibility()` give the fog of the snowfall beyond the particles.
- `SnowCover3D.get_air_temperature_at()` and `get_snowfall_rate_at()`.
- `SnowSurface3D`: deep snow as real geometry on top of the `SnowCover3D` pack, in raster and under path tracing. Bodies in the `snow_deform` (or `grass_trample`) group press prints and tracks into it: snow compacts under them and is pushed into a rim, loaded bodies sink by Bekker's pressure-sinkage law, and a collision patch keeps them in their own tracks. New snow and wind fill tracks, settling and melt shrink them, and tracks are kept when the camera leaves and returns. `press()`, `press_box()`, `get_surface_height_at()`.
- `SnowCover3D.get_new_snow_at()`, `sample_ground_height()`, `get_update_count()`, `get_drift_factor()`.
- `TerrainGenerator3D` coast and lakes: `ocean_edges` or `sea_map` make sea that the land drains to, with a Dean-profile seabed and beaches; lakes form behind natural dams across valleys (`lake_count`, or `lakes` markers), at their spill level or lower when evaporation wins; `build_water()` creates the sea and lake surfaces (PhysXWaterSurface3D when present) and `build_river_paths()` ends rivers in them.
- `TerrainGenerator3D.get_detail_shader_code()`: the erosion detail filter's next octaves as a shader function (`terrain_detail_normal()`), its slope differentiated analytically, for gullies below the vertex spacing in the terrain material; octaves fade out by pixel footprint.
- `TerrainGenerator3D` tile erosion (`tile_erosion_enabled`): droplet hydraulic erosion with loose and bedrock layers plus thermal slides on a fine grid, in overlapping tiles cross-faded so there are no seams; deterministic, threaded. Rills and alluvial valley floors; flow and deposit maps (`get_tile_flow()`, `get_tile_deposit()`).
- `PlantSpecies.canopy_radius`, `canopy_opacity` and the `shade_range` rule: canopy species shade the ground and understorey species are placed by that shade. `VegetationField3D.get_shade_at()` returns it.
- `VegetationField3D.weather_map`: the placed canopy is written to `WeatherMap3D.canopy_map`, so snow, wind and light under trees follow the same forest as the shade rule.
- `TerrainDataMap3D.geology_source`: bakes soil depth, bare rock and scree from a `TerrainGenerator3D`; `PlantSpecies` rules `soil_depth`, `rock` and `talus` keep trees off bare rock and scree.

### Changed
- README: lightning raster shadows.
- README: lightning and thunder line.
- README: vegetation placement by geology line.
- README: precipitation haze line.
- README: snow climate and cornices lines and images.
- README: snow shelter line and image.
- README: snow sparkle line and image; cornices on milder ranges.
- README: deep snow with tracks.
- README: falling snow line, image and clip.
- README: terrain coast and lakes line and images.
- README: terrain material detail line and image.
- README: terrain tile erosion line and image.
- Blowing snow (`SnowFall3D`) also lifts a fresh surface on a settled pack.
- `TerrainDataMap3D`: rivers that list their cross-sections are only queried near their channel, so world-size maps with rivers bake about 18 times faster (same result); water surfaces always bake as a level plane, without their waves.

## build-defbd730 (2026-11-10)

### Added
- Path tracing: water caustics in three cascades (16, 64 and 256 m by default), so the swell's light bands reach far out while near lines stay as sharp. `rendering/pathtracing/caustics_cascades` (default 3; 1 is the single map).

### Changed
- README: caustic cascades line.
- README: terrain geology maps and river network; irregular crests; refreshed terrain images.

## build-f4df6168 (2026-11-10)

### Added
- Grass carpet: `GrassField3D.view_facing` (edge-on blades widen), `blade_segments` (short undercoat fields), ground tint for terrain shaders (`GrassField3D.get_ground_shader_code()`), `PlantSpecies` patches (`patch_scale`, `patch_cover`, `patch_invert`) and `visibility_range` for clover, flowers and other ground cover.
- `GrassField3D` trample field: any node in the `grass_trample` group (radius from its collision shape) lays the grass down, and it springs back over `trample_recovery` seconds; `add_trample()` for scripts.
- Spatial shader built-in `PREVIOUS_FRAME` (true while the vertex function runs for last frame's position) and render mode `alpha_motion_vectors` (a blended surface writes motion vectors in the alpha pass, blended by its alpha)
- PhysXHair3D strands write motion vectors, so TAA, FSR 2 and DLSS follow moving hair instead of smearing it
- Water: shore waves get their own heights (Rayleigh, in sets of about five bigger waves every twelve or so), so each breaks at its own place: big sets on the outer bars, small waves further in. `PhysXWaterSurface3D.shore_wave_groups` (default 1; 0 gives the old regular waves).
- `SnowCover3D`: a snowpack that follows the camera and takes its weather from `WeatherMap3D`: rain or snow by temperature and humidity, colder with altitude, new snow density by temperature, settling, albedo ageing, melt by temperature and sunlight on each slope, meltwater held and refrozen, snow sliding off steep slopes and drifting into the lee of ridges. Queries `get_depth_at()`, `get_swe_at()`, `get_runoff_at()` and more; shader globals `snow_map`, `snow_map_rect`.
- BaseMaterial3D Snow (`snow_enabled`, `snow_amount`, `snow_max_slope`, `snow_from_weather`): snow cover from the snowpack on any material, in raster and under path tracing.
- `TerrainGenerator3D`: generates mountain ranges for a whole world from tectonic uplift (painted map, Path3D ranges, noise) against stream power erosion (Cordonnier et al. 2016, Braun and Willett 2013 implicit solver), with sediment-filled basins, a critical slope, hillslope diffusion and multiple-flow-direction drainage. Deterministic per seed. Writes into Terrain3D (by name) or a `HeightMapShape3D`, re-applying a `TerrainDeformer3D`'s brushes on the new ground; exposes height, drainage area and sediment maps.
- `TerrainGenerator3D` detail: an erosion filter (Clay John 2018, Fewes 2023, Johansen 2026) carves branching gullies and spurs into the simulated ranges, computed per point so tiles join exactly; written in parallel (`detail_wavelength`, `detail_octaves`, `detail_depth`).
- Weather: falling snow no longer wets surfaces (only the rain share, `WeatherMap3D.rain_fraction()`, reaches the water film), and a snowpack's runoff does instead (`set_surface_water_source()`, fed by `SnowCover3D`). New getters `get_air_temperature()`, `get_air_humidity()`, `get_canopy_height_at()`, `get_canopy_closure_at()`.

### Changed
- Water: plunging-breaker lips build on worker threads, about a third less CPU per tick, with the same mesh and spray.
- README: grass trampling and ground cover lines; new grass images and clip.
- README: hair motion vectors line and image.
- README: Snow section.
- README: terrain erosion detail images; detail fades on crests.
- README: mountain generation erosion filter detail.
- README: Terrain section with mountain generation; deformation and data map lines moved there.
- README: shore wave groups line.

### Fixed
- `TerrainGenerator3D` detail no longer notches crests into a comb of teeth: the filter fades with the slope's coherence where opposite slopes meet. New `sample_heights()` returns the final ground on a grid, filled in parallel.

## build-13a222ec (2026-11-10)

### Added
- PhysXHair3D `volume_enabled` (on), `volume_friction`, `volume_stiffness`: strands push and drag on each other through a density and velocity grid (Petrovic et al. 2005), so hair keeps its volume and moves as a mass instead of packing flat

### Changed
- README: cloud reprojection line.
- README: Clouds section split out of Weather.
- README: hair volume line and image.

## build-7eb6a91b (2026-11-10)

### Added
- Sky shaders: `half_res_reprojection` render mode. The half resolution pass is drawn a sixteenth at a time and resolved at full resolution, as Nubis does. `WeatherSkyMaterial.cloud_reprojection` is on by default: full-resolution clouds for a quarter of the marching.
- `GrassField3D`: procedural grass around the camera, blades seeded by world position and grouped in clumps, rules over a `TerrainDataMap3D`, LOD by rank, wind from `WeatherMap3D` and wetness from the weather map; visible to the path tracer.
- Path tracer: light reuse across pixels and frames (`rendering/pathtracing/light_reuse`, ReSTIR DI, off by default): +1.2 dB at 1 spp with hundreds of overlapping lights.
- BaseMaterial3D Wetness (`wetness_enabled`, `wetness_porosity`, `wetness_amount`, `wetness_from_weather`): surfaces darken and turn glossier when the weather map is wet (Lagarde 2013), in raster and under path tracing.
- `TerrainDataMap3D`: baked terrain analysis maps for placement and materials: slope, aspect, curvature, flow accumulation, wetness index, water depth and signed distance to water, as CPU queries and shader globals.
- Vegetation placement: `PlantSpecies` (meshes, spacing, footprint, rules on slope, height, curvature, wetness, water depth and distance), `Biome` (species in priority order) and `VegetationField3D` (tiled Poisson-disk placement over a `TerrainDataMap3D`, one MultiMesh per mesh per tile).

### Changed
- README: Vegetation grass line, image and clip.
- README: light reuse (ReSTIR DI) line.
- README: Vegetation plant placement line and image.
- README: Vegetation terrain data maps line and image; terrain analysis papers in docs/REFERENCES.md.
- README: wet surfaces material line and image.

### Fixed
- Shaders using `WeatherMap3D.get_wind_shader_code()` compile without a WeatherMap3D in the scene (the wind globals are declared as project settings), and `weather_wind()` returns no wind there.

## build-950721c5 (2026-11-10)

### Added
- Weather: wind near the ground. WeatherMap3D shelters the wind inside plant canopies (Cionco's profile; `add_canopy()`, `canopy_map`) and in the lee of obstacles (`add_shelter_box()`, the `weather_shelter` group), and carries gust patches of a few metres across fields (`gust_patch_size`, `gust_patch_strength`); `get_wind_shader_code()` gives shaders the same wind as `get_wind_at()` (#212)
- `MultiMesh.bones_per_instance`: consecutive records skin one instance (instanced skinning with a `use_particle_trails` material), in Forward+, Mobile and the path tracer, for wind on many plants without a Skeleton3D each.

### Fixed
- Particle trails under path tracing: only the first trail was captured.

### Changed
- README: wind near the ground line; canopy and shelter wind papers in docs/REFERENCES.md.
- README: Vegetation instance bones line and image.

## build-309945bb (2026-11-10)

### Added
- Water: plunging breakers throw a lip of water along a ballistic path off the crest (Thurey et al. 2007) and spray where it is thin and where it lands (Ihmsen et al. 2012); `shore_spray`, `shore_spray_amount`, `get_breaker_stats()`
- `RenderingServer.multimesh_mark_buffer_rd_changed()`: MultiMesh records written by a compute shader now reach the AABB, motion vectors and the path tracer (merged at once, per-instance records after an asynchronous readback).

### Fixed
- Path tracing: a malformed format string in the material buffer warning
- Path tracing: a MultiMesh under the merge cap kept its first records after `multimesh_set_buffer()` or `multimesh_instance_set_*()` when motion vectors were off.

### Changed
- README: hair body collision line and image.
- README: fresh hair wind clip with sharp raster strands.
- README: plunging breakers line and image; breaking-wave papers in docs/REFERENCES.md.
- README: Vegetation section, GPU-placed instances.

## build-16937fed (2026-11-10)

### Added
- PhysXHair3D `collision_body`: strands collide with a body mesh (skinned or not) through a signed distance field built on the GPU every frame, as AMD TressFX; `collision_cell_size` sets its resolution

### Changed
- README: hair distance LOD line; raster hair images replaced with sharp strands.

## build-b95bf5f6 (2026-11-10)

### Added
- PhysXHair3D distance LOD (`lod_enabled`, `lod_start_distance`, `lod_end_distance`, `lod_strand_ratio`): far away the rasterizer draws a random share of the strands, widened to keep the hair's opacity, as TressFX; simulation and path tracing keep every strand

### Fixed
- Raster strand hair measured its pixel size with the wrong sign and drew every strand as a 2 cm ribbon: strands are now sharp and much cheaper to draw

## build-eb40e5d1 (2026-11-10)

### Fixed
- Path tracing works again: the light buffer declarations lost their semicolons in the cloud shadow change, so the path tracer's shaders failed to compile and every view fell back to raster.

### Changed
- README: raster hair shading

## build-62ff210c (2026-11-10)

### Added
- PhysXHair3D `hair_shading` (on by default): raster strands are lit with the Marschner hair model as Unreal Engine evaluates it, with multiple scattering, and let part of the light through in shadow maps, so the side away from the light is lit through the hair instead of falling black

### Changed
- README: weather clouds, cloud shadows, sun projector; weather sources in docs/REFERENCES.md

## build-b08716a5 (2026-11-10)

### Added
- DirectionalLight3D projector: `light_projector` now works on directional lights, projected along the light over `projector_size` and `projector_offset` metres, in raster (Forward+, Mobile) and in path traced sun light alike (#207).
- Weather clouds: `WeatherMap3D` cloud layer (`cloud_base_height`, `cloud_thickness`, `cloud_density`, `cloud_shape_scale`) shaped by Nubis-style 3D noise from the map's cover, type and storms, with cloud shadows baked onto `sun_light` in the background, and `get_cloud_density_at()` and `get_sun_visibility_at()` (#207).
- `WeatherSkyMaterial`: the physical sky with raymarched clouds (energy-conserving steps, two-lobe phase, Wrenninge octaves, two-stream diffuse light) drawn at half resolution, seen by path traced camera and bounce rays (#207).

## build-b28cdd3d (2026-11-10)

### Changed
- README: Weather section

### Added
- WeatherMap3D (new weather module): one weather state across the world, following the camera. Per cell: mean wind on the log law with Kaimal gusts (IEC 61400-1, frozen turbulence), cloud cover and type, rain rate, and the water rain leaves on surfaces (a film, then puddles that drain and dry by Penman evaporation). Storms drift with the wind aloft and bring a rain core and a downburst outflow. Published to every shader as global uniforms (weather_wind_map, weather_state_map, weather_map_rect, weather_gusts), raster and path traced (#206)
- PhysXWaterSurface3D follow_weather: the sea takes its wind from the weather map, building up over the Shore Protection Manual's growth time for its fetch (#206)
- PhysXCloth3D and PhysXHair3D weather_wind: cloth and hair move in the weather map's wind and gusts (#206)

## build-e70d586b (2026-11-10)

### Changed
- README: sorted raster strands
- README: shore run-up, curling breakers, whitecaps, mesh detail
- PhysXWaterSurface3D: the swash runs each breaking shore wave up the sand, as far as the Stockdon run-up for the slope; shore wave crests are sharper and troughs flatter as they break (#205)
- PhysXWaterSurface3D: whitecaps also where the wave cascades add up (Jacobian of the summed waves); the water material reads the cascades once per pixel for normals and foam (about 25% less fragment work), and the GPU profiler labels the water passes (#205)

### Added
- PhysXWaterSurface3D shore_wave_curl: plunging breakers curl over on beaches whose slope makes them plunge (surf similarity number), and mesh_detail for a finer water mesh (#205)

### Fixed
- PhysXWaterSurface3D: with step_mode per frame the swash now follows the shore waves (#205)

## build-573849e9 (2026-11-10)

### Changed
- Raster strand hair is alpha blended (TressFX coverage) and sorted far to near on the GPU every frame, so it is smooth without TAA or MSAA; shadow maps keep the coverage as a share of texels
- README: raster strands, IN_PATH_TRACER

## build-56f1940b (2026-11-10)

### Added
- Strand hair in raster drawn as AMD TressFX does: camera-facing ribbons widened to at least about 0.7 px with coverage opacity (hashed; smooth with TAA or MSAA), and the same coverage in shadow maps so hair shadows itself; path tracing keeps real-width strands
- IN_PATH_TRACER spatial shader built-in: true while the path tracer captures or shades the geometry, false in raster

### Changed
- README: Hair section (shading, grooms, TressFX simulation, shader outputs); TressFX in References

## build-4975e792 (2026-11-09)

### Changed
- PhysXHair3D's solver is now a port of AMD TressFX 4.1's (MIT): two pinned root points, damped Verlet toward the air's velocity, global shape over a range, velocity shock propagation, iterated local shape and length constraints, collisions that stop the point, speed clamp. New properties global_stiffness, global_range, local_stiffness, local_iterations, length_iterations, vsp_coeff, vsp_accel_threshold, max_speed (TressFX defaults); root_follow, tip_follow, follow_range, shape_stiffness, length_damping, air_drag, shape_damping, inertia_scale and inertia_limit removed

## build-fa54fb14 (2026-11-09)

### Changed
- PhysXHair3D moves like hair: air drag from fibre physics with wind in m/s and carried turbulence, guide strands with follow strands (moves in clumps, simulates a tenth), inertia scale and limit, damped shape spring

## build-cd4f28ce (2026-11-09)

### Changed
- PhysXHair3D: the pull toward the groomed pose holds only the strand's first follow_range (0.3) from the root, so hair swings and blows freely below the scalp

## build-bdcf0b67 (2026-11-09)

### Fixed
- Path tracing: custom shaders' render modes hair_strands, hair_cards and sss_mode_skin now reach the path tracer (strands shaded as a dense shell, near black)

## build-eded33cb (2026-11-09)

### Added
- Moving strand hair: PhysXHair3D simulates a HairGroom's strands on the GPU (follow-the-leader with shape constraints, wind, collision spheres) and draws them as path traced hair fibres; shaders can shade as hair with HAIR, HAIR_ABSORPTION and HAIR_ROUGHNESS
- PhysXWaterSurface3D shore_waves: swell that turns with the seabed, grows in the shallows and breaks on the beach as white bores (shore_wave_height, shore_wave_period, shore_wave_direction) (#197)
- PhysXRiver3D living waterline: the river edge laps, is ragged and carries a thin foam band (edge_lap_height, edge_relief, edge_foam) (#197)

### Changed
- README: moving strand hair
- README: shore waves, living waterline

## build-09874cea (2026-11-09)

### Added
- Strand hair: HairGroom (strands grown on a scalp mesh, or read from a HAIR file) drawn by HairMesh as crossed strips; BaseMaterial3D Hair direction Strands, where the path tracer shades each strand from where on the fibre the ray lands (#196)

### Changed
- README: ocean swell
- README: strand hair

## build-56219800 (2026-11-09)

### Added
- PhysXWaterSurface3D wave_cascades and wave_cascade_ratio: up to four FFT tiles of growing size, each with its own band of the spectrum, so open water gets tall swell under the chop without visible tiling (#195)

### Changed
- PhysXWaterSurface3D's wave spectrum is now TMA (JONSWAP in finite depth): shallow `depth` takes energy out of the long waves (#195)

## build-995d5696 (2026-11-09)

### Changed
- README: waterfalls; rivers as a sub-list

### Added
- PhysXRiver3D waterfalls: a drop of at least fall_min_height that the water can clear becomes a free fall from the lip, as a falling sheet that thins, whitens and breaks up as it falls, with a boiling plunge pool, spray (fall_spray) and mist on the scene's Flow (fall_mist); get_waterfalls() (#194)

## build-60c96101 (2026-11-09)

### Changed
- README: river joins

### Added
- PhysXRiver3D mouth_target: rivers join lakes, the sea and other rivers. The river ponds back to the water's level and fades into it, its outflow spreads into a lake or the sea as a plume (PhysXWaterSurface3D.set_inflow), and a tributary's discharge adds to the main river below the join (#193)

## build-b69095d3 (2026-11-09)

### Added
- BaseMaterial3D Hair: hair fibre scattering in the path tracer (Chiang et al. 2016: white highlight, light through the fibres, coloured second highlight), colour from the albedo or from melanin, a mask for characters whose hair shares one material, strands running from a crown point or along the UVs, shells or cards; an anisotropic highlight across the strands in raster; setting rendering/pathtracing/hair (#191)
- PhysXRiver3D live window: the sea's shallow-water solver runs on a square round the camera, fed the river's depth and flow, so rocks shed eddies and a meandering wake with live foam and floaters swirl in them (live_flow) (#192)

### Changed
- README: hair line and image
- README: river live flow

## build-8c6c8958 (2026-11-09)

### Changed
- README: rivers round rocks
- README: subsurface scattering line and image
- README: terrain deformation line

### Added
- PhysXRiver3D: the current parts round rocks (a diffusive-wave stream function solved over the river), with white water on each rock and trailing behind it; obstacle_collision_mask finds rocks for carving rivers (#190)
- Path tracing: subsurface scattering as a random walk inside the mesh (Cycles' method), for BaseMaterial3D subsurface scattering and `SSS_STRENGTH`; radius set by `rendering/pathtracing/subsurface_scattering_scale`, toggle `rendering/pathtracing/subsurface_scattering` (#189).
- `TerrainDeformer3D`: non-destructive height brushes over a heightfield ground (Unreal-style edit layers). Keeps and saves the base heights under every brush, composites the stack in order (lower, raise, flatten, add, blended by each brush's mask) and writes only changed vertices, so moving or removing a brush restores the ground. Grounds: a Terrain3D node, called by name and never bundled (edited regions re-uploaded and saved, collision rebuilt), or a `HeightMapShape3D` with an optional mesh rebuilt from the same heights. `base_height_at()` gives brushes such as `PhysXRiver3D` the uncarved ground.
- `TerrainBrush3D`: circle or rectangle brush, flat or bowl, with a soft edge, for craters, plots and road beds.

### Fixed
- `test_render_raytracing_capture.cpp` compiles with GCC (a doctest `CHECK` on a bitwise and).

## build-bae7c3f5 (2026-11-09)

### Changed
- README: river carve option
- README: trimmed; water and material media in one collapsed block each (#188)
- README: camera under water line, image and clip (#186)

### Added
- PhysXRiver3D carve: the river designs its own channel (cut through rises, deeper on the outside of bends) and offers it as height brushes for a terrain deformer, reading the uncarved ground from base_height_source (#187)

## build-65a22988 (2026-11-09)

### Changed
- README: rivers line and image (#184)
- Path tracing: camera rays start at the near plane, as raster clips, and a ray that misses everything inside clear water is absorbed (#185).

### Added
- PhysXRiver3D: a river drawn as a path, sitting on the measured ground at Manning's depth for its discharge, never running uphill, falling as cascades, fastest over its deep middle, with white water below falls and rapids; PhysXBuoyancy3D floats bodies on it (#183)
- Camera under water: the path tracer starts each pixel's camera ray in the medium found above its near plane point, so the water fogs, scatters and bends the view up through the surface; `PhysXWaterSurface3D.underwater_enabled` does the same in raster (depth fog with the path traced medium, the image split at the water line, Snell's window and total internal reflection from below) (#185).

## build-79507dea (2026-11-09)

### Fixed
- Path tracing: shaders that move vertices in vertex() get their varyings, UV and COLOR from the rest pose at a hit, as in raster, instead of from the already moved hit point (#169, #182).

### Removed
- PhysXWaterSurface3D: the path-traced shader no longer inverts its own wave displacement; the engine now gives it the rest pose (#182).

### Changed
- README: white water line and clip (#180)

## build-60ea3312 (2026-11-09)

### Added
- PhysXWaterSurface3D flow_foam: the live flow makes white water where it shears (lines trailing rocks, eddies) and runs supercritical (chutes, rapids), carried downstream and fading over flow_foam_persistence (#179)

### Changed
- README: live flow line and clip (#178)

## build-80024163 (2026-11-09)

### Added
- PhysXWaterSurface3D live_flow: the simulated square solves the shallow-water equations on top of the baked current, so rocks and banks shed wakes and eddies that the surface, sample_flow() and floaters follow; wall_friction sets how readily they form (#177)

## build-22be36b8 (2026-11-09)

### Added
- Shader outputs SCATTERING and SCATTERING_ANISOTROPY: a shader-defined medium (IOR + ABSORPTION) can scatter light under path tracing; PhysXWaterSurface3D uses them so deep water reads blue (#172)
- PhysXWaterSurface3D current: water flows round rocks and along the shore (solved from the seabed), waves and foam are carried with it, sample_flow() returns it, and PhysXBuoyancy3D drags floaters downstream (#175)

### Fixed
- Path-traced caustics fade out at the edge of their area instead of drawing a dark line on the seabed (#172)

### Changed
- README: currents line and clip (#176)
- README: shader SCATTERING and deep-water scattering in the Water lines (#173)
- README: path-traced ocean line and image (#171)

## build-3620d951 (2026-11-09)

### Added
- Shader outputs SCATTERING and SCATTERING_ANISOTROPY: a shader-defined medium (IOR + ABSORPTION) can scatter light under path tracing; PhysXWaterSurface3D uses them so deep water reads blue (#172)
- PhysXWaterSurface3D current: water flows round rocks and along the shore (solved from the seabed), waves and foam are carried with it, sample_flow() returns it, and PhysXBuoyancy3D drags floaters downstream (#175)

### Fixed
- Path-traced caustics fade out at the edge of their area instead of drawing a dark line on the seabed (#172)

### Changed
- README: shader SCATTERING and deep-water scattering in the Water lines (#173)
- README: path-traced ocean line and image (#171)

## build-458afd8b (2026-11-09)

### Added
- PhysXWaterSurface3D is real water under path tracing: light bends through the waves, the water tints with depth and sun caustics land on the seabed; raster look unchanged (#170)

### Fixed
- Path tracing takes a shader's unshaded and fog_disabled render modes from its path-traced variant (#170)

### Changed
- README: caustics line and image (#167)

## build-7eae6464 (2026-11-09)

### Added
- Water caustics: the path tracer gathers sunlight through rippled water into bright moving lines on the floor below, from normal maps and shader normals alike, with a photon map from the first directional light around the point the camera looks at. Settings `rendering/pathtracing/caustics` and `rendering/pathtracing/caustics_area` (#166)

### Changed
- README: call the fork Berlenga throughout, links to the renamed repo, title icon, independence note (#165)
- README: murky water line and image (#164)

## build-9d094a32 (2026-11-09)

### Changed
- The fork is named Berlenga in the window title, About box and startup header; the executables, data folders and Vulkan engine name stay Godot's.
- The editor and Project Manager use the Berlenga icon (window, Windows executable, macOS bundle, Linux desktop entry); exported projects keep the neutral default icon.
- README: the fork is named Berlenga (#162)
- README: water gets its own section (#161)
- README: refraction line and water image (#160)

### Added
- Murky water: Transmission media scatter light with `transmission_scatter_strength`, `transmission_scatter_color` and `transmission_scatter_anisotropy` (glTF draft `KHR_materials_scatter`, volumetric mode, imported and exported). The path tracer draws collisions inside the medium and lights each from every light; the rasterizer shows the scattered colour in the share the depth hides. Shaders bound a medium with the new path-traced outputs `IOR` and `ABSORPTION` (#163)

## build-50273c52 (2026-11-09)

### Added
- Transmission bounds a medium when `transmission_thin_walled` is off: `transmission_ior`, `transmission_attenuation_color` and `transmission_attenuation_distance` (glTF `KHR_materials_volume` and `KHR_materials_ior`, imported and exported). The path tracer refracts rays into and out of it with total internal reflection and absorbs every ray inside it (Beer-Lambert), shadow rays included, so water darkens and tints with depth; the rasterizer refracts the screen read by the IOR and tints it by the depth behind. Setting `rendering/pathtracing/refraction` (#159)
- Design note on refraction and a water medium in the path tracer (#158).

## build-40b63b1b (2026-11-09)

### Added
- Path tracing reads the clearcoat, back light and transmission textures, and shaders can write `TRANSMISSION`; shaders writing `TRANSMISSION` or `BACKLIGHT` pass light into their shadows (#155).

### Fixed
- Path-traced shadows of Transmission and Refraction materials let the tinted light through instead of blocking it (#155).

### Changed
- README: layer textures and shader TRANSMISSION in the material lines, with an image (#156)
- README: summary section and transmission, back lighting and clearcoat images (#154)
- README: transmission image retaken with glass shadows that pass light (#157)

## build-effa664d (2026-11-09)

### Added
- `PhysXCloth3D.tear_factor`: cloth tears by itself where it is stretched past this multiple of its rest length, splitting vertices so the rip runs across it; `torn` signal, texture coordinates on generated grids and source meshes, project setting `physics/physx_3d/cloth/tearing`.
- Design note on clearcoat and thin transmission in the path tracer (#150).
- Path-traced back lighting: BaseMaterial3D back light, bounce light through the surface, `rendering/pathtracing/back_lighting` (#152).
- Path-traced clearcoat matching the rasterizer, `rendering/pathtracing/clearcoat` (#151).
- Materials test scene, `docs/bench/materials` (#151).
- BaseMaterial3D Transmission section: thin glass in raster (screen read) and the path tracer, tinted shadows, glTF `KHR_materials_transmission`, `rendering/pathtracing/transmission` (#153).
- Path tracer: Refraction-enabled materials pass `1 - alpha` of the light through (#153).
- Path tracer: back-lit surfaces let coloured light into their shadows (#153).

### Fixed
- Path tracer: lights behind a back-lit surface no longer shadowed by the surface itself (#152).

### Changed
- README: the cloth tearing clip is collapsed, so one GIF plays by default (#149).
- README: cloth tearing line and clip in the Physics section (#148).

## build-73a3e160 (2026-11-08)

### Added
- Persistent instances also keep the per-record instances of large MultiMeshes until a record changes (#147).
- The open-world test scene can place its objects as MultiMesh tiles (`--multimesh=M`) and change records each frame (`--mmmove=N`) (#147).

## build-bd7ad4eb (2026-11-08)

### Changed
- The open-world test scene also prints the CPU render and frame setup times (#143).

### Added
- Design note on path tracing instanced forests: MultiMesh records in persistent slots, chunked forests, partitioned TLAS (#145).

### Fixed
- PhysXCloth3D: a generated grid now faces +Z as documented, and cloth normals follow the front face, so two-sided cloth is lit on the side the light falls on in raster and path tracing.

## build-35438dbd (2026-11-08)

### Changed
- Path tracer: motion vectors age out only on objects that moved, instead of visiting every listed object each frame (#139).
- Path tracer: the per-object check of persistent instances runs on the worker threads (#140).
- README: an Open worlds section with the large-world settings, a ready-to-paste project.godot block and the open-world test scene (#141).

## build-28640b78 (2026-11-08)

### Fixed
- Path tracer: `rt_vertex_dynamic` surfaces are recaptured every frame. PhysX skinned cloth no longer traces as a stale first capture (a coat left crumpled, or missing, while the raster image is right) (#138).

### Changed
- README: opacity micromaps listed under Large worlds (#137).

## build-61af2b42 (2026-11-08)

### Added
- Path tracer: opacity micromaps for alpha-scissor StandardMaterial3D surfaces (Vulkan `VK_EXT_opacity_micromap`), baked on the GPU on first trace so rays skip cut-out texels and accept solid ones without the any-hit shader; same image, opt-in with `rendering/pathtracing/opacity_micromaps` (off by default).

## build-e7f15277 (2026-11-08)

### Changed
- Path tracer: with persistent instances, the TLASes keep their instances on the GPU and each build writes only the changed ones (#135).
- Path tracer: persistent instances prefetch the instances and their slots ahead of the per-frame check, about a third less time in it (#136).
- Path tracer: the raster sun shadow casters are no longer culled while path tracing, which never draws those shadow maps (#134).

## build-8cfe788f (2026-11-08)

### Added
- Open-world bench project in `docs/bench/open_world`: rocks, trees and blocks over 2 km for path tracer timing and memory (#130).
- Research note `docs/ideas/persistent-instances.md`: persistent ray tracing instances and partitioned TLAS, from the 4090 measurements (#132).
- Path tracer: persistent instance slots (`rendering/pathtracing/persistent_instances`, off by default): unchanged objects skip the per-frame TLAS work (#133).
- README: PhysX clip (cloth, Blast destruction, rigid bodies, Flow smoke) and its test scene, `--shot=physx` in `docs/media/showcase` (#PR).
- Path tracer: simulated gas. The PhysX Flow volume nearest the camera is traced as a participating medium (delta tracking, ratio-tracked shadows, Henyey-Greenstein phase, temperature emission). Setting `rendering/pathtracing/simulated_gas` (#128).
- PhysXCloth3D.release_pins(): lets pinned vertices go without resetting the cloth, so an edge can tear free (#128).
- PhysXCloth3D.tear(): cuts the cloth along a line so it rips apart (CPU solver) (#128).
- PhysXFlowEmitter3D: an emitter with no smoke, fuel, burn or heat only pushes air, for wind on the gas; it no longer cools the gas it passes (#128).

### Changed
- README: about a third shorter; open-world summary folded into the feature list, one line per feature, shorter comparison captions (#129).
- README: trimmed to one short line per feature with its setting; papers moved to `docs/REFERENCES.md` (#131).

## build-384f62e7 (2026-11-08)

### Added
- Local exposure from a bilateral grid (`rendering/camera/auto_exposure/local_highlight_contrast`, `local_shadow_contrast`), off by default (#127).

## build-e071b5eb (2026-11-08)

### Added
- Histogram auto exposure metering (`rendering/camera/auto_exposure/metering`) and a dark-scene key (`rendering/camera/auto_exposure/dark_scene_luminance`), both off by default (#126).

### Fixed
- Auto exposure overshooting when a frame is longer than the adaptation time (#126).

## build-f7c2227f (2026-11-08)

### Added
- `rendering/pathtracing/cull_max_distance` and `rendering/pathtracing/cull_min_size`: optional distance and size limits for geometry outside the camera view in the path tracer (#125).

## build-ccfb311a (2026-11-08)

### Added
- Research note `docs/ideas/exposure.md`: auto exposure for dark scenes under path tracing (histogram metering, luminance-dependent key, exposure compensation curve, local exposure) and what the fork should take (#116).
- Project setting `rendering/pathtracing/emissive_lights` (default on) to turn off sampling emissive surfaces as lights (#115).
- Research note docs/ideas/open-world-geometry.md: RT LOD, Mega Geometry, partitioned TLAS and streaming, and the order to build them (#123).
- PhysX 5 physics engine (Windows x64, Linux x86_64), selectable per project next to Jolt, with cloth, skinned cloth, Blast destruction, Flow smoke and fire, ocean water and particle fluids; cloth, destruction, smoke and water also work with Jolt (#113).
- README: on and off comparisons for emissive lights, light visibility memory and the radiance cache, with their test scenes in `docs/media/light-selection` (#118).
- `rendering/pathtracing/mesh_lod`: static meshes trace at the LOD the rasterizer draws them at, cutting acceleration-structure memory for far geometry (#124).

### Changed
- Changelog entries are files in `changelog.d/`, one per pull request, gathered by the release, so a release made while a pull request is open no longer misplaces its entry; lines under Unreleased in CHANGELOG.md still work. The release's changelog commit retries when main moves meanwhile.
- COPYRIGHT.txt: notices for V-HACD and Boost, which ship inside the Blast authoring library in Windows and Linux x86_64 builds (#117).
- README: a note on the images: raw path tracer output, almost all CPU-rendered, without a denoiser or Ray Reconstruction (#122).
- README: what the fork does for open-world games and what comes next for them (#121).
- README: comparison captions explain what to look at, the emissive lights pair is a brightened close-up, and the many lights section shows its image (#120).

### Fixed
- Path tracing stays exact far from the world origin: it traces relative to the camera's cell on a 1024 m grid (`rendering/pathtracing/relative_origin_grid`, 0 = absolute), and ray origins clear surfaces by a few float steps of the largest coordinate, so shadows, reflections and contact points no longer break up kilometres out (#119).

## build-350c2073 (2026-11-08)

### Added
- Research note `docs/ideas/exposure.md`: auto exposure for dark scenes under path tracing (histogram metering, luminance-dependent key, exposure compensation curve, local exposure) and what the fork should take (#116).
- Project setting `rendering/pathtracing/emissive_lights` (default on) to turn off sampling emissive surfaces as lights (#115).
- PhysX 5 physics engine (Windows x64, Linux x86_64), selectable per project next to Jolt, with cloth, skinned cloth, Blast destruction, Flow smoke and fire, ocean water and particle fluids; cloth, destruction, smoke and water also work with Jolt (#113).
- README: on and off comparisons for emissive lights, light visibility memory and the radiance cache, with their test scenes in `docs/media/light-selection` (#118).

### Changed
- Changelog entries are files in `changelog.d/`, one per pull request, gathered by the release, so a release made while a pull request is open no longer misplaces its entry; lines under Unreleased in CHANGELOG.md still work. The release's changelog commit retries when main moves meanwhile.
- COPYRIGHT.txt: notices for V-HACD and Boost, which ship inside the Blast authoring library in Windows and Linux x86_64 builds (#117).

### Fixed
- Path tracing stays exact far from the world origin: it traces relative to the camera's cell on a 1024 m grid (`rendering/pathtracing/relative_origin_grid`, 0 = absolute), and ray origins clear surfaces by a few float steps of the largest coordinate, so shadows, reflections and contact points no longer break up kilometres out (#119).

## build-30bb7182 (2026-11-08)

### Added
- README: texture filtering comparison re-rendered on an RTX 4090 with the anisotropic filtering fix (#108).

### Changed
- Path tracer: the light visibility memory learns from about 65536 pixels a frame (at most one in four) instead of all of them. On an RTX 4090 at 1080p it cost 8 to 12 ms of a 17 ms frame, mostly contended compare-and-swap updates; quality at 480x270 is unchanged (26.05 vs 26.01 dB) (#114).

## build-5e22cf31 (2026-11-08)

### Changed
- README: DLSS Ray Reconstruction checked with the DLSS 4.5 DLLs (310.5) on an RTX 4090 (#112).

### Fixed
- Path tracer: color backgrounds (Background mode Custom Color or Clear Color) show behind the scene as in raster; camera rays that missed read the empty radiance map and came out black (#111).
- Release workflow: only commits on main are published as releases; a run on any other branch builds the zips as workflow artifacts only. build-3c75a9b9 was a test build of an unmerged branch published by mistake.

## build-3c75a9b9 (2026-11-08)

Test build of an unmerged branch, published by mistake and deleted. It was not a build of main.

## build-5437261e (2026-11-08)

### Added
- Path tracer: radiance cache. Rough surfaces learn, in hashed world-space cells sized by distance from the camera, the light they send back (own lighting and further bounces); a path that bounces onto one reads the cell and ends. One pixel in eight trains it each frame. Test streets at 4 bounces, equal render time: 25.4 to 27.1 dB and 20.1 to 21.1 dB. Off under environment fog. Setting `rendering/pathtracing/radiance_cache` (#110).
- README: side-by-side comparisons with NVIDIA-RTX/godot (many lights, glass shadows, texture filtering) and the test scenes that render them, in `docs/media` (#106).
- Path tracer: emissive surfaces as lights. Opaque StandardMaterial3D surfaces that emit (up to 1024 a frame, the strongest at the camera) get a light sample per shaded point: one picked by estimated power, then a triangle and a point on it, with the emission texture read there; bounce rays that hit them are weighed by multiple importance sampling (power heuristic). Sides follow the material's culling. Test street lit by signs at 4 spp: 21.2 to 24.0 dB, brightness within 0.1% of before at 1024 spp (#105).
- Path tracer: light visibility memory. Each part of the scene remembers which lights lit it lately; light selection draws half its candidates from those and weights the rest by how often they were blocked, so hidden lamps and a roofed-over sun stop taking samples. Test city street at 4 spp: 19.0 to 25.3 dB (direct light alone 21.8 to 33.0 dB); open street 17.8 to 20.1 dB. Off: `rendering/pathtracing/light_visibility_memory` (#104).
- Research note `docs/ideas/physx.md`: research pass (Arkham PhysX/APEX, Unreal Chaos destruction and cloth, heterogeneous volumes, volume path tracing papers, DLSS RR and volumes), which module features work with Jolt, and the design for path-traced smoke, cloth and props (#103).
- Research note `docs/ideas/witcher3-remaster.md`: what The Witcher 3 Remastered's path tracer suggests for the fork (ReSTIR reuse for many lights, DLSS 4.5 RR, hair fallback, exposure in dark scenes, streaming ahead of the camera) (#99).
- Research note `docs/ideas/other-engines.md`: what Unreal's MegaLights, Assassin's Creed Shadows, Doom: The Dark Ages, Alan Wake 2 and Star Wars Outlaws suggest for the fork (visibility-aware light selection, camera-following radiance cache, open-world BLAS streaming, foliage opacity test) (#100).
- Research note `docs/ideas/water.md`: rivers, waterfalls, lakes and ocean in Crimson Desert, Unreal's Water plugin, Fluid Flux and Horizon Forbidden West, and what the fork could take (flow-mapped spline rivers, FFT ocean, water medium in the path tracer, caustics) (#101).
- Research note `docs/ideas/weather.md`: clouds (Nubis), sky (Hillaire), wind, wetness, snow, fog and lightning for an open world, and how each fits the path tracer (#102).

### Changed
- Path tracer: the light grid follows the camera (256 m each way), so lights spread over a large world no longer coarsen its cells; lights reaching past it still light points outside it. A street of 460 lamps with 2000 more over 10 km: 11.7 dB to 17.8 dB at 4 spp, same as the street alone (#98).

### Fixed
- Path tracer: materials set to an anisotropic texture filter are now filtered anisotropically, and nearest-filter materials point sampled, as in raster. The filter never reached the path tracer, so grazing floors blurred in the middle distance (#108).
- Fog, sky, particle and texture blit shaders no longer fail to compile when two of them compile at once on the main and rendering threads (separate rendering thread) (#96).

## build-a3c71eda (2026-11-06)

### Added
- Research notes: one short attribution rule; outside projects are credited in the README References (#92).
- Research note `docs/ideas/physx.md`: PhysX 5 physics server module (uno1982/godot) assessed for later: features, licences, GPU sync cost under path tracing, Flow gas not traced, platforms, CI cost (#91).
- Research note `docs/ideas/valence-engine-rtx-takeaways.md`: RTX 4090 extension check (no linear swept spheres; cluster AS, partitioned TLAS and opacity micromaps present) (#90).
- Research note `docs/ideas/godot-rtx-forks-survey.md`: what other Godot RTX forks add and what is worth taking, with attribution (#89).
- Research note `docs/ideas/valence-engine-rtx-takeaways.md`: general engine fixes worth taking (index variants, pipeline precompile, blend shapes, Jolt cast_motion, render graph barriers) and how anything taken from it is attributed (#88).
- Research note `docs/ideas/valence-engine-rtx-takeaways.md`: open-world additions (strand hair, subdivision, sky sampling, water medium, subsurface) (#87).
- Research note `docs/ideas/valence-engine-rtx-takeaways.md`: how valence-engine-rtx implements light trees, emissive lights, material lobes, opacity micromaps, cluster LOD and partitioned TLAS, with the measured traps, for later reimplementation (#86).
- Research note `docs/ideas/valence-engine-rtx.md`: ranking of its features for a large open world (cluster LOD, partitioned TLAS, streaming, light tree) (#85).
- Research note `docs/ideas/valence-engine-rtx.md`: assessment of another NVIDIA path tracer fork and which of its features to reimplement here (#84).
- Research note `docs/ideas/metal-path-tracing.md`: assessment of a Metal (macOS) path tracing port and how it would fit this fork (#83).
- Path tracer: a new pipeline variant (path tracing turned on, a samples, bounces or denoiser change) builds on a worker thread; meanwhile the view keeps tracing with the previous variant, or draws raster for those frames, instead of stalling.
- README: NVIDIA-RTX/godot credited in the References.
- Path tracer: compiled shader stages are cached on disk and shared between settings that differ only in samples or bounces, so a second launch or a samples change skips the shader compile.
- Release workflow: a `build_only` run builds any branch and keeps the zips as workflow artifacts, without publishing a release.

### Changed
- Rebased on NVIDIA-RTX/godot of 2 November 2026: path traced transparency (on by default, `rendering/pathtracing/max_transparency_layers`), traced particles, decals and volumetric fog shadows, shadow groups, dynamic BLAS budgets, asynchronous pipeline compilation (`get_rendering_not_ready_reasons()`). Where NVIDIA now covers a fork change (orthographic rays, triplanar, UV2 for custom shaders, MultiMesh motion, BLAS lifetime, traced fog) the fork uses NVIDIA's.
- Kept on top: the 4096-light grid with resampled light selection, ray-cone texture filtering, emission in the Ray Reconstruction guide, frame generation fixes, device fault reports, storage image formats. With `max_transparency_layers` 0 transparents use the fork's raster layer over Ray Reconstruction with the traced light cache, while reflections and refractions still trace them.

### Fixed
- Path tracer: transparent surfaces filter shadow rays by their coverage instead of a random coin flip, so clear glass casts an even, light shadow without blotches (#94).
- DLSS: resources are tagged for the frame being rendered, so Ray Reconstruction, Super Resolution and NIS work with the separate rendering thread.
- DLSS: the render size clamp leaves a size that a Ray Reconstruction mode accepts, so a driver override that pins Super Resolution no longer shrinks a path-traced view; a failed Ray Reconstruction evaluate names the inputs it was given.
- Release retry with immutable releases: while a build waits for a runner the release stays a draft, and is published once the retry has added its zip (or after the third attempt) (#82).
- Release retry: its job had five minutes, all spent waiting before the re-run (#81).

## build-3b0494b5 (2026-11-05)

### Fixed
- A release build that GitHub found no runner for (macOS when hosted capacity is short) is retried automatically, and the re-run adds its zip to the existing release instead of failing (#80).

### Changed
- Release notes list the downloads in a table: one row per build with its link and the zip's SHA-256; the executables' hashes are in a collapsed section (#80).

## build-ad774282 (2026-11-05)

### Added
- Path tracer: particles show in reflections and refractions (rain in puddles and glass), lit from the light cache; `rendering/pathtracing/transparent_lighting/particles_in_reflections` and `particle_reflection_distance` (#79).

### Fixed
- CodeQL scans run to completion: a push to `main` waits behind a running scan instead of cancelling it (#77).

## build-2d41bc16 (2026-11-05)

### Added
- Path tracer: particles lit by a forward-scattering phase function from the light cache, so rain glints in front of lamps and headlights; `rendering/pathtracing/transparent_lighting/particle_anisotropy` (#76).
- Path tracer lights particles and transparents: a traced light cache with shadowed lights, sky and bounce replaces the raster lights and ambient for them; `rendering/pathtracing/transparent_lighting/*` settings (#74).
- CHANGELOG.md: release notes come from its Unreleased entries, and pull requests are checked for one (#73).

## build-57db5e08 (2026-11-05)

### Fixed
- Linux arm64 release build used an x86_64 Python (#71).

## build-c28b7788 (2026-11-05)

### Added
- Linux (x86_64, arm64) and macOS (universal, raster only) editor builds in each release (#66, #67).
- CodeQL scan of the C++ sources (#67).

### Changed
- Releases are built on GitHub Actions: on demand, and every Monday when `main` has moved (#65).
- CI actions pinned by commit to their latest releases, least-privilege tokens, Dependabot for action updates (#68).

### Fixed
- Path tracer: storage images declared with their textures' formats (#70).

### Removed
- Upstream's CI workflows and unused composite actions (#67).

## build-c7e111a0 (2026-11-05)

### Fixed
- Crash when resizing the window: the screen pass drew past the swap chain image (#64).

## fixes-85b5084a (2026-11-05)

First release.

### Added
- `RenderingDevice.get_video_memory_budget()` and `get_video_memory_usage()` (#62).
- `RenderingServer.is_pathtracing_ready()`; path tracer pipeline variants compile on a worker with a SPIR-V disk cache (#59).

### Changed
- Path tracer samples textures with raster's mipmap bias and ray-cone gradients (#55).
- Path tracing with Ray Reconstruction draws `blend_mul` transparents over the output (#56).

### Fixed
- Editor help no longer runs doc work after the editor is freed (#60).
- Vulkan device fault reports kept when the driver returns VK_INCOMPLETE (#52); deviceFault feature enabled before use (#40).
- WorkerThreadPool releases task Callables before scripting languages finish (#48, #50).
- ClassDB default property value cache guarded for worker threads (#46).
- PrimitiveMesh builds surface arrays on the CPU instead of reading them back (#44).
- Path tracer gives custom shaders the mesh's UV2 (#42).
