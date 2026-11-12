# Changelog

What changed in this fork, newest first. Upstream Godot's own changes are in its release notes.

Every pull request adds its entry as a file in `changelog.d/` (see [changelog.d/README.md](changelog.d/README.md)), in one of: Added, Changed, Fixed, Removed. Write what changes for someone using the engine, in one line, with the PR number. The release workflow publishes those entries as the release notes and moves them here under that release's heading.

## Unreleased

## build-6b31f67b (2026-11-12)

### Changed
- GrassField3D with a `season_source`: a summer drought or hard winter now kills whole blades to the crown, not only their tips.

### Added
- GrassField3D follows the year: `season_source` (a WeatherMap3D) browns the sward in drought and hard cold from the season's soil moisture and temperature (`season_greenness()`, `season_response`, `get_effective_dryness()`), blades, ground and far colour together.
- Snow settles on strand hair: PhysXHair3D's `snow` lies on the top layer, where no hair above shields it and on the side the wind drives it at, read from an extra deep opacity map; it is caught from the weather's snowfall, melts above freezing and from the head's warmth, and the melt wets the hair (`snow`, `snow_capacity`)
- Bracken and ferns follow the season (`PlantGenerator.season`): croziers unroll in spring, fronds die back russet in autumn and lie lodged through winter. Wood anemone dies down after flowering: `get_presence()`, applied by `make_floor()`.
- PlantGenerator flowers by the season: `season` (WeatherMap3D.get_season()) opens each flowering preset's stems through a window in degree days and takes them to seed (dandelion clocks, thistledown, foxglove capsules, wild carrot nests); tall flowers die back to brown stalks that stand through winter. `get_bloom_shares()`; `make_floor()` passes its season on.
- `outwash_age`: gravel reaches below the last ice stand or on fans become braided outwash plains (Murray-Paola cellular braiding on a flat graded plain, bare gravel); `get_braidplains()` gives each plain's centre line, half widths, water stage per point, head discharge and source (`get_last_braid_count()`).
- Ground plants meet fallen wood: `PlantSpecies.deadwood_rule` keeps plants off logs (default), grows them on a log's upper surface (`DEADWOOD_ON`, `deadwood_min_radius`) or along its side (`DEADWOOD_BESIDE`, `deadwood_band`, `deadwood_uphill`), reading the axes of the trees module's logs, fallen branches and sticks; `VegetationField3D.get_deadwood_at()`. `PlantGenerator.make_floor()` adds moss, wood sorrel and seedlings on logs, ferns along them and leaves banked on their uphill side.

### Fixed
- Hair cards no longer read much lighter than the strands they replace: they light with the strands' own shading frame (no sky sheen), sit clear of the scalp and body by their clump's thickness, and read the deep shadows on their centre line
- Hair cards no longer let the head and shoulders show through them: they draw far to near like the strands, keep their opacity at a distance and when seen at an angle, and widen to a pixel when thinner
- Snowfall no longer soaks strand hair as if it were rain: the weather's precipitation is split into rain and snow by the air's temperature and humidity
- Strand hair with deep shadows no longer shows rare black pixel specks
- Rock detail no longer draws moire hairlines at a distance or at grazing angles, and no longer darkens the ground: cracks are box filtered over each pixel's footprint, every other feature fades to its mean, it filters the same under path tracing, and it stays off rock gentler than 45 degrees.
- Falls over 25 m keep the face they fall over, with a notch at the lip, instead of a straight-sided slot and a flat pad at the foot.

## build-d76a902c (2026-11-12)

### Added
- The Linux arm64 editor and export templates include Blast (fracture), built with the system compiler since NVIDIA's Blast tooling has no arm64 packages.
- `PhysXWaterfalls3D`: water that stays on steep rock (cascades, horsetails) from a terrain's waterfall records, clear at the lip and white below the inception point, with spray off ledges and mist along the face. `PhysXRiver3D.fall_exclusions` keeps rivers from drawing those falls again as free jets.
- Forest floor: the ground under closed canopy turns to litter by the stand above (leaves, needles, feather moss), `VegetationField3D.forest_floor_enabled`, `get_floor_shader_code()`; included in `GrassField3D.get_ground_shader_code()`.
- PlantGenerator forest floor presets: needle litter, twigs, wood sorrel, seedling, feather moss, woodruff, wood anemone, ivy, heather.
- `PlantGenerator.make_floor(stand, variants)`: forest floor species for ten stand types (beech, oak mixed, montane conifer, subalpine, pine heath, riparian, hedgerow, northern hardwood, eastern oak, pine-hemlock).
- Volumetric fog takes the cloud shadow projected on the sun, so cloud shafts show in the fog.
- WeatherMap3D `aerosol_haze`, `haze_visibility`, `haze_height`: background haze from visibility and humidity, lit by the volumetric fog.
- `GrassField3D.gpu_fill` (on by default): grass tiles are filled by a compute shader that writes the blades straight into each tile's MultiMesh, seen by the path tracer as before; about 40% less main-thread time per tile, more at higher densities.
- Hair cards for distant strand hair: PhysXHair3D bakes textured cards from its groom (clustered clumps of strands, shared by every node with the same groom) and draws them in raster past `card_distance`, simulating only the guides and the cards (the path tracer keeps the strands) (`cards_enabled`, off by default; `card_count`, `card_distance`)
- Strand hair rests on simulated cloth: PhysXHair3D's `collision_cloth` names a PhysXSkinnedCloth3D whose moving triangles join the hair's collision field each frame, read on the GPU, so hair lies on a collar or hood instead of sinking through it (`cloth_thickness`)
- PhysXHair3D `deep_shadow_local_lights` (default 3): the omni and spot lights that light the hair most get their own deep opacity map (a perspective view from the light), so hair shadows itself smoothly under lamps as under the sun
- Spatial shader fragment output `POSITIONAL_SHADOW_OFFSET`: moves the fragment's omni and spot shadow lookups toward each light by that many metres
- PhysXHair3D `simplify_tolerance` (default 0.5 of the root width): straight runs of a strand are drawn through fewer of its points (Douglas-Peucker on the groom), fewer triangles for the path tracer to refit with no visible change; every point still simulates
- PhysXHair3D strand-strand collision (`strand_collision`, `strand_collision_radius`, `strand_collision_friction`): guide strands no longer pass through each other; a spatial hash finds close guide points once a frame and pushes them apart with friction, the followers come along
- Path tracer: strand hair is traced as linear swept spheres (round tubes, one primitive per segment) on GPUs with `VK_NV_ray_tracing_linear_swept_spheres`, and shaded from where the ray meets the round fibre; other GPUs keep the crossed strips (`rendering/pathtracing/linear_swept_spheres`)
- RenderingDevice: `SUPPORTS_RAYTRACING_LINEAR_SWEPT_SPHERES` and a linear swept spheres BLAS geometry type
- Wet strand hair: rain from the weather map soaks PhysXHair3D hair (less under a canopy, none under a roof) and it dries over time; wet hair is darker, glossier, heavier, hangs flatter and gathers into clumps (`wetness`, `weather_wetting`, `water_capacity`, `drying_time`, `wet_clumping`)
- Landscape3D: ground collision near a target (`collision_enabled`, `collision_radius`, `collision_target`, layer, mask, `physics_material`): one static heightfield per chunk at full detail, built and freed as the target moves (49 chunks in under 2 ms at 1 m, 0.3 ms per 64 m step). PhysX heightfields take a `fan_blocks` key so their cells split like the drawn mesh and bodies rest on the drawn triangles.
- Landscape3D: a heightfield ground drawn as static chunk meshes with an LOD chain picked by projected error (`lod_error`, pixels), neighbours one level apart with stitched edges (no cracks, no skirts). A level change swaps the chunk's mesh, so under path tracing each mesh keeps one BLAS and a moving camera costs no rebuilds or refits. Reads a TerrainGenerator3D (`source`) or a height image.
- TerrainGenerator3D: `changing` signal before a generate replaces its result.
- `BaseMaterial3D.transmission_thickness` (glTF `KHR_materials_volume` `thicknessFactor`, imported and exported): raster refraction through a solid bends the view ray again where it leaves the far side (Filament's solid sphere model) and absorbs over the path inside. `SnowIcicles3D` ice uses 4 cm.
- Generated plants bend away from bodies and spring back through `GrassField3D`'s trample field (no colliders), lean in the wind, and keep their leaf textures, wetness and snow: one shared plant shader. `GrassField3D` publishes `grass_trample_map`, `grass_trample_rect` and `grass_wind` as global uniforms.
- `PlantGenerator.make_climber_leaves(climber, variants)`: single ivy, clematis and honeysuckle leaves (base at the origin, +Z, 1 m) with the plant material, for vines.
- Ground plants thin out by distance one by one, shrinking away in blue-noise rank order (no tile pops), and leave their colour on the ground beyond (`PlantSpecies.far_color`, `vegetation_far()` in the ground shader code).
- `PlantGenerator.PRESET_VIOLET` (common dog-violet) and `PRESET_HERB_ROBERT`: broad-leaved herbs of the lighter forest floor; `make_floor()` places them in beech, oak, hedgerow and (violet) northern hardwood stands, in half shade and gaps.
- `PlantGenerator`: procedural meadow plants (dandelion, plantain, clover, fern, sedge, daisy, buttercup) with seed variations, phyllotaxis and positional functions, double-sided back-lit leaves with generated vein textures (albedo, normal and translucency maps), and distance LODs, for `PlantSpecies.meshes`.
- `PlantGenerator.make_impostor()` and `add_impostors()`: octahedral impostors for large plants past a range, raster only (path tracing keeps the meshes). `make_floor()` gives brambles and bracken impostors past 15 m (`impostor_range`).
- `PlantSpecies.litter_range` (`RULE_LITTER`) and `VegetationField3D.get_litter_at()`: fallen leaves drift past the crowns of broad-leaved trees into forest edges and clearings, thinning over about 6 m; `PlantGenerator.make_floor()` places leaf litter and twigs by it.
- `VegetationField3D.litter_wind`: the leaf drift reaches farther downwind and less upwind, from the weather map's wind by default.
- `PlantGenerator.leaf_wear`: insect bites out of leaf margins, uneven halves, dead tips on grass-like blades, and older, lower leaves yellowing and browning; drawn per leaf, so the plant's shape does not change.
- `PlantSpecies.near_meshes` and `near_range`: heavier meshes drawn near the camera with a dithered crossfade to `meshes` beyond, swapped a few plants at a time.
- `PlantSpecies.spray_range` (`RULE_SPRAY`) and `TerrainDataMap3D.get_spray_at()`: plants keep out of a waterfall's spray zone (the terrain generator's spray map) unless they take it; mosses do.
- `PlantGenerator.litter_leaves` and `stand_litter_leaves(stand, season)`: forest floor leaf litter made of the stand's own fallen leaves (beech, oak lobes, maple palms, ash leaflets, chestnut hands), fresh leaves in their autumn colour among older grey-brown ones. `make_floor()` takes a `season`.
- Forest floor leaf litter amount follows each stand's leaf fall and decay rate and the season; twigs are the class under 1 cm in the stand's bark colour.
- `PlantGenerator.PRESET_KNAPWEED`, `PRESET_SCABIOUS`, `PRESET_WILD_CARROT` and `PRESET_SORREL`: the tall flowers of a hay meadow, standing clear of the grass, about a quarter of each in seed (dark knobs, bristly balls, curled umbels, rust-brown spires).
- `PlantGenerator` wetland presets: common reed, bulrush, soft rush and white water lily. `PlantSpecies.floating` sets plants on the water surface, for floating-leaved plants zoned by `water_depth_range`.
- Generated plants sway in the wind: each frond, cane and stem rocks about its own base at a natural frequency set by its height and stiffness, with its own phase.
- `PhysXWaterfalls3D`: tall free falls (`plunge_falls`). The jet sheet whitens and comes apart after its break-up length (`6 q^0.32` m), then falls as clumps drawn as soft streaks, each slowed by air drag to its own terminal speed, widened by the air's eddies (`column_turbulence`) and carried by the weather's wind (`column_density` sets how many). Spray lands at the speed the drag leaves, and a tall mist veil hangs along the column. `get_plunge_falls()` reports break-up length, landing speed, fall time, column width and wind drift.
- `PhysXWaterMist3D`: `column_height` and `column_share` feed mist up a falling column as well as at the source.
- Path tracing: `rendering/pathtracing/transmission_bounces` (default 4). After a path's first bounce, crossing glass, ice or water does not spend `pathtracing_max_bounces`; up to this many extra segments are traced instead. Clear ice under shaded eaves and back faces of panes no longer go black at 3 bounces.
- Releases ship export templates (`godot-fork-export-templates-<build>.tpz`: Windows x86_64, Linux x86_64 and arm64, debug and release) with PhysX and its libraries, installed from Editor > Manage Export Templates > Install from File.
- The Linux arm64 editor and templates include PhysX and Flow (Blast too when its arm64 build succeeds).
- `WeatherSkyMaterial.atmosphere_model` SCATTERING (default): the Hillaire 2020 atmosphere (transmittance and multiple-scattering tables, Rayleigh, Mie and ozone) lit by both the sun and the moon, with twilight and the Earth's shadow. The clouds reach the horizon through aerial perspective. `atmo_far_water` shows open sea past the scene. `PHYSICAL_SKY` keeps the older model.
- `SkyClock.drive_light_energy` and `energy_per_lux`: the sun and moon lights take their colour and strength from the same transmittance and their brightness above the air, and drive the sky.
- `SkyClock`: date, time and place on Earth point the sun and moon lights (Meeus ephemeris, moon parallax, refraction) and give the moon's phase, bright limb and brightness, sidereal time and a celestial basis for stars. `docs/bench/sky --test` checks it against Meeus' worked examples.
- Snow and ground prints: a bow wave. Bodies in the deform groups lean their rim forward with their speed (full at 4 m/s), and rim they meet again is shoved on, so a fast body pushes a wave ahead that spills into side berms and is left as a mound where it stops. `SnowSurface3D.press()` and `press_box()` take a `velocity`.
- Snow: `SnowClimate.recipe` takes the terrain's `ClimateRecipe` (temperatures, lapse rates, wet-day wind, the year's precipitation and its seasonality), and `SnowCover3D.climate_map` takes a `TerrainDataMap3D` whose orographic precipitation field sets where more snow falls, in place of `precipitation_gradient`. One climate for snow, rivers and plants.
- TerrainGenerator3D `deposition` (G of Yuan et al. 2019): rivers drop sediment where they slow, so ranges rising out of still ground get flat alluvial valley floors with winding rivers. Off (0) by default, with output unchanged byte for byte.
- TerrainMaterial3D: `far_blend_enabled`, `far_start`, `far_fade`. Past `far_start` the ground takes the plant cover's colour from the `terrain_cover_map` / `terrain_cover_rect` globals, replacing the near grass tint and plants' far colour, before wetness and snow. Off by default.
- TerrainGenerator3D: glacial stage (`glacial_enabled`, `glacial_ela`, `glacial_share`, `glacial_final`, `glacial_erosion`, `glacial_sliding`). Shallow-ice glaciers alternate with the rivers and wear troughs and basins; the last glaciation's basins become lakes by themselves (`get_glacier_depth()`, `get_last_glacial_*`). Walls the ice leaves past `strata_cliff_angle` shed rockfall to their foot.
- Sills with a `level` are a hard band at that height (a riegel): rock above it wears down normally. Range paths with `basin` metadata scale the uplift down (a high bowl).
- `cliff_ledge_spacing`, `cliff_ledge_strength`: rock beds of uneven thickness stepped into faces steeper than 40 degrees.
- `get_waterfalls()` fall record for the water code: `id`, `direction`, `type` (plunge, horsetail, cascade, tiered), `lip_points` with `lip_share` (Manning share of the flow), `lip_width`, `caprock`, `overhang`, three `face_profiles`, `tiers`, `ledges`, `power`, `breakup_length`, drag-limited `impact_speed` and `impact_speed_free`, `discharge_months`, and the plunge pool: `pool_center`, `pool_radius`, `pool_depth`, `pool_level`, `lands_in` (pool, lake, sea, ground).
- `plunge_pools` (on): a scour hole below each fall of 4 m or more (Mason and Arumugam depth), with its tail bar downstream so it holds water.
- `terrain_spray_potential` global map (with `terrain_spray_rect`, `spray_cell` 4 m, `get_spray_potential()`, `get_spray_image()`): the static share of fall spray each place can get, from each fall's power and the distance to its water.
- `gorge_age`, `gorge_rate`: lips over hard rock wear back upstream (knickpoint retreat, rate times sqrt(Q), a twentieth in massive rock) and leave a slot gorge below the fall; never into a lake. `get_waterfalls()` reports the fall at the gorge's head.
- `get_boulders(rect, density)`: blocks shed by cliffs, resting where their travel angle drops below the shadow angle, larger blocks further out; (x, y, z, radius) for placement.
- `glacial_rock_variation`, `glacial_rock_scale`: rock quality under the ice (joint density) varies the glacial erosion rate, so the ice digs its own basins and leaves its own bars: tarns and paternoster lakes form with no markers.
- `glacial_inflow_balance`: glaciers whose catchment lies beyond the map come in with their rivers (net balance times each inflow's off-map area), so the main valley gets its trough and chain of lakes with no markers.
- `moraine_age`: the last glaciation's margins build moraines (wall rockfall and basal debris carried down the ice, laid at its edge), terminal loops and lateral ridges that dam lakes behind them.
- `scree_age`: rock walls shed scree for that many years into aprons and cones at their feet, kept out of lakes and river floors.
- `glacial_coarse_cell`: the ice ages between river steps can run on a coarser ice grid, about three times faster (the terrain is similar, not the same).
- `ice_worn_detail`: rock the last glaciers wore keeps smooth faces toward the ice flow, stepped faces away from it, and fine striations (`terrain_ice_flow` global).
- Scree from `scree_age` and moraine till from `moraine_age` show in the ground: scree as bare blocks, till as soil with stones through it (geology talus channel, negative for till).
- TerrainMaterial3D `rock_detail_enabled`, `rock_detail_scale`: steep rock gets bedding planes and staggered joints, scree and till get loose stones, from world-space noise (no textures, no stretching on any face), faded out with distance.
- `meander_age`: rivers on flat alluvial floors meander for that many years (Howard-Knutson bank migration, neck and chute cutoffs, oxbows that silt up), cut into the full-resolution ground with the river water following the bends (`get_last_meander_count()`, `get_last_oxbow_count()`).
- TerrainGenerator3D: `inflows` markers bring land from beyond the map (`area` km2, `inflow_area`) into erosion, deposition, the rivers and their monthly discharge; `get_inflow_cells()`. `fall_lips` keeps falls over hard rock vertical in the ground so the jet falls free; `get_waterfalls()` reports `face_run`, `jet_throw` and `free_fall`.
- TerrainGenerator3D: `get_river_sites(min_area, min_discharge, run)` lists river points with their discharge, drainage area and the drop over the next `run` metres, for placing lakes and falls.
- TerrainGenerator3D lake markers take `rock_lip` (or `face_slope`) metadata: a bedrock sill instead of a landslide dam, so a lake can sit right above a waterfall and spill over it instead of burying the fall under the dam's apron.
- Terrain sky occlusion: the generator keeps each point's horizons and, from the live sky, the sky it sees; raster ground takes ambient light only from that sky (`TerrainGenerator3D.sky_occlusion`, `sky_light_source`; `TerrainMaterial3D.sky_occlusion_enabled`). Slopes under a ridge no longer glow with a sunset the ridge hides, matching the path tracer.
- `PlantSpecies.canopy_conifer` and the `conifer_range` rule: floor plants follow the stand type overhead (broadleaf or needle-leaved crowns); `VegetationField3D.get_conifer_at()`.
- VegetationField3D `cover_grass_layers`: more GrassField3D under `cover_grass` for the cover map, top down, so an undercoat filling the gaps between patches of tall grass no longer reads as bare ground from afar.
- `VegetationField3D.cover_map_enabled`: the plant cover seen from far above over the whole map (crowns, plants under them, grass), from the rules, published as `terrain_cover_map` / `terrain_cover_rect` for distant terrain. `GrassField3D.get_far_color()`.
- `VegetationField3D.density_mask` and `density_mask_rect`, `PlantSpecies.mask_channel` and `mask_invert`: painted or generated density masks open clearings and confine species to areas.
- PlantSpecies `near_mesh_source` and `near_mesh_key`: near meshes on demand. VegetationField3D asks the source for a cell's near mesh once the camera is within 1.2 x `near_range` and frees the cell beyond 1.5 x, so heavy near meshes exist only for plants that come close.
- `VegetationField3D.streaming`: places plants in tiles on worker threads around the camera (or `stream_target`) out to `stream_radius`, each species only out to its visibility range, identical to a full `populate()`; the canopy, forest floor and far colour maps follow the camera. About 2 to 3 ms of main-thread time a frame at a run through dense woodland (`stream_budget_msec`).
- PhysXWaterMist3D: mist from falling water as a small simulated grid (settling, evaporation with the humidity, carried by the spray wind and the weather's wind), drawn in raster as a FogVolume and under path tracing as a traced medium. PhysXRiver3D's waterfalls now make mist on every build (before: only with NVIDIA Flow and a PhysXFlow3D in the scene); `fall_mist_amount` scales it.
- `WeatherMap3D.get_season()`: one season state for every system (day length, warmth and chill sums for budburst, leaf colouring sum, snow depth, cover and line, soil frost, soil moisture) from a `SkyClock` date and a `ClimateRecipe`, shifted by the live weather.
- WeatherMap3D snapshot: a read-only copy of rain, cloud cover and cloud shadow that worker threads can sample (C++).

### Changed
- `PhysXWaterfalls3D`: contact falls are as wide as their flow (`4 sqrt(Q)` m), thick and white in the middle and thin at the sides, with soft edges that wander down the face and wet rock beside them (`wet_margin`). The sheet no longer shows saw-tooth edges or holes below ledges, stays white across a ledge, and its spray shows soft puffs as well as drops.
- Thin contact falls over stepped rock fall ledge to ledge as threads with wet rock between them (nappe flow), and cover the face only as their flow nears the step height (`rock_step`).
- Docs site (Hugo, GitHub Pages) with a page per feature area; README shortened to a landing page.
- `GrassField3D`: blade colour varies over the field and per blade (`color_variation`), a share of dry blades are dead from the root, blades carry a midrib and veins and taper near the tip.
- `GrassField3D.get_ground_shader_code()`: the ground under grass becomes turf matched to the blades (same colour patches, streaks, thatch), so gaps between blades show grass, not soil.
- `GrassField3D.canopy_field`: the sward follows the canopy shade of a `VegetationField3D`.
- GrassField3D measures its far colour from a small render of its own blades (`get_far_color()`, `is_far_color_baked()`, signal `far_color_changed`), so the far meadow matches the blades for any grass look, lean included; VegetationField3D's cover map follows it.
- The far meadow's colour turns with the sun's heading from the view, brighter looking down-sun (the canopy hot spot), darker into the sun: GrassField3D's far colour bake renders the blades with the sun at four headings and fits the change (`get_far_color_lobes()`), and `grass_ground()` applies it with the sun from `sun_light` (or the scene's first DirectionalLight3D).
- MultiMesh buffers are cleared on the GPU when allocated instead of uploading a zeroed copy.
- GrassField3D's blue-noise rank no longer takes a lock per blade.
- GrassField3D: with a WeatherMap3D, each blade answers the gusts as a damped oscillator (about 2 Hz, slower when taller): slow gusts pass, gusts near its own frequency are amplified and lag, and the sward's resonant sway travels downwind as honami (Py, de Langre and Moulia 2006). Same model as TreeWind3D.
- GrassField3D: near density defaults to 120 blades/m2 (was 60), affordable with the GPU fill.
- GrassField3D: the darkening towards the root moved from the blades' albedo to AO, from the leaf area above (Monsi-Saeki, Goudriaan); the path tracer no longer darkens the roots twice, and raster ambient follows the sward's density.
- PhysXHair3D deep shadows: strands are drawn into the deep opacity maps once per cell into the nearest cell (as a rasterized line) instead of twice per cell into four, about a third of the maps' cost for the same picture
- `rendering/pathtracing/linear_swept_spheres` is off by default until swept sphere hair has been checked on a GPU that has it
- Landscape3D: meshes for new levels are built on worker threads and shown together once all are ready, so moving no longer stalls the main thread (42 ms to under 1 ms when 200 m of chunks change). With a TerrainGenerator3D source (`stream_heights`), the ground is read as a 4 m grid first and in full per chunk where the view needs it, nearest first; `load_all_heights()` reads the rest. Heights baked to an image load the 4 km, 1 m ground in under a second.
- `PlantGenerator.make_floor()` seedlings are now young plants of the stand's own trees (beech, oak, sycamore, ash, maple), with their leaf forms; `litter_leaves` also applies to `PRESET_SEEDLING`.
- `PhysXWaterMist3D`: mist fades out over the last cells at the box's sides and top, so the box never shows as a hard-edged block.
- README: transmission bounces, vegetation density masks, rock-lip lakes.
- README: river deposition line; Yuan et al. 2019 in references.
- README: raster waterfalls lit by the scene, with image.
- README: wide ground shot with raster reflections on the sea.
- README: lake ripples image.
- README: lake ripples line.
- README: Landscape3D line and image.
- README: sea foam lit by the scene; re-rendered waterfall pair.
- README: Sky and clouds section with the sun and moon clock.
- README: waterfall mist line and image.
- Path tracing: snow sparkle (the footprint grid) now also shows on reflected and bounced rays, sized by each ray's cone, so snow seen in a mirror, water or a window glints, and the light snow bounces carries its glints. Exact glints already applied to every bounce.
- Snow: the near square feeds its state back to the world tier under it (area averages of water, liquid, density and albedo) before each world update and before it moves, so drifts, slides and melt worked out on the fine cells are kept when the square moves away and back.
- TerrainGenerator3D: `deposition` solves each step exactly in two sweeps along the receiver tree before its Gauss-Seidel passes, so a step needs one or two passes instead of about seven (the deposition part of a generate 0.73 to 0.40 s). `get_last_deposition_passes()` reports them.
- Grass and ground plants take the light they grow in: under crowns their leaves are darker and bluer green and grass grows longer and laxer; on sunny slopes leaves are paler and yellower, grass shorter and drier. Open flat ground is unchanged. Petals and litter keep their colour.

### Fixed
- Contact falls no longer draw a pale saw-tooth fringe beside the water: damp rock is darkened and nearly matte with a soft edge, the clear film is drawn at the wet rock's darkness, and gully walls dim the film's raster sky reflection.
- Raster waterfall sheets and river white water were unshaded with a fixed light, so a cascade in a shaded valley glowed flat white; both are now lit by the scene's lights, shadows and sky (open water keeps its look).
- Volumetric fog no longer drops fog volumes thinner than 0.001 /m: density is stored at 1/65536 /m with stochastic rounding.
- GrassField3D no longer ends in a hard line at its radius: blades thin out one at a time and shrink over the outer band, and the ground under the grass turns to the blades' colour (`grass_ground_fade`, `grass_ground_far`).
- GrassField3D: in shadow passes blades widened towards the light's camera, so the sward cast more shadow than it has; they now widen towards the main camera.
- GrassField3D: the GPU fill's compute shader is freed at exit.
- Hair cards no longer read see-through and bluish beside strands: cards are lit like the strands, widen as their clump fans out with its guides, and follow every groomed point of a straight strand
- Lakes and other fetch-limited water lay flat: their short wind waves were finer than the FFT grid. The grid is now raised until the waves' peak is resolved (up to 256), so a lake at 4 m/s shows its ripples.
- TerrainGenerator3D: a lake marker that made no lake failed silently; it now warns with the reason.
- Raster waterfall mist was invisible unless the scene turned on volumetric fog by hand: PhysXWaterMist3D now turns the Environment's volumetric fog on (density 0, sky-lit) while a mist is live and puts it back after (#239).
- `PlantGenerator.litter_leaves` was not registered as a property, so scripts could not set it.
- Light projectors and decals now follow updates to their texture in raster. The raster kept the first image of an updated texture (such as the weather's cloud shadow), so raster cloud shadows sat in the wrong place.
- Path tracing: a dynamic mesh larger than `rendering/pathtracing/dynamic_blas_update_triangle_budget` (such as a simulated hair groom) was never refit after its first build, so it stood still while the raster moved; the first refit of a frame now ignores the budget, as the first build already did
- Path tracing: a material writing `BENT_NORMAL_MAP` failed to compile its hit shader and the object vanished
- Path tracing on devices without shader execution reordering (VK_EXT_ray_tracing_invocation_reorder), such as Mesa's CPU Vulkan driver: the reordering pipeline failed to build and the frame fell back to an untraced, unshadowed image. Reordering is now used only where the device supports it (`RenderingDevice.SUPPORTS_RAYTRACING_INVOCATION_REORDER`).
- Path tracing: a MultiMesh surface whose vertex shader is captured is left out until its first capture is built, instead of being traced as its bare mesh (grass blades showed as flat 1 m strips until the dynamic build budget reached them).
- Windows and macOS editor downloads build again after the vegetation far-colour and gorge changes (narrowing and ambiguous lerp errors on MSVC and Apple clang).
- Windows, macOS and Linux x86_64 editor downloads build again: vegetation, raster water reflections and river code now compile with MSVC, Apple clang and GCC 11.
- River white water no longer saturates into solid white bands at short rapids: its foam keeps an open lace.
- River water spread wider than its ribbon (onto beaches and flats) fades out along the bank's relief instead of ending in a straight edge.
- The weather sky no longer shows a magenta and green checker along the horizon at dusk.
- Raster water hides the seabed with depth, by the path tracer's own absorption, so a shallow shelf no longer shows as a pale band ending in a straight line at the edge of the measured seabed.
- Raster sea foam (whitecaps, shore foam) and wet sand were unshaded, so they glowed in shade, at dusk and under cloud shadows; they now take the scene's lights, shadows and sky, as under path tracing (open water keeps its look).
- The flat grey band between a cloud deck and a sea horizon.
- Snow: with no world tier, ground outside the snowpack's square shows no snow (raster and path traced) and `SnowCover3D.get_depth_at()` returns 0 there, instead of stretching the square's edge outward.
- Snow and ground prints: footprints are stamped with antialiased edges, loose pushed-aside snow slides off at its repose angle (no more spikes between overlapping steps at fine deform settings), and `SnowSurface3D` prefilters the deformation to its vertex spacing when its triangles are coarser than the deform texels.
- TerrainGenerator3D: the erosion detail no longer cuts trenches along river channels on an alluvial floor or a comb of gullies into a fan toe: it fades over the first half metre of alluvium (was 2 m), and the fade reaches 32 m past the deposit and then thins out over 3/8 of `detail_wavelength`.
- TerrainMaterial3D: the detail normal fades where the geometric detail does (alluvium, water, coastal strip), from a new `terrain_detail_keep_map` shader global, so alluvial plains no longer shade with a ripple pattern.
- TerrainMaterial3D with a generator painted sand on all ground under `beach_height`, turning low coastal plains into sand flats. Sand now follows the generator's beaches (passed in the geology map's soil channel as -1..0), plus the ground under the sea.
- TerrainGenerator3D with `deposition`: the bedrock detail filter (gullies, crest noise) fades out over the first 2 m of alluvium, so fan fronts and terrace banks no longer grow rocky knobs.
- Below a meandered reach the river's routed course is cut to the channel's bed all the way to its mouth (through the beach berm), so the water no longer thins over a bank at the reach's foot or stops short of the sea.
- `get_waterfalls()`: the brink moves to the channel when the river's cell sits on a slot's edge, the lip spans the wetted width only, and falls under a lake or the sea, or under the minimum height once on the channel, are left out.
- Abandoned one-cell river slots on fans are backfilled (`fan_backfill`, on).
- Detail no longer fades on faces steeper than loose fill can stand, however much sediment the cells around them hold.
- Loose fill no longer reads as soil on ground steeper than `critical_slope` (a 74 degree bar face showed 390 m of soil and grew plants).
- VegetationField3D: under path tracing the near-swap cells drop their crossfade margins, so plants in the band are no longer traced twice (near and far mesh); the margins come back when path tracing is off.
- Raster open water (sea, lakes, rivers) no longer glows at dusk and night: its body colour is lit by the sun and the baked sky's irradiance.

## build-a86e608b (2026-11-11)

### Changed
- The sea, lake and river flow bakes solve their stream function with multigrid-preconditioned conjugate gradients instead of SOR: about 6x faster at 160 x 160 cells, 10x at 320 x 320, the same flow. `physics/physx_3d/water/multigrid_flow` (on) is the opt-out.
- README: faster flow bakes line.
- README: raster reflections cover rivers; river image and shore waves clip re-rendered.

### Fixed
- Raster rivers seen from above now reflect the sky and the sun's glitter by the exact Fresnel term, like the sea since `raster_reflections` on PhysXWaterSurface3D; PhysXRiver3D `raster_reflections` (on) is the opt-out. The sea and rivers share the code (water_raster_reflections.h).

## build-163faf95 (2026-11-11)

### Fixed
- Raster water seen from above now reflects the sky and the sun's glitter by the exact Fresnel term, as the path tracer does; before, it was an unlit tint with no reflection at all (no glitter path, no sky at grazing angles). PhysXWaterSurface3D `raster_reflections` (on) is the opt-out.

### Changed
- README: boat wakes line and image; wide ground shot with the cloud layer.
- README: raster water reflections line and image.

## build-0fe6589a (2026-11-11)

### Added
- PhysXWaterWake3D `dispersive` (on by default): the wake's waves are stepped in Fourier space with deep-water dispersion (eWave, Tessendorf 2014), so each wavelength travels at its own speed and a moving hull leaves the Kelvin pattern: a 19.5 degree wedge with transverse waves behind the stern and diverging waves on its arms. The hull acts as a pressure patch. Off keeps the wave equation at `wave_speed`. The grid rounds up to a power of two (192 cells over 48 m become 256 over 64 m).
- PhysXWaterWake3D `breaking_slope` (0.3): with `dispersive`, the wave slope past which wake waves break into foam.

### Changed
- README: whitecaps by wind speed line and image.

## build-c29dac43 (2026-11-11)

### Changed
- Snow: icicles read as clear ice with a hazy core (`SnowIcicles3D.hazy_core`, `core_material`), and their ripples grow with length instead of striping every cone under path tracing.

### Added
- PhysXWaterSurface3D `whitecap_auto` (on by default) and `whitecap_cover_scale`: the foam threshold is steered so the share of sea under whitecaps follows the wind as measured at sea (Monahan and O'Muircheartaigh 1980: about 1% at 10 m/s, 3% at 14 m/s, 10% at 20 m/s), whatever the cascades, choppiness or fetch; `get_whitecap_cover()`, `get_whitecap_target_cover()`, `get_effective_foam_threshold()`.

## build-091c98e1 (2026-11-11)

### Fixed
- WeatherSkyMaterial: clouds no longer look like flat-topped slabs with vertical walls (the shape noise was stretched so far in height that each cloud was one column through the layer), and distant clouds near the horizon are no longer boxes (the noise is read at the pixel's footprint, not the march step).

### Changed
- README: cornice fall and icicles lines; cornice fall image.
- README: exact path traced snow sparkle; SPECULAR_NORMAL shader output.
- README: pixel antialiasing line and image; hair deep shadows raster and path traced pair.

### Added
- Snow: cornices open a tension crack by creep and fall once their root cannot hold them, as tumbling blocks with debris at the foot of the face (`SnowCover3D.cornice_failure_enabled`, signal `cornice_failed`, `SnowCornice3D.debris_enabled`).
- Snow: `SnowIcicles3D`, icicles grown along an edge from melt water running off the snow above into air below freezing, drawn as clear ice under raster and path tracing.
- Spatial shaders: `SPECULAR_NORMAL`, a normal for the base specular lobe alone under the path tracer (the diffuse keeps `NORMAL`); raster ignores it.
- Snow: exact path-traced sparkle, every 2 mm ice grain of dry snow a facet hit per ray, with the global shader parameter `snow_exact_glints` (off by default); `snow_glint_apply_specular()` in `get_snow_shader_code()` for custom shaders.

## build-d916f05e (2026-11-11)

### Changed
- Path tracing without Ray Reconstruction spreads each pixel's samples after the first across the pixel (box filter), so detail finer than a pixel, such as hair strands, is antialiased instead of sparkling
- README: rivers fed by rain and snowmelt line and chart.
- README: terrain rock layers and waterfalls line and image.
- README: measured ocean wave spreading line.

## build-250256ab (2026-11-10)

### Added
- PhysXWaterSurface3D `wave_spreading` (Donelan-Banner by default, Hasselmann, or the old Cos2) and `swell`, after Horvath 2015: the dominant waves run with the wind and short waves cross, matching Cox and Munk's measured slopes (1.41 against 1.48 along/across at 14 m/s; Cos2 gave 2.72).
- `physics/physx_3d/water/shared_memory_fft` (off until timed on a GPU): the ocean FFT runs each line's whole transform in shared memory, all four transforms of a step in two dispatches per cascade instead of 8 log2(N); the same field.
- `TerrainMaterial3D.ground_deform_enabled`: the terrain takes `GroundSurface3D` prints (sand, mud, leaf litter) with its own material.

### Changed
- README: wind moves lying snow line and image.

## build-939acbda (2026-11-10)

### Added
- PhysXHair3D `deep_shadows` (on, with `hair_shading`) and `deep_shadow_light`: the raster strands shade themselves from a directional light with a deep opacity map (Yuksel and Keyser 2008), smooth instead of grainy
- Spatial shader fragment output `DIRECTIONAL_SHADOW_OFFSET`: moves the fragment's directional shadow lookups toward each light by that many metres

### Changed
- PhysXHair3D strands cast into shadow maps with their own noise threshold each (stochastic transparency), so the shadow behind several strands builds up as their product
- README: clouds that never repeat line.
- README: hair deep opacity shadows line and image.

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
