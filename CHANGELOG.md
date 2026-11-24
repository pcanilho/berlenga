# Changelog

What changed in this fork, newest first. Upstream Godot's own changes are in its release notes.

Every pull request adds its entry as a file in `changelog.d/` (see [changelog.d/README.md](changelog.d/README.md)), in one of: Added, Changed, Fixed, Removed. Write what changes for someone using the engine, in one line, with the PR number. The release workflow publishes those entries as the release notes and moves them here under that release's heading.

## Unreleased

## build-1dd038a4 (2026-11-24)

### Added
- StreetWater3D: GPU shallow water over a street's relief in a camera-following window (2.5 cm cells). Rain runs off as a film into ruts, hollows and gutters; tyres and feet (submit_sphere) push the water aside into bow waves and V wakes and it runs onto dry road; soaking, evaporation and damp that dries. The road material draws it from one map (get_shader_include), so raster and path tracing match.
- RoadRelief3D.get_surface_height_at: the built mesh's height, cheap enough for many samples.

## build-8c7d4c79 (2026-11-23)

### Fixed
- Path tracing: a blended (alpha) layer's own bounce light is no longer lost at some alpha values, which left blended surfaces near black where only sky and bounce light reach them (#321).
- Path tracing: the medium scratch image starts cleared, so a stray read can't pick up another surface's leftover colours (#400).

## build-57e5524c (2026-11-23)

### Changed
- Old-growth beech stands: veteran giants grow visibly thicker than the canopy, canopy crowns are fuller and domed, and most fallen giants lie with their root plate up.

### Fixed
- Stand variants no longer compound each other's band settings (crown room, shade, shedding), so every variant of an old-growth, succession or taiga species is grown as its band intends.

## build-c2def7d8 (2026-11-23)

### Added
- RainFall3D body_collision: rain and eave drips land and splash on moving physics bodies near the camera (characters, vehicles, rigid props).
- RoadRelief3D: street surface whose crown, gutters, wheel ruts, ISO 8608 roughness, sags, birdbaths, patches and potholes are real geometry (raster, path tracer, collider and water read one height via get_height_at). Built from world position, so it never repeats and joins without seams.

## build-4121c48d (2026-11-23)

### Changed
- PhysXFlow3D surface_glow (and lights dimmed through the gas) works without a DirectionalLight3D, e.g. a fire at night.

## build-bf7f03d5 (2026-11-23)

### Fixed
- A point or spot light inside or behind PhysXFlow3D gas (a fire's own light) no longer lights surfaces straight through the gas in Forward+ (with surface_glow on).

## build-40d110e1 (2026-11-23)

### Changed
- Camera cut-out: draws the cut cannot reach compile its discard out (`CAMERA_CUTOUT_REACH` in spatial shaders), keeping early depth tests (no setting).

## build-a0f91e0f (2026-11-23)

### Changed
- Camera cut-out: draws the cut cannot reach compile its discard out (`CAMERA_CUTOUT_REACH` in spatial shaders), keeping early depth tests (no setting).

## build-1ab80137 (2026-11-23)

### Added
- Raster gas lights the surfaces around it and hides the sky from them, as under path tracing: snow beside a sunlit cloud picks up its light, and the ground around dark smoke sees less sky (PhysXFlow3D `surface_glow`, on).

## build-56347589 (2026-11-23)

### Added
- Raster gas lights the surfaces around it and hides the sky from them, as under path tracing: snow beside a sunlit cloud picks up its light, and the ground around dark smoke sees less sky (PhysXFlow3D `surface_glow`, on).

## build-12b84872 (2026-11-23)

### Changed
- Trees of one species in a stand now differ in leaf colour and lightness (setting crown_tint_variation).
- Root plates of thrown trees show lobed, torn rims, surface roots and leaf litter.
- Old-growth beech crowns are leafy through the crown, and shade-layer beech grow broad, level crowns.
- Boreal spruce branches sag, and dead branches stay only as short stubs.

### Fixed
- Fire char height and fire kill on trees placed by a vegetation field now follow each tree, not the field's origin.

## build-5ff4cdf8 (2026-11-22)

### Changed
- Camera cut-out: shadow maps of cut-out materials draw with the shared shadow material again, as before the cut-out (no setting).
- In raster, each tree crown shades its own inner and lower leaves from the sky (setting crown_occlusion).

### Fixed
- Tree leaves take sky light again under path tracing: crowns in shade no longer go near-black, and raster and path-traced crowns now match in brightness.

## build-961f7961 (2026-11-22)

### Added
- Camera cut-out: a soft dithered hole through walls, roofs and trees between the camera and the player, the same in the path tracer (global shader parameters `camera_cutout` and `camera_cutout_floor`, `BaseMaterial3D.get_camera_cutout_shader_code()`); characters and anything else can stay in view (`BaseMaterial3D.camera_cutout`).
- Lights can flicker, by a light-style pattern or as a failing lamp with seeded faults, with an emissive material following them (`Light3D.flicker_mode`).
- Positional shadows go to the most important lights first (energy times size on screen, a light keeping its shadow until another is clearly more important), so a full shadow atlas drops the least important; a budget caps how many keep shadows (project setting `rendering/lights_and_shadows/positional_shadow/max_shadowed_lights`, 0 = as many as fit).
- Rain and snow haze near the camera falls in curtains carried by the wind and following the storm cells, lit by lamps and the sun, with the same mean haze (WeatherMap3D `rain_curtains`).
- The water standing on a built surface, from the weather's pooled water on faces level enough to hold it, for materials and the path tracer alike (`weather_surface_puddle()`).

### Fixed
- Flow gas drawn without path tracing matches the path tracer more closely: the shaded side of thick gas is no longer lit from inside (snow powder lit from behind was up to 30 levels too bright), and the foot of a cloud takes the ground toward the camera as sunlit or shaded as it really is. Smoke, dust and fire move toward their path-traced look as well.

### Changed
- Floodplain trees show the last flood's silt line where the water system recorded one, at its height and facing its flow; trees the flood did not reach stay clean.
- Broadleaf leaves let through about as much light as they reflect, as measured leaves do: crowns seen from below glow green against the sky under path tracing instead of drawing near-black. Dead, frost-killed and turned leaves glow brown or yellow against the light.
- The methods that raise `WorldGround.relief_changed` and `WorldStand.stand_changed` are now `notify_relief_changed()` and `notify_stand_changed()`, so the signals and methods no longer share names.

## build-9534e321 (2026-11-22)

### Added
- Camera cut-out: a soft dithered hole through walls, roofs and trees between the camera and the player, the same in the path tracer (global shader parameters `camera_cutout` and `camera_cutout_floor`, `BaseMaterial3D.get_camera_cutout_shader_code()`).
- Positional shadows go to the most important lights first (energy times size on screen), so a full shadow atlas drops the least important; a budget caps how many keep shadows (project setting `rendering/lights_and_shadows/positional_shadow/max_shadowed_lights`, 0 = as many as fit).
- Rain and snow haze near the camera falls in curtains carried by the wind and following the storm cells, lit by lamps and the sun, with the same mean haze (WeatherMap3D `rain_curtains`).
- The water standing on a built surface, from the weather's pooled water on faces level enough to hold it, for materials and the path tracer alike (`weather_surface_puddle()`).
- The rain a face catches, for materials and the path tracer alike: the vertical rain on faces that look up and the wind-driven rain on faces turned into the wind, with the wind's push on drops along the face (`BaseMaterial3D.get_surface_rain_shader_code()`, `weather_surface_rain()`).
- Walls run with the rain the wind drives onto them, in streaks that lean with the wind, while lee walls stay drier; moving bodies blow their running water back (BaseMaterial3D `wetness_moving`, instance parameters `rain_slipstream` and `rain_wipe`; vehicles set it themselves).

### Changed
- Broadleaf leaves let through about as much light as they reflect, as measured leaves do: crowns seen from below glow green against the sky under path tracing instead of drawing near-black. Dead, frost-killed and turned leaves glow brown or yellow against the light.

## build-ab7c0a22 (2026-11-22)

### Added
- Flow emitters' smoke joins the weather's regional smoke: a campfire's or chimney's smoke drifts past the Flow box and pools in valleys on calm nights (PhysXFlowEmitter3D `regional_smoke`).
- Green alder (`TreeGenerator.PRESET_GREEN_ALDER`), the many-stemmed scrub of avalanche tracks, and snow-bent stems on steep slopes whose feet lie downslope before they rise (`cliff_sweep`); both take young avalanche tracks.
- Avalanche survivors carry impact scars on the upslope face of the stem (`avalanche_scars`).
- Foehn: a wind across a range comes down its lee warmer and drier, several K warmer after the windward side rains out, with a gap in the cloud over the lee (WeatherMap3D `foehn_winds`).
- The rain a face catches, for materials and the path tracer alike: the vertical rain on faces that look up and the wind-driven rain on faces turned into the wind, with the wind's push on drops along the face (`BaseMaterial3D.get_surface_rain_shader_code()`, `weather_surface_rain()`).
- Along-valley winds: valleys blow up-valley on sunny calm afternoons and down-valley on clear calm nights, a few m/s, carrying smoke and fog along them (WeatherMap3D `valley_winds`).
- Walls run with the rain the wind drives onto them, in streaks that lean with the wind, while lee walls stay drier; moving bodies blow their running water back (BaseMaterial3D `wetness_moving`, instance parameters `rain_slipstream` and `rain_wipe`; vehicles set it themselves).

### Fixed
- A weather map or terrain generator that leaves the scene takes its shader maps with it, so ground drawn later no longer reads a freed map's wetness, spray, geology or sky.

## build-47595e0b (2026-11-22)

### Changed
- Wind shear over dunes and snow drifts is computed about 3.5 times faster, with identical results.
- Raindrop rings on water keep falling under freezing rain, raster and path traced (the GPU's rain share reads the warm layer aloft, `weather_warm_nose`).

### Added
- Flow emitters' smoke joins the weather's regional smoke: a campfire's or chimney's smoke drifts past the Flow box and pools in valleys on calm nights (PhysXFlowEmitter3D `regional_smoke`).
- After an avalanche: in mountain stands, trees thrown down the fall line, snapped stumps, and survivors leaning downslope with their uphill limbs stripped, placed where the world records an avalanche (`TreeGenerator.SUCCESSION_AVALANCHE`, species meta `fall_downhill`).
- Trees on cliffs: rooted in a crevice, the stem leaves the rock face and curves up to lean out, roots run over the face along its cracks, and one side of the crown dies back over a strip of dead bark (`TreeGenerator.cliff_angle`, `cliff_lean`, `cliff_dieback`).
- Cliff yew over beech woods, Scots pine in montane conifers and hemlock in white pine-hemlock, on rock faces past 52 degrees.
- Trees near the camera take the season where they stand: a late frost browns the new leaves in cold hollows, and high slopes leaf out later than the valley (`WorldWeather.season_at`, `TreeGenerator.near_season_band`).
- Foehn: a wind across a range comes down its lee warmer and drier, several K warmer after the windward side rains out, with a gap in the cloud over the lee (WeatherMap3D `foehn_winds`).
- Late frost: a spring air frost below about -2 C browns the young leaves of the species already out, first in the cold hollows, until a second flush replaces them weeks later (WeatherMap3D `late_frost`).
- Sea and lake breezes: warm afternoons blow onshore a few m/s, reaching some 20 km inland, and cool nights blow a weaker land breeze off the shore (WeatherMap3D `sea_breezes`).
- Smoke in cold pools: smoke let out on a clear calm night stays near the ground in the valleys, drains down the slopes, and mixes up after sunrise (WeatherMap3D `smoke_in_pools`, `WorldWeather.smoke_concentration_at`).
- Day upslope winds: sunny calm afternoons run the air up slopes at 1 to 2 m/s, and fair-weather cumulus gathers over crests where they meet when the air is moist (WeatherMap3D `upslope_winds`).
- Along-valley winds: valleys blow up-valley on sunny calm afternoons and down-valley on clear calm nights, a few m/s, carrying smoke and fog along them (WeatherMap3D `valley_winds`).

### Fixed
- The terrain material compiles again in projects without the far sun shadow globals defined (`terrain_sun_horizon_*` now registered with the others).
- A weather map or terrain generator that leaves the scene takes its shader maps with it, so ground drawn later no longer reads a freed map's wetness, spray, geology or sky.

## build-ca985258 (2026-11-22)

### Changed
- Wind shear over dunes and snow drifts is computed about 3.5 times faster, with identical results.
- Raindrop rings on water keep falling under freezing rain, raster and path traced (the GPU's rain share reads the warm layer aloft, `weather_warm_nose`).

### Fixed
- The terrain material compiles again in projects without the far sun shadow globals defined (`terrain_sun_horizon_*` now registered with the others).

### Added
- After an avalanche: in mountain stands, trees thrown down the fall line, snapped stumps, and survivors leaning downslope with their uphill limbs stripped, placed where the world records an avalanche (`TreeGenerator.SUCCESSION_AVALANCHE`, species meta `fall_downhill`).
- Trees on cliffs: rooted in a crevice, the stem leaves the rock face and curves up to lean out, roots run over the face along its cracks, and one side of the crown dies back over a strip of dead bark (`TreeGenerator.cliff_angle`, `cliff_lean`, `cliff_dieback`).
- Cliff yew over beech woods, Scots pine in montane conifers and hemlock in white pine-hemlock, on rock faces past 52 degrees.
- Trees near the camera take the season where they stand: a late frost browns the new leaves in cold hollows, and high slopes leaf out later than the valley (`WorldWeather.season_at`, `TreeGenerator.near_season_band`).
- Late frost: a spring air frost below about -2 C browns the young leaves of the species already out, first in the cold hollows, until a second flush replaces them weeks later (WeatherMap3D `late_frost`).
- Sea and lake breezes: warm afternoons blow onshore a few m/s, reaching some 20 km inland, and cool nights blow a weaker land breeze off the shore (WeatherMap3D `sea_breezes`).
- Smoke in cold pools: smoke let out on a clear calm night stays near the ground in the valleys, drains down the slopes, and mixes up after sunrise (WeatherMap3D `smoke_in_pools`, `WorldWeather.smoke_concentration_at`).
- Day upslope winds: sunny calm afternoons run the air up slopes at 1 to 2 m/s, and fair-weather cumulus gathers over crests where they meet when the air is moist (WeatherMap3D `upslope_winds`).

## build-2462187b (2026-11-22)

### Added
- Ash blown off burnt ground settles downwind and darkens snow (WorldSnow impurity, as black carbon equivalent).
- Static bottom-level acceleration structures are compacted after their build, saving ray tracing memory with identical frames (`rendering/rendering_device/acceleration_structure/compaction`, on by default).
- `PhysXDestructible3D.initial_linear_velocity` and `initial_angular_velocity`: a dynamic destructible can start moving, so swapping a falling object for one keeps its motion.
- `PhysXBlastAsset.prepare_render()`: converts the chunks' render meshes ahead of time, from any thread.
- `PhysXBlastAuthoring.build_prefractured` takes pieces with several materials (a Mesh or an Array of surfaces; `PhysXBlastAsset.materials` by slot) and an optional `health` per bond (`PhysXBlastAsset.bond_healths`, a multiple of the destructible's health), so a tree's limbs draw bark, wood and leaves and a twig joint breaks before a trunk joint. `PhysXDestructible3D.get_piece_chunks()` lists the chunks a piece carries.
- Burning buildings drop their window glass as shards and their ceilings and roofs sag and fall in room by room. `PhysXBuildingFire3D.glass_panes`.
- PhysXBuildingFire3D records its first catch: `get_ignition_time()`, `get_ignition_cause()` ("direct", "firebrand" or "heat") and the `ignited` signal.
- `docs/bench/village_fire`: a wind-driven grass fire runs into a street of timber houses; prints which houses catch, when and by what; dry beds against the walls, lawns, buffer, gap and wind are options.
- Burnt-out rooms smoulder: their debris glows for hours, smokes, sheds embers in wind and can rekindle. `PhysXBuildingFire3D.get_room_ember()`, signal `rekindled`.
- Smoke runs ahead of building fires: rooms fill with a smoke layer from the ceiling that spills through doors into the next rooms and pours from windows and eaves minutes before flames; visibility inside is reported. `PhysXBuildingFire3D.get_visibility_at()`.
- Water puts burning buildings out: a hose or fluid water in a room knocks its fire down below flashover and puts it out with steam pouring from the windows, and a weak stream only holds it back. `PhysXBuildingFire3D.apply_water()`.
- Window flames of burning buildings run up the wall above as sheets instead of balls.
- Wind drives building fires: it blows in through windward openings, flames leave by the leeward ones and hot gas is pushed through doors.
- Burning wooden buildings: rooms grow to flashover, break their windows, throw flames out of them, burn through walls, floors and roofs into the next rooms and collapse; they catch from ground fires, radiation and firebrands, and throw firebrands and smoke. `PhysXBuildingFire3D`.
- WorldBurnScar3D: author an old burn (a rectangle or polygon and its age in years); the fire contract reports how long ago the ground burnt (`WorldFire.burn_age_at`).
- `PhysXBurnable3D`: wooden objects (crates, logs, debris) catch fire from a `PhysXFireSpread3D` once the heat they take in reaches the ignition dose, burn with a Flow flame, char from the ignition point with glowing alligator cracks, smoulder when rain puts them out, and throw embers that can start new fires where they land.
- `PhysXCloth3D` `burnable` (on): cloth catches fire from ground fire, hot Flow gas and burning objects, chars with a glowing edge and burns into holes until it falls apart. `flame_spread`, `burn_through_time`, `ignite()`.
- River current drags clothing: worn cloth under water loses most of its weight and is dragged by the current (any water with a flow: rivers, waves), pressing against the legs on the upstream side.
- River current drags flags and sheets: `PhysXCloth3D` under water keeps about 30% of its weight and streams with the current from the world water contract (CPU solver).
- Wading wets clothing: worn and free cloth that dips into any water (sea, lakes, rivers, pools) soaks up to a ragged tide line just above the surface, then dries; the wet band adds its weight and cling.
- `PhysXCloth3D` `weather_wetting` (on): rain soaks cloth by how it faces the rain, and it dries in the air; wet cloth is heavier, limper and darker, clings to what it touches and needs its water boiled off before it burns. `wetness`, `water_capacity`.
- `PhysXSkinnedCloth3D` `weather_wetting` (on): worn clothing soaks in rain and dries the same way; wet garments draw darker, swing less and cling to the body. `wetness`, `fabric_density`, `water_capacity`.
- The ground contract reports how much of the ground pebbles and gravel cover, from the stone fields (`WorldGround.coarse_cover_at`, `RockField3D.get_coarse_cover_at`).
- Post-fire debris flows carry boulders at their front and throw mud, in a train of surges that stop on their fan. `PhysXDebrisSurge3D`.
- Smoke haze, burnt forest and settling dust darken snow and make it melt sooner; fresh snow buries the dirt and the thaw brings it back (`SnowCover3D.impurities_enabled`).
- Waterfalls count as water for everything that touches them: bodies and clothes in a fall get wet and are pushed down by it.
- Wind lifts ash after a fire: a strong wind over the cold burnt ground of a `PhysXFireSpread3D` lifts its ash as a low grey dust that drifts downwind in gusts until the ash is gone; wet ground holds it. `blowing_ash` (on by default), `get_ash_blown()`.
- Rain washes the burn scar: a fire leaves burnt soil water-repellent for a year or two, so even light rain runs off it, and the runoff carries the ash away. `WorldFire.water_repellency_at()`, `PhysXFireSpread3D.get_water_repellency_at()`, `PhysXFireSpread3D.get_ash_washed()`; GroundSurface3D reads it (burnt litter gone, infiltration cut).
- `PhysXFireSpread3D` burns the logs and fallen branches of its `vegetation_field` as large wood: they catch from the fire under them, flame for about three minutes per centimetre of diameter, then smoulder; bark chars and glows in its cracks. `get_heavy_fuel_at()` reads what is left.
- `PhysXFireSpread3D` burns the plants over it: grass burns down to charred stubble with glowing tips in the burning band, small plants char under the flame and scorch brown above it, tree bark chars to the flame length and leaves brown up to the crown scorch height (Van Wagner 1973), in raster and path tracing, through a new `fire_state_map` global.
- `PhysXFireSpread3D.fuel_patchiness` (on) makes fuel and moisture patchy, so a burn scar has ragged edges and unburnt islands; `fuel_map` takes moisture in its green channel, and `vegetation_field` makes worn paths bare and leaf litter burn.
- Burnt trees fall later: a fire chars each stem's foot (`PhysXFireSpread3D.get_stem_char_at()`, `WorldFire.stem_char_at()`), and in a later storm `TreeWind3D` lets a charred tree snap at its foot and fall without its root plate. `get_storm_snap_count()`.
- Fire clouds: a fire's plume that rises past the condensation level caps itself with a pyrocumulus, held over the bent-over plume while it burns and drifting off after; the largest fires tower into a pyrocumulonimbus with lightning and a gusty outflow. `WorldFire.plumes()`, `WeatherMap3D.fire_clouds`, `WeatherMap3D.get_fire_cloud_for()`, `PhysXFireSpread3D.get_plume_heat()`.
- `fire_age_map` shader global: game years since each patch burnt, for plants that change in the years after a fire.
- Burning buildings and props break apart: a `PhysXBurnable3D` on a `PhysXDestructible3D` loses strength as its timber chars, so a burning wall or shed breaks into pieces that fall and keep their char. `collapse_share`; `PhysXDestructible3D.set_material_remap()`, `set_instance_shader_parameter()`, `get_render_materials()`.
- Burnt crowns lose their needles and leaves: a torched tree stands as bare black branches over a patch of grey ash, and burning twigs drop out of the crown while it burns.
- `PhysXFireSpread3D` `crown_fire` (on): trees and shrubs catch fire from the ground fire under them by Van Wagner's crown fire initiation, flare up, light crowns their flames reach and throw far more embers. Crowns come from `vegetation_field` or `add_tree()`; `foliar_moisture`.
- `VegetationField3D.get_crowns_in()`: the placed crowns over a rect, with their crown base heights.
- Burning pieces keep their flames: once a burning `PhysXDestructible3D` breaks, its `PhysXBurnable3D` moves the flames, sparks and firebrands onto the burning pieces, which smoulder where they land. `PhysXDestructible3D.get_piece_rest_center()`, `get_piece_volume()`.
- Firebrands that land on a `PhysXBurnable3D` pile up (on top faces, at the foot of walls) and heat it until it catches: a burning stand of trees upwind can set a wooden shed alight. `add_brand()`, `get_brand_pile_count()`.
- Burnt grass shows its ash: dry grass and litter burn out to white-grey ash, damp fuel leaves black char, in patches; wind thins the ash and rain darkens it, and grass stubble carries the same flecks. `PhysXFireSpread3D.grass_ash` (on by default).
- Ridge fire whirls follow the weather's terrain wind: lee-slope whirls form where the terrain wind has separated behind a crest, driven by the ridge's wind. `WorldWeather.terrain_lee_at()`.
- Fire whirls behind ridges: on a steep lee slope in a strong wind, `PhysXFireSpread3D` whirls form in the ridge's eddy and run across the slope, lighting the fuel they pass, so the fire spreads sideways along the ridge. `get_lee_whirls_spawned()`.
- Fire makes its own wind: a `PhysXFireSpread3D` draws air in toward its burning front, so grass and `PhysXCloth3D` nearby lean into it and fronts heading toward each other pull together. `fire_wind` (on by default), `get_fire_wind_at()`, `WorldFire.wind_at()`, shader globals `fire_wind_map` and `fire_wind_rect`.
- `PhysXBurnable3D` throws firebrands while it burns (`embers`): lofted by its own plume, carried by the wind and able to start spot fires on the dry ground fuel of a `PhysXFireSpread3D`. `get_firebrand_count()`, `get_spot_fire_count()`.
- Smoke haze: fires' smoke drifts and spreads on the wind as a smoke layer over the weather map; under it the air hazes, the day stays cooler, and the sun dims and turns orange, then red. `WeatherMap3D.smoke_haze`, `WeatherMap3D.get_smoke_optical_depth_at()`, `WorldFire.smoke_sources()`, `PhysXFireSpread3D.get_smoke_rate()`.
- Smouldering ground fire: a passing fire lights the duff under it where it is dry enough, and it burns on for hours of game time with no flame, creeping a few centimetres an hour, glowing dull red in spots under a pale, low-lying smoke. Heavy rain or water puts it out; light rain does not; under unburnt grass it can flame again when the wind picks up. `PhysXFireSpread3D.smouldering`, `duff_depth`, `duff_moisture`, `duff_mineral`.
- Fire-killed trees stand as snags: a fire kills a tree by how much of its crown it scorched against its bark, and over the game years the dead tree's limbs and roots rot, so storms strip it and it falls at its butt, leaving a stump. `WorldFire.crown_scorch_at()`, `TreeWind3D.get_snag_count()`.
- `PhysXFireSpread3D.get_burn_age_at()`: how long ago each patch burnt, in the weather's game years (`WorldFire.burn_age_at()`).
- `PhysXFireSpread3D` `spotting` (on): burning grass, litter and logs throw glowing firebrands that the plume lofts and the wind carries; where one lands on dry unburnt fuel it starts a spot fire ahead of the front. `spotting_scale`, `get_firebrand_count()`, `get_spot_fire_count()`.
- `PhysXFireSpread3D`: fire that spreads over grass and leaf litter at Rothermel's rate (faster downwind and uphill, stopped by wet fuel), chars the ground, glows along the front and drives NVIDIA Flow flames and smoke; hot gas from another Flow (a campfire) can set it alight.
- Water on fire makes steam: a white cloud billows up where water lands on burning or glowing ground or on a burning prop, and along a shore where a fire burns at the water's edge. `PhysXFireSpread3D.get_steam_mass()`.
- A tree that snaps at a fire-charred foot leaves its stump: bark cut 0.3 to 0.9 m up with a short charred break on top, solid to bump into, and the trunk folds over the stump's edge.
- Water puts fires out: `PhysXParticleFluid3D` water lying on a `PhysXFireSpread3D` grid soaks the grass and litter under it, putting out burning cells and stopping the front at wet ground; water on a `PhysXBurnable3D` damps and puts out its flames and drowns the embers on it. `PhysXFireSpread3D.get_doused_cells()`.
- `PhysXFireSpread3D.use_weather_moisture` (on): fuel moisture follows the active `WeatherMap3D`, drying or damping towards the air's equilibrium moisture over `moisture_timelag`, and rain wets the fuel at once, slows the front and puts burning ground out; `get_moisture_at()` reads it.
- Fire whirls: a strong flank of a `PhysXFireSpread3D` running along the wind can spin up a whirl of flame some three flame lengths tall that wanders downwind and lofts firebrands higher, most often when the wind is near the fire's own updraft speed. `fire_whirls` (on by default), `get_whirl_count()`, `get_whirls_spawned()`.
- Rivers over their banks, lakes and any other water soak the ground they cover, and still floodwater leaves its silt and ash behind as fresh mud (`GroundSurface3D.flood_silt`).
- Snow contract: `WorldSnow.avalanche_pressure_at` and `avalanche_peak_pressure_at`.
- PhysXFlow3D `diffuse_transmission` (on by default): in raster, bright gas lets the scene behind it through, blurred, by multiple scattering (two-stream transmittance beyond the direct beam), as under the path tracer.
- `PhysXFlow3D.fire_light`: a burning Flow emitter lights its surroundings with a soft-shadowed light that flickers at the flame's puffing frequency (raster and path traced; under the path tracer it stands in for the flame's glow as a light source, so the ground is lit without noise). `fire_light_energy` scales it.
- `PhysXFlow3D.embers`: glowing blackbody embers rise from a fire as streaks along their flight, `ember_rate` per second.
- PhysXFlow3D `ground_color` (white by default): the colour of the ground under the gas for the raster light scattering pass, scaled by `ground_albedo`. The light the ground sends up into a cloud's base takes it, as path-traced bounces take the ground's own colour.
- PhysXGroundDust3D: tyre dust near the camera simulated in Flow. Every sliding or spinning wheel feeds it; the dust trails, rolls in the car's wake, is pushed by the ground wind, flows round the car body, rests on the ruts and shades the ground by its density. Farther dust stays with GroundDust3D.
- GroundSurface3D `get_height`, so a PhysXFlow3D terrain floor can follow the deformed sand.
- `PhysXFlow3D.ground_albedo`: the sunlit ground under Flow gas lights its underside in raster, as path tracing's ground bounces do; snow powder clouds use 0.8.
- NVIDIA Flow gas now pushes what is in it: `PhysXFlow3D.push_bodies` (on) blows dynamic bodies such as debris and Blast pieces with drag from the gas velocity, `PhysXCloth3D.flow` lets a flag or tarp be blown by a Flow gas and stir it back, and `PhysXFlow3D.sample_velocity()` reads the gas velocity.
- PhysXFlow3D `scattering_albedo` (default 0.9): how much light the gas keeps at each scatter, in both the path tracer and the raster renderer, so very white media such as snow powder (about 0.99) stay bright inside instead of turning grey.
- The raster renderer adds the light of scattering orders past its three multiple-scattering octaves for gas whiter than the 0.9 default, so it brightens with `scattering_albedo` as the path tracer does.
- PhysXFlow3D `terrain_floor` (on by default): the ground under the gas is a solid, laid as overlapping tiles tilted to the slope, so smoke and dust lie on and run down terrain instead of sinking into it. Heights come from `terrain_path` or a downward ray onto static bodies.
- PhysXFlow3D `smoke_buoyancy` (Flow's buoyancy per smoke): below 0 the smoke is heavier than air, for dust and cold fog.
- MPM fluid makes spray, foam and air bubbles (Ihmsen et al. 2012 diffuse particles; bubble drag and foam surface hold from Wretborn et al. 2022). On by default through `foam_enabled`.
- Cloud sitting on wooded ground drips from the trees: leaves catch fog droplets into the canopy, which drips onto the ground and wets it (`WeatherMap3D.fog_drip`, `WorldWeather.fog_water_at`, `fog_drip_at`).
- Hot air above fires bends light: the background shimmers through the plume, from the simulated temperature, in raster and path tracing (`PhysXFlow3D.heat_shimmer`).
- Heavy bodies and vehicles break through lake ice too thin for their weight (white and thawing ice give first); the hole freezes over again.
- Feet, wheels and other moving bodies kick up the fallen leaves they pass through.
- Fallen leaves float and drift on ponds and rivers, lie on snow, get buried by fresh snow and reappear when it melts (`LeafFall3D.leeway`).
- Snow drifts into the lee of rocks, walls, buildings and vehicles and blows off their tops (`SnowCover3D.drift_obstacles`); blown sand drops behind them (`GroundSurface3D.obstacle_lee`).
- Boulders and vehicles block the wind (`RockField3D.wind_obstacles`, `PhysXVehicle3D.wind_obstacle`); any node in the `weather_shelter` group can list its own boxes with `get_wind_obstacles(rect)`.
- Weather contract: `WorldWeather.obstacle_height_at` and `obstacle_lee_at`.
- Feet and tyres leaving mud print it on the firm ground beyond, thinner with each step, and water washes it off (`WorldGround.track_mud`, `WeatherMap3D.get_body_mud`).
- Shader built-in `RAY_FOOTPRINT` (fragment): the width of the path tracer's ray cone where it meets the surface, in world units; 0 in raster. Rivers use it to filter ripples finer than a pixel into roughness, as raster does.
- Rain and snow wash smoke haze and dust out of the air (`WeatherMap3D.smoke_washout`, `PhysXFlow3D.rain_washout`).
- `RainFall3D`: falling rain round the camera from the weather map, streaks slanting in the wind, splashes, rain haze, and drips from leaves and eaves that keep falling after the rain. `max_drops`, `splashes_enabled`, `drips_enabled`, `haze_enabled`, `shelter_enabled`.
- Raindrop rings on the sea, lakes, pools, rivers, live water and the ground's puddles, raster and path traced; heavy rain dulls reflections (water shader uniform `rain_rings`).
- `WeatherMap3D` rain on leaves: the canopy holds rain and drips after it stops (`canopy_water_enabled`, `canopy_storage`); rain and snow split by the air's temperature at each height (`get_rain_share_at`, `get_rainfall_at`, `get_snowfall_at`).
- `BaseMaterial3D` `wetness_canopy`: leaves stay wet while the canopy holds water, with beads on their upper faces; on for trees and falling leaves, and ground plants.
- `WeatherRunoff3D`: rain runs off the land over kilometres; ground with runoff over it stays wet.
- `PhysXRiver3D` `rain_response` (on): rivers rise and run faster with the runoff; lakes with `level_from_flows` rise with it.
- Two-layer refraction in Forward+ (`rendering/transparency/refraction_second_layer`, on): water and glass that read the screen texture (Transmission, refraction) now show the refracting surface behind them, such as the pool under a falling stream or a second pane of glass, instead of only the opaque scene. Depth peeling, one extra layer; only frames with such a surface in view pay.
- Shrub and tree leaves near the sea brown one by one with the salt the spray leaves on them, as grass does (`weather_spray_map`).
- Sea salt melts shore snow and lake ice below freezing, as much as its brine can hold, and meltwater carries it away (`SnowCover3D.salt_melt`).
- `PhysXSeaCaveWater3D`: the sea inside sea caves, from `CaveNetwork3D`'s surge (level, width and current along the passage) with the river's surface, from the arch in. Short waves come in at the mouth and reflect off the walls and the back of the cave (Water Surface Wavelets, Jeschke et al. 2018), as real geometry; steep faces break white. `CaveNetwork3D.sea_water` (on) makes one.
- `PhysXWaterSurface3D` water clipping: nodes in the `water_clippers` group give capsules where the sea cuts itself out and cross-fades to another surface, in raster and the path tracer. `CaveNetwork3D.get_water_clip_capsules()` cuts the sea along sea-cave passages; `get_sea_cave_water()` gains `along`, `covered_from` and `wave_rms`.
- The sea surges into sea caves: waves run up the passage, growing as it narrows, with currents in and out, and storm swells drive air up blowholes (`CaveNetwork3D.sea_surge`, `get_sea_cave_water`, `get_blowhole_spout`, signal `blowhole_spout`).
- Sea spray from breaking waves wets the shore downwind and leaves salt that rain washes off; cliff faces catch the spray, and salted grass browns at the tips (`WeatherMap3D.sea_spray`, `WorldWeather.sea_spray_at`, `salt_at`, `WorldWater.breaking_at`).
- Lying snow lays grass, crops and plants down under its weight and buries short grass; soft stems give first.
- The weather contract also answers day of year, season, cloud cover, water and surface temperature, ground height and time scale; ground, snow, vegetation and water now read the weather only through it.
- Terrain generator: coastal dunes. The beaches feed wind-blown sand inland on a fine grid; marram builds foredunes along the plant line, and blowouts and parabolic dunes open where the cover fails (`dunes_enabled`; `get_dune_height()`, `get_dune_stability()`).
- Terrain material: scree shows a gravel and fines matrix between its stones up close (`rock_detail_enabled`).
- Sea cliffs grow a patchy white salt crust where the spray's salt dries on rock and scree, gone while the stone is wet (TerrainMaterial3D `salt_bloom`).
- Terrain generator: desert sand seas. Transverse ridges, barchans, linear and star dunes grow over `sand_sea_rect` from the wind regime and the sand cover (`sand_seas_enabled`, `sand_sea_kind`, `sand_sea_depth`, `sand_sea_wind_angle`, `sand_sea_scale`; `get_sand_sea_height()`).
- Avalanches break trees: `TreeWind3D.avalanche_enabled` reads `WorldSnow.avalanche_peak_pressure_at`; crowns snap from about 3 kPa, stems by diameter (10-20 kPa at 30-50 cm), falling downslope. Felled trees cut their leaf area out of the weather map's canopy.
- Bark beetle kill: `TreeGenerator.beetle_years` grows a conifer from green attack (resin beads, boring dust) through red needles to a grey snag shedding its bark, then a broken top; mesh meta `tree_beetle_years`. Stand `STAND_BEETLE_SPRUCE` with grey snag spots, a red front at their edges and living spruce between, sized by `beetle_outbreak`.
- `TreeGenerator.get_living_meshes()`: a species' meshes without its fire band variants.
- Trees: beech and birch boles carry dark branch-scar chevrons below their shed limbs, a callus knob over the scar and two arms fading down round the stem, longer and flatter the older the scar (`bark_chevrons`).
- Five broadleaves: aspen, black poplar, wild cherry, hornbeam and sweet chestnut, each with its own form, leaves, bark, seasons and habitat (`TreeGenerator.PRESET_ASPEN` to `PRESET_SWEET_CHESTNUT`).
- Poplar leaves tremble in the lightest air.
- Hornbeam stems and limbs are fluted.
- Wild cherry flowers white in April.
- Cork oak: `TreeGenerator.PRESET_CORK_OAK`, an evergreen oak whose short bole divides into a few thick spreading limbs under a broad crown, with small toothed leaves grey below and deep, spongy grey-buff cork bark.
- Leader split for decurrent crowns: the stone pine and cork oak leaders stop at a set height and a few limbs take over, clear of side limbs until near the crown.
- Cork harvest: `TreeGenerator.cork_stripped_years` strips the cork oak's bole and limb bases, bright orange when fresh, red-brown within weeks, near black within the year, then new cork growing back, with a step up to the virgin cork above. By default each tree picks a stage of the nine-year cycle.
- Date palm: `TreeGenerator` preset `PRESET_DATE_PALM` with a trunk of cut leaf bases in their diamond lattice, arching feather fronds with V-set pinnae and basal spines, a dead-frond skirt and offshoots on wild palms, flowers and hanging date bunches by season; stand `STAND_DATE_OASIS` (tended rows, wild clumps, olives beneath).
- Fire band variants: placed tree species carry one variant per fire band with mesh meta `tree_fire_years` and `tree_fire_band`, which `VegetationField3D` picks by burn age. `TreeGenerator.fire_variants`.
- Desert shrubs: `TreeGenerator.PRESET_CALLIGONUM`, `PRESET_WHITE_BROOM` and `PRESET_SALTBUSH`, taken by `make_shrub_species` in every shrub form, for sand seas and their interdune flats.
- Calligonum grows leafless jointed green twiglets and red bristly fruit in spring that dry to buff; white broom hangs silvery switches and flowers white from January to March; saltbush is a dense, matte silver-grey bush.
- Shrubs carry an extra first level of detail a few metres out, so the full meshes cost a third to a half less in the middle distance.
- Calligonum flowers white from February to April; saltbush carries pale fruiting panicles above its leaves from late summer into winter.
- Dwarf fan palm: `TreeGenerator.PRESET_DWARF_FAN_PALM`, a clump of short shaggy, fibrous trunks or stemless crowns with plicate fan leaves on spiny green petioles and a skirt of dead leaves; spring flowers among the leaf bases and dates on female plants. In the five `make_shrub_species` forms.
- Trees a fire killed outright redden over the first months and drop their whole crown over a year or two; the same mortality rule (`VegetationFire::tree_killed`) drives the leaf shader and `TreeWind3D`'s standing snags.
- Trees regrow after fire: `TreeGenerator.fire_years` gives a charred snag with its species' response, epicormic shoots (cork oak), a basal clump (holm oak and most broadleaves) or seedlings on the ash bed (maritime pine, rockrose). `get_fire_response()`.
- Trees: wind-flagged trees with limbs only on the lee side and krummholz mats at the tree line (`wind_exposure`, `krummholz`; flagged and krummholz forms in the subalpine stand).
- Trees: trees on banks with roots running down the slope and exposed by erosion (`ground_slope`, `bank_erosion`).
- Gorse and broom: `TreeGenerator.PRESET_GORSE` and `PRESET_BROOM`, with gorse spines and pea flowers and pods as geometry, green twigs, and flowering seasons (`TreeGenerator.get_bloom`, `get_leaf_season` "flowering"). `TreeGenerator.make_shrub_species` returns a placeable species in young, mature, old, thicket or wind-pruned cushion form.
- Hedge shrubs: `TreeGenerator.PRESET_BLACKTHORN`, `PRESET_FIELD_MAPLE` and `PRESET_DOG_ROSE`, and `make_shrub_species` takes them with hawthorn and hazel.
- Cut hedges: `TreeGenerator.SHRUB_HEDGE` grows a two-metre segment of flailed field hedge, A-shaped or boxed, leafy to the ground, for laying end to end along field edges.
- Hedge seasons: hawthorn white in May with haws in autumn, blackthorn white on bare twigs in March and April with sloes, dog rose pink in June with scarlet hips.
- Holm oak: `TreeGenerator.PRESET_HOLM_OAK`, an evergreen oak with a short bole under a dense, dark, rounded dome, small leaves grey below and dark grey cracked bark.
- Ice storms load trees: a new weather contract reports glaze (clear ice from freezing rain), and wind-rigged trees carry its weight, bow and break under it, and stand up again after the thaw (`TreeWind3D.glaze_load_enabled`).
- Freezing rain: a warm layer aloft (`WeatherMap3D.warm_nose_temperature`) turns rain over sub-zero ground into glaze, accreted by Jones' ice model and melted in a thaw (`get_glaze_at`).
- Clear ice on surfaces glazed by freezing rain, raster and path traced (`BaseMaterial3D.glaze`, on for trees).
- Glazed trees as geometry: a clear ice coat round twigs and limbs and icicles under them, seen alike by raster and the path tracer (`TreeGenerator.glaze_mm`).
- Near trees in a vegetation field grow the weather's glaze as ice geometry, in bands, and swap as it builds or melts.
- After an ice storm: broken tops, stripped crowns and birch bent over in arches, by species, and a northern hardwood stand after one (`TreeGenerator.ice_storm_years`, `STAND_ICE_STORM`).
- Trees: deadwood meshes expose `tree_deadwood_features` (peel edges and drying checks, 8 floats each) for placing growth along them.
- Wild olive: `TreeGenerator.PRESET_WILD_OLIVE`, a gnarled small tree of crooked stems under a dense, pale silver grey-green crown, narrow leaves silvery below.
- Maritime pine: `TreeGenerator.PRESET_MARITIME_PINE`, a tall pine with a long clear bole, a dense irregular rounded crown, long stout needles and thick purplish plated bark.
- Carob: `TreeGenerator.PRESET_CAROB`, an evergreen with a short trunk under a broad dense dome of glossy paired leaflets.
- Needle cast after fire: needles and leaves a fire killed brown fall over the following year, most in the first months, read from the new `fire_age_map`.
- Trees: roots draped over a generated boulder beside the stem (`root_stone`).
- Trees: bank forms with slope variants for stream banks and gorge sides, in the riparian and beech stands.
- Mediterranean scrub shrubs: `TreeGenerator.PRESET_GUM_ROCKROSE` (white blotched flowers, capsules, summer leaf fold), `PRESET_KERMES_OAK` (spiny-toothed leaves, acorns in spiny cups) and `PRESET_LENTISK` (paripinnate leaves, red to black drupes on female plants), in the five `make_shrub_species` forms.
- Stone pine parkland: `TreeGenerator.STAND_STONE_PINE`, open-grown umbrella pines about 11 m apart with saplings, snags and logs.
- Stone pine: `TreeGenerator.PRESET_STONE_PINE`, the umbrella pine. A dome when young, then a flat crown as wide as the tree is tall on a clear bole, with long needles in tufts, plated red-brown bark and cones of three ages on top of the crown.
- Storms break and throw rigged trees: `TreeWind3D` checks each limb's wind moment against its wood (species MOR, knots) and each tree's against its root anchorage; broken limbs and thrown trees (with their root plate) fall as hinged PhysX bodies under the wind, and on landing become Blast destructibles that snap where they hit and settle under their own weight, a thrown tree lying propped on its plate standing on edge. `TreeWind3D.storm_*`, `TreeGenerator.storm_kit`, signal `storm_damage` with the root pit.
- Vegetation: alpine and scree plants above the treeline: moss campion cushions, purple saxifrage, mountain avens, curved sedge, dwarf willow and alpine toadflax, in fellfield, grassland, snowbed and scree sets above the map's own treeline (growing-season temperature, `PlantSpecies.RULE_SEASON_TEMPERATURE`) and sorted by curvature, sun and talus (`PlantGenerator.make_alpine`).
- Vegetation: bog and moor plants as real geometry: sphagnum hummocks, cotton grass with soft cotton heads, bog asphodel and sundew, sorted into hummocks and hollows on wet, level ground (`PlantGenerator.make_bog()` for blanket bog and valley mire).
- Vegetation: cereal crops on `GrassField3D` (`crop`: wheat, barley, oats, rye). Each tiller is real geometry (stem, four leaves, the ear with awns, or the open oat panicle) in drill rows (`row_spacing`, `drill_angle`), one to three tillers to a plant, swaying in the same wind as the grass. `crop_stage` (BBCH) sets the look: leafy rows, stem elongation, heading, ears and straw ripening from green to gold with the leaves dying from the bottom, nodding barley, and stubble after harvest.
- Vegetation: crop fields (`GrassField3D.fields`): polygons each with its own drill angle, fixed stage or sowing calendar (`sown_day`, `harvest_day` with a season source; `crop_calendar_stage`), tramlines (paired unsown wheel tracks) and a headland drilled round the edge. The ground under them shows the drilled soil and darker wheelings, and past the tillers the crop's colour and cover at its stage (`crop_canopy_look`), so fields read as a patchwork to the horizon.
- Vegetation: dune plants follow the terrain generator's coastal dunes, embryo and mobile dune sets on moving sand and fixed dune sets where marram holds it (`PlantSpecies.dune_range`, `RULE_DUNE`).
- Vegetation: dune plants as real geometry: marram, lyme grass and sea holly in embryo, mobile and fixed dune sets by distance to the water (`PlantGenerator.make_dunes()`). Placed plants now shelter blowing sand and trap it when their field is in a ground surface's `wind_shelters` (`VegetationField3D.sample_lateral_cover()`, `PlantSpecies.wind_frontal_area`).
- Vegetation: plants past their range can be drawn as real volume instead of ground colour, a porous voxel shell of their leaf cover out to 150 m (`VegetationField3D.far_volume`, on by default; `far_volume_range`, `far_volume_cell`).
- Vegetation: Atlantic heath: bell heather, cross-leaved heath and crowberry as real-needle dwarf shrubs, in dry, wet and maritime heath sets placed by rainfall, winter cold, wetness, shade and coast exposure (`PlantGenerator.make_heath`).
- Vegetation: ground mist as a real gas in the near field, pooling in hollows, parting round walkers and burning off as the sun climbs (`VegetationMist3D`, needs Flow).
- Vegetation: pollen puffs when bodies brush grasses and other wind-pollinated plants in flower (`VegetationPollen3D`, needs Flow; `PlantGenerator.get_pollen_release`).
- Vegetation: salt marsh and shingle plants: glasswort, cord-grass, sea purslane, sea aster, sea lavender, sea kale, sea campion and yellow horned-poppy, zoned up the tidal frame in pioneer, mid and high marsh sets on sheltered, level coasts and a shingle set on the backshore (`PlantGenerator.make_saltmarsh`; `PlantSpecies.RULE_TIDE_HEIGHT`, `RULE_COAST_EXPOSURE`, `RULE_BEACH` from the terrain generator's sea).
- Vegetation: Mediterranean scrub: rosemary, French lavender, thyme and sage-leaved rockrose as real-leaf dwarf shrubs, in garrigue and maquis sets placed by winter cold, rainfall, aridity and soil depth, with the rockrose's summer drought; `PlantGenerator.apply_scrub_rules` places other scrub shrubs in the same sets (`PlantGenerator.make_scrub`).
- Vegetation: waterside and wet-stone plants as real geometry: marsh marigold, meadowsweet and butterbur placed by distance to water (`PlantGenerator.make_banks()` for streams, lakes and waterfall spray), aquatic moss trailing downstream on river stones and liverworts on wet tops (`PlantSpecies.stone_top_water`, `follow_flow`).
- Vegetation: heath and scrub sets now include the woody shrubs: gorse and broom on dry heath, wind-clipped gorse cushions on cliff-top heath, lentisk, kermes oak, gum rockrose and dwarf fan palm in the maquis, kermes oak cushions, young fan palms and young gum rockrose in the garrigue, each in clumps under the set's rules (`PlantGenerator.make_heath`, `make_scrub`, `shrubs`).
- Vegetation: worn paths: give `VegetationField3D` and `GrassField3D` a list of `paths` (centre line, width, walkers a year) and plants thin out and grow shorter by how well their growth form resists trampling, leaving bare trodden soil in the middle and tough rosettes and grasses at the edges (`PlantSpecies.trample_resistance`, `GrassField3D.trample_resistance`).
- Characters print the ground, snow, sand and mud with their feet, step by step, not with their whole capsule; grass, plants and water still feel the body (`WorldContracts.describe_body` walker, meta `world_contact_feet`).
- A river's current lays the grass and plants under it downstream, flat in a fast stream, from the water contract's surface and flow.
- Fire melts the snow under it, whatever the air temperature (`WorldFire`).
- Plants, hair, falling leaves, ash and embers, smoke and rock dust lean into or drift toward a nearby fire's indraft.
- Tyres lose grip on lying snow (more on powder and slush, near ice on a hard crust) and on bare lake ice; chains and lugs win it back.
- `PhysXWaterSurface3D.foam_shade` (0.8): whitecaps and shore foam shade the bed under them in raster through the caustics map (bubbles send most of the sun back up), as the path tracer already did from the foam itself.
- `WorldWater.wave(position, direction, height, period, width)`: an impulse wave in metres and seconds from a landslide, avalanche, calving or rockfall entering the water. `PhysXWaterSurface3D.submit_wave` seeds it in the ripple solver as one crest and trough with its long-wave velocity, so it runs one way at sqrt(g h), runs up the shores and reflects; water without a solver takes a splash of the crest's height.
- `OasisSprings3D`: a still pool at each desert spring (`springs`, x, level, z and discharge in L/s, or `spring_source` with `get_springs()`), sized by its water balance (area = discharge / (evaporation + seepage), evaporation from the climate by Hargreaves or set) with a noise outline unique to each spring, and sand boils at its vents whose floors heave with the flow while the surface above lifts and sheds rings.
- Rivers carry a load entering by one bank as a plume along that bank, spreading across by turbulent mixing over tens of channel widths, instead of mixing it at once through the whole channel.
- Rivers drop silt and ash where the water slackens (by the banks, in pools); `PhysXRiver3D.get_sediment_deposit_at()` and `WorldWater.sediment_deposit_at()` give what has settled, in kg/m².
- `WorldWater.add_sediment_mass(position, mineral_kg, ash_kg)`: a load dropped into water at once (a debris flow's or landslide's fines), mixed through the river section or lake it lands in and capped at hyperconcentrated flow (1000 kg/m³). Debris-flow dumps through `add_sediment` were held as a rate for two seconds, doubling their mass, and a second dump into the same river section on a later frame replaced the first.
- Rivers and lakes carry suspended sediment from `WorldWater.add_sediment` (silt and clay, ash and char): rivers move it downstream at their speed, lakes mix it through their volume and let it settle (silt in about half a day, ash in about two). Silt browns and ash greys the water alike in raster and path tracing; raster turbid water now adds multiple scattering so it reads opaque and tan like the path-traced water.
- Cold air pools: on clear calm nights valleys and hollows fill with colder air, so frost and fog form there first while slopes above stay milder (WeatherMap3D `cold_pools`).
- WeatherRunoff3D starts post-fire debris flows: burnt steep basins in intense rain (USGS M1 likelihood, Gartner volume) emit `debris_flow(event)` on the runoff and on `WorldGround`, with the outlet, volume, channel path, bankfull widths and burn severity. `debris_flows` (on by default), `debris_flow_threshold`.
- Drainage winds: on clear calm nights cold air runs down slopes at 1 to 3 m/s a few metres above the ground, in every wind reader and the plants' wind (WeatherMap3D `drainage_winds`).
- Rime: ground standing in freezing cloud grows white rime on the windward side of plants, rocks and cliffs, thicker the longer it lasts, and sheds it in a thaw (WeatherMap3D `rime`).
- WeatherRunoff3D answers the runoff contract (`WorldGround.add_runoff_load`): loose silt and ash put on the land move down the drainage with the water and enter rivers and lakes through `WorldWater.add_sediment`.
- Valley fog wets the trees standing in it (fog drip) and rimes them below freezing, through `WorldWeather.fog_water_at`.
- Valley fog: saturated cold air pools fill with a flat white fog sea on clear calm nights that lifts after sunrise or in a wind; the top is readable through `WorldWeather.valley_fog_top_at` (WeatherMap3D `valley_fog`).
- Bodies coming out of water, wet mud or puddles leave wet footprints and tyre tracks that fade along the way, drip where they stand, and dry with the weather (`WeatherMap3D.wet_tracks`, `get_body_water`).
- Avalanche paths keep the forest young: where a dense avalanche breaks trunks the stand starts again, so tracks fill with pioneers (SnowAvalanche3D `stand_damage`).
- A beaver dam node: when it fails, its drained pond becomes a beaver meadow that plants and trees grow into (`WorldBeaverDam3D`).
- Ground standing at the low cloud's base fills with hill fog that wind and sun do not clear, and lightning starts at the local cloud base (`WorldWeather.cloud_base_at`).
- Smoke and dust take the colour of the ground under them in the light it bounces up: terrain layers, wet ground and snow (`WorldGround.albedo_at`, `WorldSnow.albedo_at`, PhysXFlow3D `ground_from_world`, on by default).
- The air follows the season and the time of day: coldest near dawn, warmest in the afternoon, humid at night (`air_follows_climate`, `air_daily_cycle`, `daily_temperature_range`), and cools with height.
- The sun dries wet ground faster than shade or night (`sun_drying`).
- Clear calm nights bring dew, and hoarfrost on terrain, grass, plants and rocks below freezing; the morning sun melts it (`dew_and_frost`).
- Radiation mist comes sooner and thicker over wet ground, and steam fog rises off lakes and the sea when the air is much colder than the water (`WeatherMap3D.get_water_temperature`).
- Fire dries the ground under it, and fuel lying on wet or dewy ground burns slower (`WorldFire`).
- Lightning strikes tall things first and can start fires in dry fuel (`lightning_ignition`).
- Tyres lose grip on wet, flooded and icy ground, and aquaplane at speed.
- Every moving body bends grass, plants and branches, presses ground and snow and ripples water (`physics/3d/world_contact/auto_join`).
- World contracts: weather, water and body contacts each behind one interface (`WorldContracts`), with script stand-ins for tests.
- Weather rain follows the terrain climate: more on windward slopes, a rain shadow behind ridges, and rain or snow by each spot's own height, so summits snow while valleys rain (WeatherMap3D `orographic_rain`).
- `WorldGround.relief_at` and the `terrain_relief_map` global: the drawn ground's relief over the heightfield, for anything placed on the terrain.
- Tyres take their grip on snow from the snow itself where it keeps one: packed roads and snow polished to ice by traffic (`WorldSnow.grip_at`).
- Drained beaver ponds are a stand disturbance, so meadows can grow where a dam failed (`WorldStand.DISTURBANCE_BEAVER`).
- Deer browsing pressure over the stand, with how high the animals reach, so plants and young trees can show browsing (`WorldStand.browse_at`, `browse_reach_at`).
- A stand-age contract gives the years since the last stand-replacing fire, windthrow or beetle kill at any point, for forests that age (`WorldContracts.get_stand`, `WorldStand.stand_age_at`).
- Game trails are published once for every system to read along their tread, and boar rooting is a field-layer disturbance (`WorldStand.set_trails`, `trail_at`, `rooting_age_at`).
- Lightning that strikes a tree crown is remembered by the stand, so forests can show the struck tree's scar for years after (`WorldStand.strike_age_at`, `WeatherLightning3D.record_tree_strikes`).

### Changed
- Destructible pieces are drawn as instances of per-chunk meshes built once per asset and shared, instead of a merged mesh rebuilt on every spawn and split: no more frame stall when a large asset such as a tree spawns or breaks.
- `PhysXCloth3D`: wind gusts vary across the cloth so it ripples and billows instead of tilting as a plane; cloth no longer falls through thin colliders such as ropes and rails; new `move_pins()` hangs cloth from pegs set closer than it is wide, so it falls in swags.
- Wet prints and drips read their contact wetness with a Catmull-Rom filter in raster and path tracing, so small prints keep a rounded shape instead of square texels.
- PhysXDebrisSurge3D finds the scene's GroundDebrisFlow3D by itself and its lead surge rides the mud body's front; later surges run behind it.
- Flow's default `heat_ramp` is a blackbody: flames have a bright core and sharp tongues, and cooler gas reads as smoke instead of an orange glow.
- The path tracer honours a light's `light_volumetric_fog_energy` when it lights participating media.
- PhysXFlow3D raster smoke lighting is on by default: `light_scattering`, `ambient_scattering`, `sun_scattering`, `cast_shadows` and `scattering_temporal` now default to true.
- PhysXParticleFluid3D (MPM): `mpm_auto_colliders` is on by default and takes static bodies and every shape of a body, so a basin built from boxes holds the fluid without listing it. Scenes that relied on only the bodies in `mpm_colliders` (for example particles meant to fall through a ground body and be handed to a height field) should turn `mpm_auto_colliders` off.
- MPM fluid spray is drawn as clear water drops; foam and bubbles stay white.
- PhysXParticleFluid3D's default liquid is clear water: transmission with refraction (n 1.33), Fresnel reflection and depth tint, instead of a translucent blue tint.
- MPM fluid surface is smooth: a wider surface kernel (three particle spacings) and normals continuous across grid cells, so it no longer shades as facets.
- PhysXParticleFluid3D defaults to `render_mode` Isosurface (a real mesh that casts shadows and shows in reflections and path tracing) with `foam_enabled` on.
- `GroundDust3D` and `CaveNetwork3D` follow the current camera when `follow_target` is empty, as the weather, the ground and the other world systems do. A cave network whose meshes were built with `build_meshes()` keeps them and does not stream.
- PhysXGroundGrains3D: each thrown speck keeps at least about 1.3 pixels across, so the spray arc reads from several metres away.
- Needle ice, frozen ground and puddle ice follow the ground's surface temperature instead of the air's, so clear calm nights with the air just above freezing grow needle ice and skim puddles over.
- PhysXHair3D.cards_enabled is on by default: far hair draws as thick strands per clump in raster.
- Path tracing: transmissive surfaces pick reflection or transmission by the Fresnel at the view angle instead of the head-on reflectance, so water and glass seen at grazing angles show their sky reflection without the bright speckle (about a third of the noise at the same sample count).
- PhysX: static bodies entering a space go into the scene together before the next step or query, through a pruning structure for large batches, so streaming in a region of static bodies costs less in its first frame. Adds `docs/bench/physx_openworld`.
- PhysX: bodies with the same friction and bounce share one material instead of each making its own, so streaming a region no longer costs a step that grows with the world (5,000 statics: 3.5 to 8 ms less), and large worlds stay under PhysX's 65,535-material limit.
- PhysX: a streamed region's static bodies enter the scene over a few steps (`physics/physx_3d/streaming/statics_per_step`, 1024), those near moving bodies and those a query reaches first, so nothing falls through or is missed; static removals go out in one batch. The first-step hitch of a region drops by up to a third with up to 10,000 statics loaded.
- Terrain features are on by default: `dunes_enabled`, `glacial_enabled`, `glacial_converged`, `tile_erosion_enabled`, `strata_enabled`, `guide_preview`, `ice_worn_detail`, `deposition` 0.5, `moraine_age` 2000, `scree_age` 10000, `meander_age` 3000, `outwash_age` 10000 (TerrainGenerator3D); `cliff_relief` 0.6 m (Landscape3D); `rock_detail_enabled`, `cliff_self_shadow`, `far_blend_enabled`, `far_start_from_vegetation` (TerrainMaterial3D). Turn them off where a project needs a faster generate.
- Terrain material: sand the generator laid (beaches, dunes) holds on slopes up to the cliff slope instead of turning to soil past 40 degrees.
- Terrain material: scree stones stand out of the gravel bed in parallax, with their own sun shadows under the raster and the path tracer (`rock_detail_enabled`).
- Terrain material: cliff faces break into irregular blocks with long bedding cracks and broken cross joints instead of a regular panel grid (`rock_detail_enabled`).
- Terrain material: thin soil on talus shows as grey debris, and the layer blend widens with distance so far slopes do not shimmer.
- Terrain generator: talus cones stay smooth, with no erosion gullies or ledges cut into them.
- Trees build faster: bark and moss textures are made once and shared by every tree generator, so forest-grown trees build about a third faster with identical results.
- Trees: smooth bark (beech) shows cross-stem creases, lenticel striations and a fine grain instead of soft blotches.
- Far tree meshes leave out the displaced bark relief (drawn by the near mesh) and the centimetre fruit; far date palm pinnae are two triangles. A wild date palm clump far mesh is about half the triangles.
- Trees: birch lenticels vary in length, thickness and spacing and gather in bands, instead of one dash per grid cell.
- Trees: far meshes of every broadleaf start at the lighter leaf level, with full leaves only near (`far_lod1`, on).
- Trees and shrubs: far meshes leave out leaf twigs under a millimetre, which only the near mesh draws (a mature lentisk's far bark 929k to 233k triangles).
- Gorse gets a near mesh like the other shrubs; its far mesh starts at the lighter spine, flower and pod levels (2.5M to 1.2M triangles mature).
- Trees grow about 15 % faster with identical meshes: leaf LODs, skin padding and needles fill their arrays in one pass, and the crown fire heights use selection instead of sorting every leaf vertex.
- Trees: krummholz mats grow as low domed cushions on many layered stems, their limbs under a skin of short needled shoots, instead of a flat table on one stem.
- Trees: the tree line follows the growing season's mean temperature (6.4 C), matching the alpine plants; flagged spruce sit just below it and krummholz mats just above.
- Trees: drying checks on logs are real splits, the bark's edge where it peels is a step of its real thickness, and moss margins are ragged and thin.
- Trees: full-leaf near meshes (`near_leaf_range` 60 m) and full-needle near meshes (`near_needle_range` 30 m) are on by default.
- Cork oak: a denser, darker crown on four thick limbs clad in cork, and virgin cork as thick ridges broken into blocks by wide, deep, dark fissures; the bole sweeps into its four limbs as one surface, its bark running on unbroken.
- Gorse and broom flowers and pods thin out with distance, as leaves do, with their colour kept: a mature gorse in summer draws about 70k crown-part triangles far off instead of 1.7M.
- Stone pine: the leader splits at about 5 m into three limbs that divide again, under a darker, denser cushion crown; stouter bole, long offset grey-brown bark plates with red-brown fissures (`BARK_PINEA`), and full-size cones set behind the needle tufts.
- Vegetation: the far colour that replaces plants past their range is matched live to the plants on screen, so the ground no longer lightens into a ring where the cover plants end (`VegetationField3D.far_match_view`, on by default).
- Vegetation: floor plants keep clear of a stone a tree's roots grow over (mesh meta `tree_root_stone`).
- Vegetation: fallen leaves and twigs no longer land on open ground far from any crowns (the litter rule's fade ran below zero litter).
- Ground mist sets its emitters only when a cell's place, density or wind changes, instead of seven property writes per cell every frame; the mist is unchanged.
- Ground mist reads wet ground, water and steam at each cell twice a second instead of every frame, and takes the ground height from the world's ground before casting physics rays.
- Oasis pools hold clear spring water (near pure water's absorption, little scattering), in raster and path tracing alike.
- An oasis spring whose ground is not hollowed below its level gets no pool or boils, with a warning; `TerrainGenerator3D.carve_oases()` carves it.
- Sand boils vary in size from vent to vent, with darker throats.
- Raster rivers mirror their banks where the screen shows them (screen-space reflections over the sky), instead of open sky everywhere; muddy water at grazing views keeps its brown instead of a blue-grey veil. Shallow turbid water shows its bed through the diffuse attenuation (Maritorena 1994).
- Flood water reads brown like milky coffee rather than pale milk: silt scatters forward as measured (backscatter about 2%) and carries the humic matter washed off with it; raster calibrated against the path tracer at flood loads.
- The contact wetness map dries and re-encodes only the area that is wet, and skips the GPU upload when nothing visible changed; the picture is unchanged.
- Wet tracks check each walking body's own-mud parts only when its children change, not every frame.
- Freezing rain reaches every reader of the rain share (cloth, hair, the snowpack, falling sleet), and a deep cold layer under the warm nose refreezes it to ice pellets instead (`WeatherMap3D.warm_nose_base`, Bourgouin 2000); `ice_storm_glaze` holds a glaze for tests.
- The weather map's cloud cover noise takes its per-update constants once instead of per cell (16 % fewer instructions, identical cover), and its texture packing runs on worker threads instead of the main thread.
- Ground cover reads for sand armour take the whole grid in one call under one lock, and stand-age lookups check one 64 m grid cell instead of every event; results are unchanged.

### Fixed
- `apply_radial_damage` on a scaled destructible: the point and radii are now in the asset's units.
- Blast pieces no longer fly apart and through the ground when they break: `PhysXDestructible3D.max_depenetration_velocity` (3 m/s by default) caps how fast PhysX pushes touching pieces apart, so projects need not cap it globally.
- Wet prints, drips and splashes stay drawn round the camera when the weather window follows a far-off anchor or vehicle.
- Raster Flow gas tinted by its colour once where the path tracer tints every scatter: near-white gas such as snow powder drew grey and coloured gas too light and neutral. Each multiple-scattering order now takes the colour to its own power, and the octaves are refit to the path tracer.
- The far side of a dense, bright Flow cloud lit from behind it no longer draws bright white in raster: its deep scattering orders fade by diffusion theory's transport coefficient.
- Screen-space refraction in MPM fluid no longer shows objects in front of the liquid.
- The foot of raster Flow gas lying on the ground drew dark: its sky and ground light now count the open side toward the camera, not only the gas straight above and below.
- Raster shadows of bright, forward-scattering gas (snow powder, steam) on the scene were too dark: they now pass the forward-scattered light on (delta-Eddington scaling); dark smoke keeps its full shadow.
- Flow gas lit from behind the camera drew mid grey with dark blotches in raster where path tracing shows it near white: multiply-scattered light now spreads evenly instead of leaning forward like single scattering.
- PhysXFlow3D: vehicles (bodies whose shapes live on their own PhysX actor) are solids for the gas again, from their CollisionShape3D children; before, they were skipped with an error each step.
- Path-traced smoke, dust and powder came out grey and see-through when the scene's bounce count was low: scattering inside gas now has its own budget instead of using up the surface bounces.
- Thin edges of Flow gas in raster were as bright as thick gas; light scattered more than once now needs gas around it, so wisps stay see-through as under path tracing.
- Raster Flow gas: a ground sample off the sun's shadow map no longer counts as fully sunlit and outvotes a shadowed one.
- A dark seam along the base of raster Flow gas lying on sloped ground: the snow under the gas was lit as if it lay at the gas box's floor, under the slope, and gas cut by the ground showed unlit where the scattering march ended a step early.
- Raster Flow gas: the scattering march's step budget counts only samples in gas, and the upsample no longer blends texels with no gas in as black.
- A build with Flow support no longer crashes when libnvflow.so / libnvflowext.so (or nvflow.dll / nvflowext.dll) are missing or incomplete: it reports one error and the Flow nodes stay inert.
- The lit face of large, thin-per-metre Flow gas (snow powder) drew grey in raster lit from the camera's side: multiple scattering now looks at the gas along the sun's whole line through the point, not only toward the sun.
- Flow smoke and dust in the raster renderer were lit up to 1.75 times brighter than under path tracing, which flattened their self-shadowing; they now match.
- Flow smoke and dust in the raster renderer now match the path tracer under a real sky: sunlit gas was too dark and sky-lit gas too bright (the earlier parity fix was measured against a background the path tracer does not light with).
- Horizontal banding in tall, dense Flow clouds in raster: the sun and sky volumetric shadow maps now step once per gas cell.
- Dense Flow gas (smoke above 1) drew no denser in raster than smoke 1; Flow's ray march now uses Beer-Lambert opacity, clamped at 2 as the path tracer does.
- A dark, dotted rim around Flow gas in raster: the light pass now uses the opacity of Flow's own ray march (Flow patch 0011).
- Raster Flow gas lacked the sky's blue: sky light is no longer halved or greyed by a whole-sphere average, and the ground under the gas is lit by the sky above it.
- PhysXFlow3D light scattering at reduced resolution no longer steps along the smoke's edges over sloped floors, and smoke in front of the sky now blends over time and upsamples smoothly.
- The ground under bright gas (snow powder, steam, white smoke) is no longer shadowed as darkly as under black smoke in raster: the gas's volume shadow passes the light it scatters many times (two-stream total transmittance), as under the path tracer.
- Raster Flow gas's shadow on the ground no longer stair-steps into a hard dark outline at the gas's edge: the shadow map averages four columns per texel and has a texel per two gas cells.
- The top of a deep, bright Flow cloud no longer draws dark under the sky: its sky light's scattering orders count the gas below the point as well as above, and in bright gas reach deeper by diffusion theory (bluer inside under a blue sky).
- Flow gas in a strong weather wind no longer boils at higher `vorticity`: vorticity confinement takes the gas's speed relative to the wind it is carried on, not to the ground (needs Flow rebuilt with physx_patches 0012).
- Flow gas in strong wind no longer fills `max_blocks` within seconds: blocks the wind moves fast stay allocated only while they hold some smoke, so empty air the wind blows through is freed.
- Flow gas drifts on the weather wind again: the wind reserves the blocks along the plume's path, swept downwind from the sources for as long as the smoke lasts (gas lives only in allocated blocks, and Flow does not allocate ahead of it).
- Flow gas keeps time below 60 frames a second: it catches up with up to eight steps a frame, as Godot's physics does (it took one, so at 30 frames a second it ran at half speed).
- PhysXParticleFluid3D's isosurface casts shadows in the raster renderer (a shadow-only copy of the surface casts it).
- A smooth MPM pour no longer fills with white specks: trapped air is measured against each particle's own velocity field, so only liquid striking liquid makes foam and bubbles.
- `foam_size` set on an MPM fluid no longer rescales its foam spheres twice.
- MPM fluid takes as many substeps as `mpm_stiffness` needs (up to 128) instead of quietly softening the water, which made it sag like a gel in layers at fine particle sizes.
- Emitted fluid leaves at its rest spacing from a round emission sphere, so a high `emission_rate` pours a stream instead of a sheet or a burst.
- PhysXParticleFluid3D's raster shadow copy is left out of the path tracer, where it turned the water black.
- MPM fluid surface no longer shows bright cell outlines: closed air pockets inside the marched surface are smoothed out before meshing.
- MPM fluid streams fall at full speed again: velocity damping is per second instead of per substep, so many substeps no longer hold a falling stream under 1 m/s. Pours now thin and break up as they accelerate.
- PhysXGranular3D (and the sand grains a tyre throws) draws as particles again: the fluid's Isosurface default had turned the grains into a clear-water surface, so thrown sand was invisible.
- Wind obstacles on raised terrain counted their height from the weather's flat ground height, so they cast a lee far too long.
- A `PhysXDestructible3D` (dynamic, intact) moved by a script now goes there at rest instead of snapping back to its body; a `PhysXCloth3D` placed by a script before its first step is rebuilt there.
- Path tracing honours `Environment.ambient_light_source = AMBIENT_SOURCE_DISABLED`: diffuse bounces that leave the scene no longer gather the sky (and a procedural sky's sun glow), so a scene lit only by its sun matches the rasterizer (a white Lambert plane under a 30 degree sun: 0.498 traced, 0.497 raster, 0.500 analytic; before, the sky added 7 to 17%).
- Path tracing: a surface whose custom shader is still compiling (`rendering/pathtracing/async_shader_compilation`) is traced with the default material until its shader is ready, if it is opaque and never discards; it was left out, so a character drawn by `PhysXSkinnedCloth3D` vanished for the first minute on a cold shader cache. Stills still wait for the real shader.
- Path traced fire and other glowing gas no longer breaks up into scattered bright specks at a few samples a pixel: the glow is gathered along the whole ray.
- Path tracing: a bounce ray that leaves for the sky no longer adds the weather's traced aerosol haze on top of the sky. The radiance map already holds that aerosol along the whole ray, and with the ambient light off the sky lights nothing, as in raster. A sun-only scene under a hazy WeatherMap3D was 17% too bright at a 30 degree sun and 35% at 12 degrees; it now matches the analytic value (0.502 vs 0.500).
- The path tracer skips caustic photons and the camera medium probe while no material can bound a medium, and dry ground, beside water or without it, no longer loses the radiance cache to the caustics map (`rendering/pathtracing/caustics`).
- Path tracing: a mesh whose vertex() samples a texture now follows that texture when it is rewritten in place (`ImageTexture.update`, `texture_2d_update`, a 3D texture update, `texture_replace`), material or global; it kept the shape from before the change until something else moved it.
- The weather contract now answers rainfall, snowfall, canopy water and ground water input from the weather map; snow and rain on bodies read none before.
- Windows and macOS downloads build again after the caves, rocks, avalanche, ground, burnable and waterside changes (ambiguous math overloads and narrowing errors on MSVC and Apple clang).
- Under a thick smoke plume the sky keeps a wide ruddy glow round the sun (the light the smoke scatters forward lights its own forward lobe), and `SkyClock`'s adapting exposure counts the smoke, so the land stays visible in brown light instead of near black.
- Fires' smoke now dims and reddens the sky's sun disc and the sunlight its air scatters (the aureole round the sun, the sky under the smoke), by colour as it already did the sun light. Before, the disc stayed white under a thick plume. New `WeatherSkyMaterial.set_sun_smoke`; `set_aerosol` takes the smoke's share.
- Forest edges no longer repeat one tree: edge trees get more variants and turn more freely. A deer wood's edge is browsed, with its mantle eaten down to hedged thorns. Forest-grown sycamore and horse chestnut now grow leafy crowns instead of dense balls of bare twigs.
- Forest edge trees keep green limbs low on their open side.
- Trees and shrubs: no more "index_count >= index_array_len" errors from LOD levels no smaller than their base.
- Gorse and broom flowers read as flat ochre cards in raster: petals now fold along the midline, each flower turns its own way, golden yellow with a shaded throat, glowing against the light.
- `TreeGenerator.make_woodland` and `make_stand` with one variant built four slope variants of each stream-bank tree (about 47M extra triangles in a European woodland); they now build one.
- Tree crown parts (cones, berries, keys, catkins, flowers) were lit from behind in raster.
- Smooth bark (beech, hornbeam) drew about 1.5 times too bright.
- PhysX vehicles (car, motorcycle, tank) hold still at `Engine.time_scale` 0 instead of printing "timestep must be positive" every frame.
- Path tracing: bands along the depth contours under flat water, where the outer caustics cascades took their photons from every other pixel row and column.
- Path tracing: water surfaces (sea, lakes, pools, rivers) cast their shadows through the water, so sunlight reaching the bed is tinted by the depth it crossed on the way down as well as on the way back up; shallow beds no longer read pale and warm against raster.
- `add_sediment` docs: it is a rate held for 2 seconds after each call; a one-off mass goes to `add_sediment_mass`.
- SkyClock: with its clock held (a still), the sun now dims and reddens as fires' smoke is added or drifts over; it only followed the smoke while time moved. `get_smoke_transmittance()` is 1 with `smoke_haze` off.
- Raster water: the light reaching the bed is cut by the surface's Fresnel transmission on the way in (direct sun at its angle, sky light by its hemispherical average), as path tracing does; clear shallow water no longer reads brighter and bluer in raster than in path tracing.
- Silty and ashy water no longer path traces as glitter: the load enters the medium by the similarity relation (same reduced scattering, broader phase), so it converges. Silt now reads khaki-tan, the same hue in raster (multiple scattering calibrated against the path tracer) and path tracing. White water and falls in loaded rivers take the water's colour (beige in silt, grey in ash).
- Silty water reads khaki to brown instead of pink-orange: the silt's absorption follows measured mineral particles (Babin et al. 2003), and white water on it takes a lighter beige.
- Silty falls and white water: a fall's sheet on a loaded river scatters like the river (a tan curtain in raster and path tracing; path tracing drew dark brown glass), and white water on heavy loads is cream to beige.
- Torn spray sheets on storm crests take the sun's shadow in raster (they ignored it), so a sheet under a cloud or in a cliff's shadow is dark. River and water-surface benches gain `--shadow-bar=Y`, a board whose shadow falls across the water mid-frame.
- Surf foam no longer shows the shore wave grid as rows of short straight bars: PhysXWaterSurface3D reads its shore wave fields smoothly (cubic) and tears the roller's white water with wandering streaks; foam noise uses turned lattices, its holes cluster and its grain is patchy.
- Path-traced open sea no longer turns black toward the horizon: wave detail is filtered by the pixel's footprint under the path tracer too.
- `PhysXWaterSurface3D`: the sea no longer lifts its surface onto land more than a few metres above the still water (swash runs no higher). Land along the edge of its grid carried on outward through the clamped depth texture and stood in the sky over the horizon as thin dark posts.
- Raster silty water matches path tracing (within about 2% at light and heavy loads); it read warm tan where path tracing showed grey-brown.
- Wet prints and splashes ahead of a high or distant camera now show: the contact wetness square centres on the ground the camera looks at, not on the camera.
- Windows export with "Modify Resources" (and PCK embedding) no longer produces an executable Windows refuses to start.
- A cloud base authored below high ground stays there, so ridges in the cloud still get fog drip.
- WeatherMap3D.advance() now sees canopies and walls added since the last frame.

### Improved
- Trees: beech and other smooth bark read mottled silver-grey up close, with smaller pale lichen crusts; smooth bark on dead wood stays grey; lying logs carry fewer crusts and their moss film lies in drifts; bare wood grain meanders and knots are larger.
- Trees: lying logs show deep drying checks, grain that bends round knots and patchy staining on bare wood; moss shoots fade into the carpet edge; bark stories (`bark_wounds`) are on by default.
- Trees: moss on logs and fallen limbs carries modelled shoots over its cushions; weathered bare wood shows ridges along the grain and a ragged torn bark edge; beech bark has softer folds and lenticels at 40 cm.

## build-f83759ed (2026-11-22)

### Added
- Ash blown off burnt ground settles downwind and darkens snow (WorldSnow impurity, as black carbon equivalent).
- `PhysXDestructible3D.initial_linear_velocity` and `initial_angular_velocity`: a dynamic destructible can start moving, so swapping a falling object for one keeps its motion.
- `PhysXBlastAsset.prepare_render()`: converts the chunks' render meshes ahead of time, from any thread.
- `PhysXBlastAuthoring.build_prefractured` takes pieces with several materials (a Mesh or an Array of surfaces; `PhysXBlastAsset.materials` by slot) and an optional `health` per bond (`PhysXBlastAsset.bond_healths`, a multiple of the destructible's health), so a tree's limbs draw bark, wood and leaves and a twig joint breaks before a trunk joint. `PhysXDestructible3D.get_piece_chunks()` lists the chunks a piece carries.
- Burning buildings drop their window glass as shards and their ceilings and roofs sag and fall in room by room. `PhysXBuildingFire3D.glass_panes`.
- PhysXBuildingFire3D records its first catch: `get_ignition_time()`, `get_ignition_cause()` ("direct", "firebrand" or "heat") and the `ignited` signal.
- `docs/bench/village_fire`: a wind-driven grass fire runs into a street of timber houses; prints which houses catch, when and by what; dry beds against the walls, lawns, buffer, gap and wind are options.
- Burnt-out rooms smoulder: their debris glows for hours, smokes, sheds embers in wind and can rekindle. `PhysXBuildingFire3D.get_room_ember()`, signal `rekindled`.
- Smoke runs ahead of building fires: rooms fill with a smoke layer from the ceiling that spills through doors into the next rooms and pours from windows and eaves minutes before flames; visibility inside is reported. `PhysXBuildingFire3D.get_visibility_at()`.
- Water puts burning buildings out: a hose or fluid water in a room knocks its fire down below flashover and puts it out with steam pouring from the windows, and a weak stream only holds it back. `PhysXBuildingFire3D.apply_water()`.
- Window flames of burning buildings run up the wall above as sheets instead of balls.
- Wind drives building fires: it blows in through windward openings, flames leave by the leeward ones and hot gas is pushed through doors.
- Burning wooden buildings: rooms grow to flashover, break their windows, throw flames out of them, burn through walls, floors and roofs into the next rooms and collapse; they catch from ground fires, radiation and firebrands, and throw firebrands and smoke. `PhysXBuildingFire3D`.
- WorldBurnScar3D: author an old burn (a rectangle or polygon and its age in years); the fire contract reports how long ago the ground burnt (`WorldFire.burn_age_at`).
- `PhysXBurnable3D`: wooden objects (crates, logs, debris) catch fire from a `PhysXFireSpread3D` once the heat they take in reaches the ignition dose, burn with a Flow flame, char from the ignition point with glowing alligator cracks, smoulder when rain puts them out, and throw embers that can start new fires where they land.
- `PhysXCloth3D` `burnable` (on): cloth catches fire from ground fire, hot Flow gas and burning objects, chars with a glowing edge and burns into holes until it falls apart. `flame_spread`, `burn_through_time`, `ignite()`.
- River current drags clothing: worn cloth under water loses most of its weight and is dragged by the current (any water with a flow: rivers, waves), pressing against the legs on the upstream side.
- River current drags flags and sheets: `PhysXCloth3D` under water keeps about 30% of its weight and streams with the current from the world water contract (CPU solver).
- Wading wets clothing: worn and free cloth that dips into any water (sea, lakes, rivers, pools) soaks up to a ragged tide line just above the surface, then dries; the wet band adds its weight and cling.
- `PhysXCloth3D` `weather_wetting` (on): rain soaks cloth by how it faces the rain, and it dries in the air; wet cloth is heavier, limper and darker, clings to what it touches and needs its water boiled off before it burns. `wetness`, `water_capacity`.
- `PhysXSkinnedCloth3D` `weather_wetting` (on): worn clothing soaks in rain and dries the same way; wet garments draw darker, swing less and cling to the body. `wetness`, `fabric_density`, `water_capacity`.
- The ground contract reports how much of the ground pebbles and gravel cover, from the stone fields (`WorldGround.coarse_cover_at`, `RockField3D.get_coarse_cover_at`).
- Post-fire debris flows carry boulders at their front and throw mud, in a train of surges that stop on their fan. `PhysXDebrisSurge3D`.
- Smoke haze, burnt forest and settling dust darken snow and make it melt sooner; fresh snow buries the dirt and the thaw brings it back (`SnowCover3D.impurities_enabled`).
- Waterfalls count as water for everything that touches them: bodies and clothes in a fall get wet and are pushed down by it.
- Wind lifts ash after a fire: a strong wind over the cold burnt ground of a `PhysXFireSpread3D` lifts its ash as a low grey dust that drifts downwind in gusts until the ash is gone; wet ground holds it. `blowing_ash` (on by default), `get_ash_blown()`.
- Rain washes the burn scar: a fire leaves burnt soil water-repellent for a year or two, so even light rain runs off it, and the runoff carries the ash away. `WorldFire.water_repellency_at()`, `PhysXFireSpread3D.get_water_repellency_at()`, `PhysXFireSpread3D.get_ash_washed()`; GroundSurface3D reads it (burnt litter gone, infiltration cut).
- `PhysXFireSpread3D` burns the logs and fallen branches of its `vegetation_field` as large wood: they catch from the fire under them, flame for about three minutes per centimetre of diameter, then smoulder; bark chars and glows in its cracks. `get_heavy_fuel_at()` reads what is left.
- `PhysXFireSpread3D` burns the plants over it: grass burns down to charred stubble with glowing tips in the burning band, small plants char under the flame and scorch brown above it, tree bark chars to the flame length and leaves brown up to the crown scorch height (Van Wagner 1973), in raster and path tracing, through a new `fire_state_map` global.
- `PhysXFireSpread3D.fuel_patchiness` (on) makes fuel and moisture patchy, so a burn scar has ragged edges and unburnt islands; `fuel_map` takes moisture in its green channel, and `vegetation_field` makes worn paths bare and leaf litter burn.
- Burnt trees fall later: a fire chars each stem's foot (`PhysXFireSpread3D.get_stem_char_at()`, `WorldFire.stem_char_at()`), and in a later storm `TreeWind3D` lets a charred tree snap at its foot and fall without its root plate. `get_storm_snap_count()`.
- Fire clouds: a fire's plume that rises past the condensation level caps itself with a pyrocumulus, held over the bent-over plume while it burns and drifting off after; the largest fires tower into a pyrocumulonimbus with lightning and a gusty outflow. `WorldFire.plumes()`, `WeatherMap3D.fire_clouds`, `WeatherMap3D.get_fire_cloud_for()`, `PhysXFireSpread3D.get_plume_heat()`.
- `fire_age_map` shader global: game years since each patch burnt, for plants that change in the years after a fire.
- Burning buildings and props break apart: a `PhysXBurnable3D` on a `PhysXDestructible3D` loses strength as its timber chars, so a burning wall or shed breaks into pieces that fall and keep their char. `collapse_share`; `PhysXDestructible3D.set_material_remap()`, `set_instance_shader_parameter()`, `get_render_materials()`.
- Burnt crowns lose their needles and leaves: a torched tree stands as bare black branches over a patch of grey ash, and burning twigs drop out of the crown while it burns.
- `PhysXFireSpread3D` `crown_fire` (on): trees and shrubs catch fire from the ground fire under them by Van Wagner's crown fire initiation, flare up, light crowns their flames reach and throw far more embers. Crowns come from `vegetation_field` or `add_tree()`; `foliar_moisture`.
- `VegetationField3D.get_crowns_in()`: the placed crowns over a rect, with their crown base heights.
- Burning pieces keep their flames: once a burning `PhysXDestructible3D` breaks, its `PhysXBurnable3D` moves the flames, sparks and firebrands onto the burning pieces, which smoulder where they land. `PhysXDestructible3D.get_piece_rest_center()`, `get_piece_volume()`.
- Firebrands that land on a `PhysXBurnable3D` pile up (on top faces, at the foot of walls) and heat it until it catches: a burning stand of trees upwind can set a wooden shed alight. `add_brand()`, `get_brand_pile_count()`.
- Burnt grass shows its ash: dry grass and litter burn out to white-grey ash, damp fuel leaves black char, in patches; wind thins the ash and rain darkens it, and grass stubble carries the same flecks. `PhysXFireSpread3D.grass_ash` (on by default).
- Ridge fire whirls follow the weather's terrain wind: lee-slope whirls form where the terrain wind has separated behind a crest, driven by the ridge's wind. `WorldWeather.terrain_lee_at()`.
- Fire whirls behind ridges: on a steep lee slope in a strong wind, `PhysXFireSpread3D` whirls form in the ridge's eddy and run across the slope, lighting the fuel they pass, so the fire spreads sideways along the ridge. `get_lee_whirls_spawned()`.
- Fire makes its own wind: a `PhysXFireSpread3D` draws air in toward its burning front, so grass and `PhysXCloth3D` nearby lean into it and fronts heading toward each other pull together. `fire_wind` (on by default), `get_fire_wind_at()`, `WorldFire.wind_at()`, shader globals `fire_wind_map` and `fire_wind_rect`.
- `PhysXBurnable3D` throws firebrands while it burns (`embers`): lofted by its own plume, carried by the wind and able to start spot fires on the dry ground fuel of a `PhysXFireSpread3D`. `get_firebrand_count()`, `get_spot_fire_count()`.
- Smoke haze: fires' smoke drifts and spreads on the wind as a smoke layer over the weather map; under it the air hazes, the day stays cooler, and the sun dims and turns orange, then red. `WeatherMap3D.smoke_haze`, `WeatherMap3D.get_smoke_optical_depth_at()`, `WorldFire.smoke_sources()`, `PhysXFireSpread3D.get_smoke_rate()`.
- Smouldering ground fire: a passing fire lights the duff under it where it is dry enough, and it burns on for hours of game time with no flame, creeping a few centimetres an hour, glowing dull red in spots under a pale, low-lying smoke. Heavy rain or water puts it out; light rain does not; under unburnt grass it can flame again when the wind picks up. `PhysXFireSpread3D.smouldering`, `duff_depth`, `duff_moisture`, `duff_mineral`.
- Fire-killed trees stand as snags: a fire kills a tree by how much of its crown it scorched against its bark, and over the game years the dead tree's limbs and roots rot, so storms strip it and it falls at its butt, leaving a stump. `WorldFire.crown_scorch_at()`, `TreeWind3D.get_snag_count()`.
- `PhysXFireSpread3D.get_burn_age_at()`: how long ago each patch burnt, in the weather's game years (`WorldFire.burn_age_at()`).
- `PhysXFireSpread3D` `spotting` (on): burning grass, litter and logs throw glowing firebrands that the plume lofts and the wind carries; where one lands on dry unburnt fuel it starts a spot fire ahead of the front. `spotting_scale`, `get_firebrand_count()`, `get_spot_fire_count()`.
- `PhysXFireSpread3D`: fire that spreads over grass and leaf litter at Rothermel's rate (faster downwind and uphill, stopped by wet fuel), chars the ground, glows along the front and drives NVIDIA Flow flames and smoke; hot gas from another Flow (a campfire) can set it alight.
- Water on fire makes steam: a white cloud billows up where water lands on burning or glowing ground or on a burning prop, and along a shore where a fire burns at the water's edge. `PhysXFireSpread3D.get_steam_mass()`.
- A tree that snaps at a fire-charred foot leaves its stump: bark cut 0.3 to 0.9 m up with a short charred break on top, solid to bump into, and the trunk folds over the stump's edge.
- Water puts fires out: `PhysXParticleFluid3D` water lying on a `PhysXFireSpread3D` grid soaks the grass and litter under it, putting out burning cells and stopping the front at wet ground; water on a `PhysXBurnable3D` damps and puts out its flames and drowns the embers on it. `PhysXFireSpread3D.get_doused_cells()`.
- `PhysXFireSpread3D.use_weather_moisture` (on): fuel moisture follows the active `WeatherMap3D`, drying or damping towards the air's equilibrium moisture over `moisture_timelag`, and rain wets the fuel at once, slows the front and puts burning ground out; `get_moisture_at()` reads it.
- Fire whirls: a strong flank of a `PhysXFireSpread3D` running along the wind can spin up a whirl of flame some three flame lengths tall that wanders downwind and lofts firebrands higher, most often when the wind is near the fire's own updraft speed. `fire_whirls` (on by default), `get_whirl_count()`, `get_whirls_spawned()`.
- Rivers over their banks, lakes and any other water soak the ground they cover, and still floodwater leaves its silt and ash behind as fresh mud (`GroundSurface3D.flood_silt`).
- Snow contract: `WorldSnow.avalanche_pressure_at` and `avalanche_peak_pressure_at`.
- PhysXFlow3D `diffuse_transmission` (on by default): in raster, bright gas lets the scene behind it through, blurred, by multiple scattering (two-stream transmittance beyond the direct beam), as under the path tracer.
- `PhysXFlow3D.fire_light`: a burning Flow emitter lights its surroundings with a soft-shadowed light that flickers at the flame's puffing frequency (raster and path traced; under the path tracer it stands in for the flame's glow as a light source, so the ground is lit without noise). `fire_light_energy` scales it.
- `PhysXFlow3D.embers`: glowing blackbody embers rise from a fire as streaks along their flight, `ember_rate` per second.
- PhysXFlow3D `ground_color` (white by default): the colour of the ground under the gas for the raster light scattering pass, scaled by `ground_albedo`. The light the ground sends up into a cloud's base takes it, as path-traced bounces take the ground's own colour.
- PhysXGroundDust3D: tyre dust near the camera simulated in Flow. Every sliding or spinning wheel feeds it; the dust trails, rolls in the car's wake, is pushed by the ground wind, flows round the car body, rests on the ruts and shades the ground by its density. Farther dust stays with GroundDust3D.
- GroundSurface3D `get_height`, so a PhysXFlow3D terrain floor can follow the deformed sand.
- `PhysXFlow3D.ground_albedo`: the sunlit ground under Flow gas lights its underside in raster, as path tracing's ground bounces do; snow powder clouds use 0.8.
- NVIDIA Flow gas now pushes what is in it: `PhysXFlow3D.push_bodies` (on) blows dynamic bodies such as debris and Blast pieces with drag from the gas velocity, `PhysXCloth3D.flow` lets a flag or tarp be blown by a Flow gas and stir it back, and `PhysXFlow3D.sample_velocity()` reads the gas velocity.
- PhysXFlow3D `scattering_albedo` (default 0.9): how much light the gas keeps at each scatter, in both the path tracer and the raster renderer, so very white media such as snow powder (about 0.99) stay bright inside instead of turning grey.
- The raster renderer adds the light of scattering orders past its three multiple-scattering octaves for gas whiter than the 0.9 default, so it brightens with `scattering_albedo` as the path tracer does.
- PhysXFlow3D `terrain_floor` (on by default): the ground under the gas is a solid, laid as overlapping tiles tilted to the slope, so smoke and dust lie on and run down terrain instead of sinking into it. Heights come from `terrain_path` or a downward ray onto static bodies.
- PhysXFlow3D `smoke_buoyancy` (Flow's buoyancy per smoke): below 0 the smoke is heavier than air, for dust and cold fog.
- MPM fluid makes spray, foam and air bubbles (Ihmsen et al. 2012 diffuse particles; bubble drag and foam surface hold from Wretborn et al. 2022). On by default through `foam_enabled`.
- Cloud sitting on wooded ground drips from the trees: leaves catch fog droplets into the canopy, which drips onto the ground and wets it (`WeatherMap3D.fog_drip`, `WorldWeather.fog_water_at`, `fog_drip_at`).
- Hot air above fires bends light: the background shimmers through the plume, from the simulated temperature, in raster and path tracing (`PhysXFlow3D.heat_shimmer`).
- Heavy bodies and vehicles break through lake ice too thin for their weight (white and thawing ice give first); the hole freezes over again.
- Feet, wheels and other moving bodies kick up the fallen leaves they pass through.
- Fallen leaves float and drift on ponds and rivers, lie on snow, get buried by fresh snow and reappear when it melts (`LeafFall3D.leeway`).
- Snow drifts into the lee of rocks, walls, buildings and vehicles and blows off their tops (`SnowCover3D.drift_obstacles`); blown sand drops behind them (`GroundSurface3D.obstacle_lee`).
- Boulders and vehicles block the wind (`RockField3D.wind_obstacles`, `PhysXVehicle3D.wind_obstacle`); any node in the `weather_shelter` group can list its own boxes with `get_wind_obstacles(rect)`.
- Weather contract: `WorldWeather.obstacle_height_at` and `obstacle_lee_at`.
- Feet and tyres leaving mud print it on the firm ground beyond, thinner with each step, and water washes it off (`WorldGround.track_mud`, `WeatherMap3D.get_body_mud`).
- Shader built-in `RAY_FOOTPRINT` (fragment): the width of the path tracer's ray cone where it meets the surface, in world units; 0 in raster. Rivers use it to filter ripples finer than a pixel into roughness, as raster does.
- Rain and snow wash smoke haze and dust out of the air (`WeatherMap3D.smoke_washout`, `PhysXFlow3D.rain_washout`).
- `RainFall3D`: falling rain round the camera from the weather map, streaks slanting in the wind, splashes, rain haze, and drips from leaves and eaves that keep falling after the rain. `max_drops`, `splashes_enabled`, `drips_enabled`, `haze_enabled`, `shelter_enabled`.
- Raindrop rings on the sea, lakes, pools, rivers, live water and the ground's puddles, raster and path traced; heavy rain dulls reflections (water shader uniform `rain_rings`).
- `WeatherMap3D` rain on leaves: the canopy holds rain and drips after it stops (`canopy_water_enabled`, `canopy_storage`); rain and snow split by the air's temperature at each height (`get_rain_share_at`, `get_rainfall_at`, `get_snowfall_at`).
- `BaseMaterial3D` `wetness_canopy`: leaves stay wet while the canopy holds water, with beads on their upper faces; on for trees and falling leaves, and ground plants.
- `WeatherRunoff3D`: rain runs off the land over kilometres; ground with runoff over it stays wet.
- `PhysXRiver3D` `rain_response` (on): rivers rise and run faster with the runoff; lakes with `level_from_flows` rise with it.
- Two-layer refraction in Forward+ (`rendering/transparency/refraction_second_layer`, on): water and glass that read the screen texture (Transmission, refraction) now show the refracting surface behind them, such as the pool under a falling stream or a second pane of glass, instead of only the opaque scene. Depth peeling, one extra layer; only frames with such a surface in view pay.
- Shrub and tree leaves near the sea brown one by one with the salt the spray leaves on them, as grass does (`weather_spray_map`).
- Sea salt melts shore snow and lake ice below freezing, as much as its brine can hold, and meltwater carries it away (`SnowCover3D.salt_melt`).
- `PhysXSeaCaveWater3D`: the sea inside sea caves, from `CaveNetwork3D`'s surge (level, width and current along the passage) with the river's surface, from the arch in. Short waves come in at the mouth and reflect off the walls and the back of the cave (Water Surface Wavelets, Jeschke et al. 2018), as real geometry; steep faces break white. `CaveNetwork3D.sea_water` (on) makes one.
- `PhysXWaterSurface3D` water clipping: nodes in the `water_clippers` group give capsules where the sea cuts itself out and cross-fades to another surface, in raster and the path tracer. `CaveNetwork3D.get_water_clip_capsules()` cuts the sea along sea-cave passages; `get_sea_cave_water()` gains `along`, `covered_from` and `wave_rms`.
- The sea surges into sea caves: waves run up the passage, growing as it narrows, with currents in and out, and storm swells drive air up blowholes (`CaveNetwork3D.sea_surge`, `get_sea_cave_water`, `get_blowhole_spout`, signal `blowhole_spout`).
- Sea spray from breaking waves wets the shore downwind and leaves salt that rain washes off; cliff faces catch the spray, and salted grass browns at the tips (`WeatherMap3D.sea_spray`, `WorldWeather.sea_spray_at`, `salt_at`, `WorldWater.breaking_at`).
- Lying snow lays grass, crops and plants down under its weight and buries short grass; soft stems give first.
- The weather contract also answers day of year, season, cloud cover, water and surface temperature, ground height and time scale; ground, snow, vegetation and water now read the weather only through it.
- Terrain generator: coastal dunes. The beaches feed wind-blown sand inland on a fine grid; marram builds foredunes along the plant line, and blowouts and parabolic dunes open where the cover fails (`dunes_enabled`; `get_dune_height()`, `get_dune_stability()`).
- Terrain material: scree shows a gravel and fines matrix between its stones up close (`rock_detail_enabled`).
- Sea cliffs grow a patchy white salt crust where the spray's salt dries on rock and scree, gone while the stone is wet (TerrainMaterial3D `salt_bloom`).
- Terrain generator: desert sand seas. Transverse ridges, barchans, linear and star dunes grow over `sand_sea_rect` from the wind regime and the sand cover (`sand_seas_enabled`, `sand_sea_kind`, `sand_sea_depth`, `sand_sea_wind_angle`, `sand_sea_scale`; `get_sand_sea_height()`).
- Avalanches break trees: `TreeWind3D.avalanche_enabled` reads `WorldSnow.avalanche_peak_pressure_at`; crowns snap from about 3 kPa, stems by diameter (10-20 kPa at 30-50 cm), falling downslope. Felled trees cut their leaf area out of the weather map's canopy.
- Bark beetle kill: `TreeGenerator.beetle_years` grows a conifer from green attack (resin beads, boring dust) through red needles to a grey snag shedding its bark, then a broken top; mesh meta `tree_beetle_years`. Stand `STAND_BEETLE_SPRUCE` with grey snag spots, a red front at their edges and living spruce between, sized by `beetle_outbreak`.
- `TreeGenerator.get_living_meshes()`: a species' meshes without its fire band variants.
- Trees: beech and birch boles carry dark branch-scar chevrons below their shed limbs, a callus knob over the scar and two arms fading down round the stem, longer and flatter the older the scar (`bark_chevrons`).
- Cork oak: `TreeGenerator.PRESET_CORK_OAK`, an evergreen oak whose short bole divides into a few thick spreading limbs under a broad crown, with small toothed leaves grey below and deep, spongy grey-buff cork bark.
- Leader split for decurrent crowns: the stone pine and cork oak leaders stop at a set height and a few limbs take over, clear of side limbs until near the crown.
- Cork harvest: `TreeGenerator.cork_stripped_years` strips the cork oak's bole and limb bases, bright orange when fresh, red-brown within weeks, near black within the year, then new cork growing back, with a step up to the virgin cork above. By default each tree picks a stage of the nine-year cycle.
- Date palm: `TreeGenerator` preset `PRESET_DATE_PALM` with a trunk of cut leaf bases in their diamond lattice, arching feather fronds with V-set pinnae and basal spines, a dead-frond skirt and offshoots on wild palms, flowers and hanging date bunches by season; stand `STAND_DATE_OASIS` (tended rows, wild clumps, olives beneath).
- Fire band variants: placed tree species carry one variant per fire band with mesh meta `tree_fire_years` and `tree_fire_band`, which `VegetationField3D` picks by burn age. `TreeGenerator.fire_variants`.
- Desert shrubs: `TreeGenerator.PRESET_CALLIGONUM`, `PRESET_WHITE_BROOM` and `PRESET_SALTBUSH`, taken by `make_shrub_species` in every shrub form, for sand seas and their interdune flats.
- Calligonum grows leafless jointed green twiglets and red bristly fruit in spring that dry to buff; white broom hangs silvery switches and flowers white from January to March; saltbush is a dense, matte silver-grey bush.
- Shrubs carry an extra first level of detail a few metres out, so the full meshes cost a third to a half less in the middle distance.
- Calligonum flowers white from February to April; saltbush carries pale fruiting panicles above its leaves from late summer into winter.
- Dwarf fan palm: `TreeGenerator.PRESET_DWARF_FAN_PALM`, a clump of short shaggy, fibrous trunks or stemless crowns with plicate fan leaves on spiny green petioles and a skirt of dead leaves; spring flowers among the leaf bases and dates on female plants. In the five `make_shrub_species` forms.
- Trees a fire killed outright redden over the first months and drop their whole crown over a year or two; the same mortality rule (`VegetationFire::tree_killed`) drives the leaf shader and `TreeWind3D`'s standing snags.
- Trees regrow after fire: `TreeGenerator.fire_years` gives a charred snag with its species' response, epicormic shoots (cork oak), a basal clump (holm oak and most broadleaves) or seedlings on the ash bed (maritime pine, rockrose). `get_fire_response()`.
- Trees: wind-flagged trees with limbs only on the lee side and krummholz mats at the tree line (`wind_exposure`, `krummholz`; flagged and krummholz forms in the subalpine stand).
- Trees: trees on banks with roots running down the slope and exposed by erosion (`ground_slope`, `bank_erosion`).
- Gorse and broom: `TreeGenerator.PRESET_GORSE` and `PRESET_BROOM`, with gorse spines and pea flowers and pods as geometry, green twigs, and flowering seasons (`TreeGenerator.get_bloom`, `get_leaf_season` "flowering"). `TreeGenerator.make_shrub_species` returns a placeable species in young, mature, old, thicket or wind-pruned cushion form.
- Hedge shrubs: `TreeGenerator.PRESET_BLACKTHORN`, `PRESET_FIELD_MAPLE` and `PRESET_DOG_ROSE`, and `make_shrub_species` takes them with hawthorn and hazel.
- Cut hedges: `TreeGenerator.SHRUB_HEDGE` grows a two-metre segment of flailed field hedge, A-shaped or boxed, leafy to the ground, for laying end to end along field edges.
- Hedge seasons: hawthorn white in May with haws in autumn, blackthorn white on bare twigs in March and April with sloes, dog rose pink in June with scarlet hips.
- Holm oak: `TreeGenerator.PRESET_HOLM_OAK`, an evergreen oak with a short bole under a dense, dark, rounded dome, small leaves grey below and dark grey cracked bark.
- Ice storms load trees: a new weather contract reports glaze (clear ice from freezing rain), and wind-rigged trees carry its weight, bow and break under it, and stand up again after the thaw (`TreeWind3D.glaze_load_enabled`).
- Freezing rain: a warm layer aloft (`WeatherMap3D.warm_nose_temperature`) turns rain over sub-zero ground into glaze, accreted by Jones' ice model and melted in a thaw (`get_glaze_at`).
- Clear ice on surfaces glazed by freezing rain, raster and path traced (`BaseMaterial3D.glaze`, on for trees).
- Glazed trees as geometry: a clear ice coat round twigs and limbs and icicles under them, seen alike by raster and the path tracer (`TreeGenerator.glaze_mm`).
- Near trees in a vegetation field grow the weather's glaze as ice geometry, in bands, and swap as it builds or melts.
- After an ice storm: broken tops, stripped crowns and birch bent over in arches, by species, and a northern hardwood stand after one (`TreeGenerator.ice_storm_years`, `STAND_ICE_STORM`).
- Trees: deadwood meshes expose `tree_deadwood_features` (peel edges and drying checks, 8 floats each) for placing growth along them.
- Wild olive: `TreeGenerator.PRESET_WILD_OLIVE`, a gnarled small tree of crooked stems under a dense, pale silver grey-green crown, narrow leaves silvery below.
- Maritime pine: `TreeGenerator.PRESET_MARITIME_PINE`, a tall pine with a long clear bole, a dense irregular rounded crown, long stout needles and thick purplish plated bark.
- Carob: `TreeGenerator.PRESET_CAROB`, an evergreen with a short trunk under a broad dense dome of glossy paired leaflets.
- Needle cast after fire: needles and leaves a fire killed brown fall over the following year, most in the first months, read from the new `fire_age_map`.
- Trees: roots draped over a generated boulder beside the stem (`root_stone`).
- Trees: bank forms with slope variants for stream banks and gorge sides, in the riparian and beech stands.
- Mediterranean scrub shrubs: `TreeGenerator.PRESET_GUM_ROCKROSE` (white blotched flowers, capsules, summer leaf fold), `PRESET_KERMES_OAK` (spiny-toothed leaves, acorns in spiny cups) and `PRESET_LENTISK` (paripinnate leaves, red to black drupes on female plants), in the five `make_shrub_species` forms.
- Stone pine parkland: `TreeGenerator.STAND_STONE_PINE`, open-grown umbrella pines about 11 m apart with saplings, snags and logs.
- Stone pine: `TreeGenerator.PRESET_STONE_PINE`, the umbrella pine. A dome when young, then a flat crown as wide as the tree is tall on a clear bole, with long needles in tufts, plated red-brown bark and cones of three ages on top of the crown.
- Storms break and throw rigged trees: `TreeWind3D` checks each limb's wind moment against its wood (species MOR, knots) and each tree's against its root anchorage; broken limbs and thrown trees (with their root plate) fall as hinged PhysX bodies under the wind, and on landing become Blast destructibles that snap where they hit and settle under their own weight, a thrown tree lying propped on its plate standing on edge. `TreeWind3D.storm_*`, `TreeGenerator.storm_kit`, signal `storm_damage` with the root pit.
- Vegetation: alpine and scree plants above the treeline: moss campion cushions, purple saxifrage, mountain avens, curved sedge, dwarf willow and alpine toadflax, in fellfield, grassland, snowbed and scree sets above the map's own treeline (growing-season temperature, `PlantSpecies.RULE_SEASON_TEMPERATURE`) and sorted by curvature, sun and talus (`PlantGenerator.make_alpine`).
- Vegetation: bog and moor plants as real geometry: sphagnum hummocks, cotton grass with soft cotton heads, bog asphodel and sundew, sorted into hummocks and hollows on wet, level ground (`PlantGenerator.make_bog()` for blanket bog and valley mire).
- Vegetation: cereal crops on `GrassField3D` (`crop`: wheat, barley, oats, rye). Each tiller is real geometry (stem, four leaves, the ear with awns, or the open oat panicle) in drill rows (`row_spacing`, `drill_angle`), one to three tillers to a plant, swaying in the same wind as the grass. `crop_stage` (BBCH) sets the look: leafy rows, stem elongation, heading, ears and straw ripening from green to gold with the leaves dying from the bottom, nodding barley, and stubble after harvest.
- Vegetation: crop fields (`GrassField3D.fields`): polygons each with its own drill angle, fixed stage or sowing calendar (`sown_day`, `harvest_day` with a season source; `crop_calendar_stage`), tramlines (paired unsown wheel tracks) and a headland drilled round the edge. The ground under them shows the drilled soil and darker wheelings, and past the tillers the crop's colour and cover at its stage (`crop_canopy_look`), so fields read as a patchwork to the horizon.
- Vegetation: dune plants follow the terrain generator's coastal dunes, embryo and mobile dune sets on moving sand and fixed dune sets where marram holds it (`PlantSpecies.dune_range`, `RULE_DUNE`).
- Vegetation: dune plants as real geometry: marram, lyme grass and sea holly in embryo, mobile and fixed dune sets by distance to the water (`PlantGenerator.make_dunes()`). Placed plants now shelter blowing sand and trap it when their field is in a ground surface's `wind_shelters` (`VegetationField3D.sample_lateral_cover()`, `PlantSpecies.wind_frontal_area`).
- Vegetation: plants past their range can be drawn as real volume instead of ground colour, a porous voxel shell of their leaf cover out to 150 m (`VegetationField3D.far_volume`, on by default; `far_volume_range`, `far_volume_cell`).
- Vegetation: Atlantic heath: bell heather, cross-leaved heath and crowberry as real-needle dwarf shrubs, in dry, wet and maritime heath sets placed by rainfall, winter cold, wetness, shade and coast exposure (`PlantGenerator.make_heath`).
- Vegetation: ground mist as a real gas in the near field, pooling in hollows, parting round walkers and burning off as the sun climbs (`VegetationMist3D`, needs Flow).
- Vegetation: pollen puffs when bodies brush grasses and other wind-pollinated plants in flower (`VegetationPollen3D`, needs Flow; `PlantGenerator.get_pollen_release`).
- Vegetation: salt marsh and shingle plants: glasswort, cord-grass, sea purslane, sea aster, sea lavender, sea kale, sea campion and yellow horned-poppy, zoned up the tidal frame in pioneer, mid and high marsh sets on sheltered, level coasts and a shingle set on the backshore (`PlantGenerator.make_saltmarsh`; `PlantSpecies.RULE_TIDE_HEIGHT`, `RULE_COAST_EXPOSURE`, `RULE_BEACH` from the terrain generator's sea).
- Vegetation: Mediterranean scrub: rosemary, French lavender, thyme and sage-leaved rockrose as real-leaf dwarf shrubs, in garrigue and maquis sets placed by winter cold, rainfall, aridity and soil depth, with the rockrose's summer drought; `PlantGenerator.apply_scrub_rules` places other scrub shrubs in the same sets (`PlantGenerator.make_scrub`).
- Vegetation: waterside and wet-stone plants as real geometry: marsh marigold, meadowsweet and butterbur placed by distance to water (`PlantGenerator.make_banks()` for streams, lakes and waterfall spray), aquatic moss trailing downstream on river stones and liverworts on wet tops (`PlantSpecies.stone_top_water`, `follow_flow`).
- Vegetation: heath and scrub sets now include the woody shrubs: gorse and broom on dry heath, wind-clipped gorse cushions on cliff-top heath, lentisk, kermes oak, gum rockrose and dwarf fan palm in the maquis, kermes oak cushions, young fan palms and young gum rockrose in the garrigue, each in clumps under the set's rules (`PlantGenerator.make_heath`, `make_scrub`, `shrubs`).
- Vegetation: worn paths: give `VegetationField3D` and `GrassField3D` a list of `paths` (centre line, width, walkers a year) and plants thin out and grow shorter by how well their growth form resists trampling, leaving bare trodden soil in the middle and tough rosettes and grasses at the edges (`PlantSpecies.trample_resistance`, `GrassField3D.trample_resistance`).
- Characters print the ground, snow, sand and mud with their feet, step by step, not with their whole capsule; grass, plants and water still feel the body (`WorldContracts.describe_body` walker, meta `world_contact_feet`).
- A river's current lays the grass and plants under it downstream, flat in a fast stream, from the water contract's surface and flow.
- Fire melts the snow under it, whatever the air temperature (`WorldFire`).
- Plants, hair, falling leaves, ash and embers, smoke and rock dust lean into or drift toward a nearby fire's indraft.
- Tyres lose grip on lying snow (more on powder and slush, near ice on a hard crust) and on bare lake ice; chains and lugs win it back.
- `PhysXWaterSurface3D.foam_shade` (0.8): whitecaps and shore foam shade the bed under them in raster through the caustics map (bubbles send most of the sun back up), as the path tracer already did from the foam itself.
- `WorldWater.wave(position, direction, height, period, width)`: an impulse wave in metres and seconds from a landslide, avalanche, calving or rockfall entering the water. `PhysXWaterSurface3D.submit_wave` seeds it in the ripple solver as one crest and trough with its long-wave velocity, so it runs one way at sqrt(g h), runs up the shores and reflects; water without a solver takes a splash of the crest's height.
- `OasisSprings3D`: a still pool at each desert spring (`springs`, x, level, z and discharge in L/s, or `spring_source` with `get_springs()`), sized by its water balance (area = discharge / (evaporation + seepage), evaporation from the climate by Hargreaves or set) with a noise outline unique to each spring, and sand boils at its vents whose floors heave with the flow while the surface above lifts and sheds rings.
- Rivers carry a load entering by one bank as a plume along that bank, spreading across by turbulent mixing over tens of channel widths, instead of mixing it at once through the whole channel.
- Rivers drop silt and ash where the water slackens (by the banks, in pools); `PhysXRiver3D.get_sediment_deposit_at()` and `WorldWater.sediment_deposit_at()` give what has settled, in kg/m².
- `WorldWater.add_sediment_mass(position, mineral_kg, ash_kg)`: a load dropped into water at once (a debris flow's or landslide's fines), mixed through the river section or lake it lands in and capped at hyperconcentrated flow (1000 kg/m³). Debris-flow dumps through `add_sediment` were held as a rate for two seconds, doubling their mass, and a second dump into the same river section on a later frame replaced the first.
- Rivers and lakes carry suspended sediment from `WorldWater.add_sediment` (silt and clay, ash and char): rivers move it downstream at their speed, lakes mix it through their volume and let it settle (silt in about half a day, ash in about two). Silt browns and ash greys the water alike in raster and path tracing; raster turbid water now adds multiple scattering so it reads opaque and tan like the path-traced water.
- Cold air pools: on clear calm nights valleys and hollows fill with colder air, so frost and fog form there first while slopes above stay milder (WeatherMap3D `cold_pools`).
- WeatherRunoff3D starts post-fire debris flows: burnt steep basins in intense rain (USGS M1 likelihood, Gartner volume) emit `debris_flow(event)` on the runoff and on `WorldGround`, with the outlet, volume, channel path, bankfull widths and burn severity. `debris_flows` (on by default), `debris_flow_threshold`.
- Rime: ground standing in freezing cloud grows white rime on the windward side of plants, rocks and cliffs, thicker the longer it lasts, and sheds it in a thaw (WeatherMap3D `rime`).
- WeatherRunoff3D answers the runoff contract (`WorldGround.add_runoff_load`): loose silt and ash put on the land move down the drainage with the water and enter rivers and lakes through `WorldWater.add_sediment`.
- Bodies coming out of water, wet mud or puddles leave wet footprints and tyre tracks that fade along the way, drip where they stand, and dry with the weather (`WeatherMap3D.wet_tracks`, `get_body_water`).
- Avalanche paths keep the forest young: where a dense avalanche breaks trunks the stand starts again, so tracks fill with pioneers (SnowAvalanche3D `stand_damage`).
- A beaver dam node: when it fails, its drained pond becomes a beaver meadow that plants and trees grow into (`WorldBeaverDam3D`).
- Ground standing at the low cloud's base fills with hill fog that wind and sun do not clear, and lightning starts at the local cloud base (`WorldWeather.cloud_base_at`).
- Smoke and dust take the colour of the ground under them in the light it bounces up: terrain layers, wet ground and snow (`WorldGround.albedo_at`, `WorldSnow.albedo_at`, PhysXFlow3D `ground_from_world`, on by default).
- The air follows the season and the time of day: coldest near dawn, warmest in the afternoon, humid at night (`air_follows_climate`, `air_daily_cycle`, `daily_temperature_range`), and cools with height.
- The sun dries wet ground faster than shade or night (`sun_drying`).
- Clear calm nights bring dew, and hoarfrost on terrain, grass, plants and rocks below freezing; the morning sun melts it (`dew_and_frost`).
- Radiation mist comes sooner and thicker over wet ground, and steam fog rises off lakes and the sea when the air is much colder than the water (`WeatherMap3D.get_water_temperature`).
- Fire dries the ground under it, and fuel lying on wet or dewy ground burns slower (`WorldFire`).
- Lightning strikes tall things first and can start fires in dry fuel (`lightning_ignition`).
- Tyres lose grip on wet, flooded and icy ground, and aquaplane at speed.
- Every moving body bends grass, plants and branches, presses ground and snow and ripples water (`physics/3d/world_contact/auto_join`).
- World contracts: weather, water and body contacts each behind one interface (`WorldContracts`), with script stand-ins for tests.
- Weather rain follows the terrain climate: more on windward slopes, a rain shadow behind ridges, and rain or snow by each spot's own height, so summits snow while valleys rain (WeatherMap3D `orographic_rain`).
- Tyres take their grip on snow from the snow itself where it keeps one: packed roads and snow polished to ice by traffic (`WorldSnow.grip_at`).
- Drained beaver ponds are a stand disturbance, so meadows can grow where a dam failed (`WorldStand.DISTURBANCE_BEAVER`).
- Deer browsing pressure over the stand, with how high the animals reach, so plants and young trees can show browsing (`WorldStand.browse_at`, `browse_reach_at`).
- A stand-age contract gives the years since the last stand-replacing fire, windthrow or beetle kill at any point, for forests that age (`WorldContracts.get_stand`, `WorldStand.stand_age_at`).
- Game trails are published once for every system to read along their tread, and boar rooting is a field-layer disturbance (`WorldStand.set_trails`, `trail_at`, `rooting_age_at`).
- Lightning that strikes a tree crown is remembered by the stand, so forests can show the struck tree's scar for years after (`WorldStand.strike_age_at`, `WeatherLightning3D.record_tree_strikes`).

### Changed
- Destructible pieces are drawn as instances of per-chunk meshes built once per asset and shared, instead of a merged mesh rebuilt on every spawn and split: no more frame stall when a large asset such as a tree spawns or breaks.
- `PhysXCloth3D`: wind gusts vary across the cloth so it ripples and billows instead of tilting as a plane; cloth no longer falls through thin colliders such as ropes and rails; new `move_pins()` hangs cloth from pegs set closer than it is wide, so it falls in swags.
- Wet prints and drips read their contact wetness with a Catmull-Rom filter in raster and path tracing, so small prints keep a rounded shape instead of square texels.
- PhysXDebrisSurge3D finds the scene's GroundDebrisFlow3D by itself and its lead surge rides the mud body's front; later surges run behind it.
- Flow's default `heat_ramp` is a blackbody: flames have a bright core and sharp tongues, and cooler gas reads as smoke instead of an orange glow.
- The path tracer honours a light's `light_volumetric_fog_energy` when it lights participating media.
- PhysXFlow3D raster smoke lighting is on by default: `light_scattering`, `ambient_scattering`, `sun_scattering`, `cast_shadows` and `scattering_temporal` now default to true.
- PhysXParticleFluid3D (MPM): `mpm_auto_colliders` is on by default and takes static bodies and every shape of a body, so a basin built from boxes holds the fluid without listing it. Scenes that relied on only the bodies in `mpm_colliders` (for example particles meant to fall through a ground body and be handed to a height field) should turn `mpm_auto_colliders` off.
- MPM fluid spray is drawn as clear water drops; foam and bubbles stay white.
- PhysXParticleFluid3D's default liquid is clear water: transmission with refraction (n 1.33), Fresnel reflection and depth tint, instead of a translucent blue tint.
- MPM fluid surface is smooth: a wider surface kernel (three particle spacings) and normals continuous across grid cells, so it no longer shades as facets.
- PhysXParticleFluid3D defaults to `render_mode` Isosurface (a real mesh that casts shadows and shows in reflections and path tracing) with `foam_enabled` on.
- `GroundDust3D` and `CaveNetwork3D` follow the current camera when `follow_target` is empty, as the weather, the ground and the other world systems do. A cave network whose meshes were built with `build_meshes()` keeps them and does not stream.
- PhysXGroundGrains3D: each thrown speck keeps at least about 1.3 pixels across, so the spray arc reads from several metres away.
- PhysXHair3D.cards_enabled is on by default: far hair draws as thick strands per clump in raster.
- Path tracing: transmissive surfaces pick reflection or transmission by the Fresnel at the view angle instead of the head-on reflectance, so water and glass seen at grazing angles show their sky reflection without the bright speckle (about a third of the noise at the same sample count).
- PhysX: static bodies entering a space go into the scene together before the next step or query, through a pruning structure for large batches, so streaming in a region of static bodies costs less in its first frame. Adds `docs/bench/physx_openworld`.
- PhysX: bodies with the same friction and bounce share one material instead of each making its own, so streaming a region no longer costs a step that grows with the world (5,000 statics: 3.5 to 8 ms less), and large worlds stay under PhysX's 65,535-material limit.
- PhysX: a streamed region's static bodies enter the scene over a few steps (`physics/physx_3d/streaming/statics_per_step`, 1024), those near moving bodies and those a query reaches first, so nothing falls through or is missed; static removals go out in one batch. The first-step hitch of a region drops by up to a third with up to 10,000 statics loaded.
- Terrain features are on by default: `dunes_enabled`, `glacial_enabled`, `glacial_converged`, `tile_erosion_enabled`, `strata_enabled`, `guide_preview`, `ice_worn_detail`, `deposition` 0.5, `moraine_age` 2000, `scree_age` 10000, `meander_age` 3000, `outwash_age` 10000 (TerrainGenerator3D); `cliff_relief` 0.6 m (Landscape3D); `rock_detail_enabled`, `cliff_self_shadow`, `far_blend_enabled`, `far_start_from_vegetation` (TerrainMaterial3D). Turn them off where a project needs a faster generate.
- Terrain material: sand the generator laid (beaches, dunes) holds on slopes up to the cliff slope instead of turning to soil past 40 degrees.
- Terrain material: scree stones stand out of the gravel bed in parallax, with their own sun shadows under the raster and the path tracer (`rock_detail_enabled`).
- Terrain material: cliff faces break into irregular blocks with long bedding cracks and broken cross joints instead of a regular panel grid (`rock_detail_enabled`).
- Terrain material: thin soil on talus shows as grey debris, and the layer blend widens with distance so far slopes do not shimmer.
- Terrain generator: talus cones stay smooth, with no erosion gullies or ledges cut into them.
- Trees build faster: bark and moss textures are made once and shared by every tree generator, so forest-grown trees build about a third faster with identical results.
- Trees: smooth bark (beech) shows cross-stem creases, lenticel striations and a fine grain instead of soft blotches.
- Far tree meshes leave out the displaced bark relief (drawn by the near mesh) and the centimetre fruit; far date palm pinnae are two triangles. A wild date palm clump far mesh is about half the triangles.
- Trees: birch lenticels vary in length, thickness and spacing and gather in bands, instead of one dash per grid cell.
- Trees: far meshes of every broadleaf start at the lighter leaf level, with full leaves only near (`far_lod1`, on).
- Trees and shrubs: far meshes leave out leaf twigs under a millimetre, which only the near mesh draws (a mature lentisk's far bark 929k to 233k triangles).
- Gorse gets a near mesh like the other shrubs; its far mesh starts at the lighter spine, flower and pod levels (2.5M to 1.2M triangles mature).
- Trees grow about 15 % faster with identical meshes: leaf LODs, skin padding and needles fill their arrays in one pass, and the crown fire heights use selection instead of sorting every leaf vertex.
- Trees: krummholz mats grow as low domed cushions on many layered stems, their limbs under a skin of short needled shoots, instead of a flat table on one stem.
- Trees: the tree line follows the growing season's mean temperature (6.4 C), matching the alpine plants; flagged spruce sit just below it and krummholz mats just above.
- Trees: drying checks on logs are real splits, the bark's edge where it peels is a step of its real thickness, and moss margins are ragged and thin.
- Trees: full-leaf near meshes (`near_leaf_range` 60 m) and full-needle near meshes (`near_needle_range` 30 m) are on by default.
- Cork oak: a denser, darker crown on four thick limbs clad in cork, and virgin cork as thick ridges broken into blocks by wide, deep, dark fissures; the bole sweeps into its four limbs as one surface, its bark running on unbroken.
- Gorse and broom flowers and pods thin out with distance, as leaves do, with their colour kept: a mature gorse in summer draws about 70k crown-part triangles far off instead of 1.7M.
- Stone pine: the leader splits at about 5 m into three limbs that divide again, under a darker, denser cushion crown; stouter bole, long offset grey-brown bark plates with red-brown fissures (`BARK_PINEA`), and full-size cones set behind the needle tufts.
- Vegetation: the far colour that replaces plants past their range is matched live to the plants on screen, so the ground no longer lightens into a ring where the cover plants end (`VegetationField3D.far_match_view`, on by default).
- Vegetation: floor plants keep clear of a stone a tree's roots grow over (mesh meta `tree_root_stone`).
- Vegetation: fallen leaves and twigs no longer land on open ground far from any crowns (the litter rule's fade ran below zero litter).
- Ground mist sets its emitters only when a cell's place, density or wind changes, instead of seven property writes per cell every frame; the mist is unchanged.
- Ground mist reads wet ground, water and steam at each cell twice a second instead of every frame, and takes the ground height from the world's ground before casting physics rays.
- Oasis pools hold clear spring water (near pure water's absorption, little scattering), in raster and path tracing alike.
- An oasis spring whose ground is not hollowed below its level gets no pool or boils, with a warning; `TerrainGenerator3D.carve_oases()` carves it.
- Sand boils vary in size from vent to vent, with darker throats.
- Raster rivers mirror their banks where the screen shows them (screen-space reflections over the sky), instead of open sky everywhere; muddy water at grazing views keeps its brown instead of a blue-grey veil. Shallow turbid water shows its bed through the diffuse attenuation (Maritorena 1994).
- Flood water reads brown like milky coffee rather than pale milk: silt scatters forward as measured (backscatter about 2%) and carries the humic matter washed off with it; raster calibrated against the path tracer at flood loads.
- The contact wetness map dries and re-encodes only the area that is wet, and skips the GPU upload when nothing visible changed; the picture is unchanged.
- Wet tracks check each walking body's own-mud parts only when its children change, not every frame.
- Freezing rain reaches every reader of the rain share (cloth, hair, the snowpack, falling sleet), and a deep cold layer under the warm nose refreezes it to ice pellets instead (`WeatherMap3D.warm_nose_base`, Bourgouin 2000); `ice_storm_glaze` holds a glaze for tests.
- The weather map's cloud cover noise takes its per-update constants once instead of per cell (16 % fewer instructions, identical cover), and its texture packing runs on worker threads instead of the main thread.
- Ground cover reads for sand armour take the whole grid in one call under one lock, and stand-age lookups check one 64 m grid cell instead of every event; results are unchanged.

### Fixed
- `apply_radial_damage` on a scaled destructible: the point and radii are now in the asset's units.
- Blast pieces no longer fly apart and through the ground when they break: `PhysXDestructible3D.max_depenetration_velocity` (3 m/s by default) caps how fast PhysX pushes touching pieces apart, so projects need not cap it globally.
- Wet prints, drips and splashes stay drawn round the camera when the weather window follows a far-off anchor or vehicle.
- Raster Flow gas tinted by its colour once where the path tracer tints every scatter: near-white gas such as snow powder drew grey and coloured gas too light and neutral. Each multiple-scattering order now takes the colour to its own power, and the octaves are refit to the path tracer.
- The far side of a dense, bright Flow cloud lit from behind it no longer draws bright white in raster: its deep scattering orders fade by diffusion theory's transport coefficient.
- Screen-space refraction in MPM fluid no longer shows objects in front of the liquid.
- The foot of raster Flow gas lying on the ground drew dark: its sky and ground light now count the open side toward the camera, not only the gas straight above and below.
- Raster shadows of bright, forward-scattering gas (snow powder, steam) on the scene were too dark: they now pass the forward-scattered light on (delta-Eddington scaling); dark smoke keeps its full shadow.
- Flow gas lit from behind the camera drew mid grey with dark blotches in raster where path tracing shows it near white: multiply-scattered light now spreads evenly instead of leaning forward like single scattering.
- PhysXFlow3D: vehicles (bodies whose shapes live on their own PhysX actor) are solids for the gas again, from their CollisionShape3D children; before, they were skipped with an error each step.
- Path-traced smoke, dust and powder came out grey and see-through when the scene's bounce count was low: scattering inside gas now has its own budget instead of using up the surface bounces.
- Thin edges of Flow gas in raster were as bright as thick gas; light scattered more than once now needs gas around it, so wisps stay see-through as under path tracing.
- Raster Flow gas: a ground sample off the sun's shadow map no longer counts as fully sunlit and outvotes a shadowed one.
- A dark seam along the base of raster Flow gas lying on sloped ground: the snow under the gas was lit as if it lay at the gas box's floor, under the slope, and gas cut by the ground showed unlit where the scattering march ended a step early.
- Raster Flow gas: the scattering march's step budget counts only samples in gas, and the upsample no longer blends texels with no gas in as black.
- A build with Flow support no longer crashes when libnvflow.so / libnvflowext.so (or nvflow.dll / nvflowext.dll) are missing or incomplete: it reports one error and the Flow nodes stay inert.
- The lit face of large, thin-per-metre Flow gas (snow powder) drew grey in raster lit from the camera's side: multiple scattering now looks at the gas along the sun's whole line through the point, not only toward the sun.
- Flow smoke and dust in the raster renderer were lit up to 1.75 times brighter than under path tracing, which flattened their self-shadowing; they now match.
- Flow smoke and dust in the raster renderer now match the path tracer under a real sky: sunlit gas was too dark and sky-lit gas too bright (the earlier parity fix was measured against a background the path tracer does not light with).
- Horizontal banding in tall, dense Flow clouds in raster: the sun and sky volumetric shadow maps now step once per gas cell.
- Dense Flow gas (smoke above 1) drew no denser in raster than smoke 1; Flow's ray march now uses Beer-Lambert opacity, clamped at 2 as the path tracer does.
- A dark, dotted rim around Flow gas in raster: the light pass now uses the opacity of Flow's own ray march (Flow patch 0011).
- Raster Flow gas lacked the sky's blue: sky light is no longer halved or greyed by a whole-sphere average, and the ground under the gas is lit by the sky above it.
- PhysXFlow3D light scattering at reduced resolution no longer steps along the smoke's edges over sloped floors, and smoke in front of the sky now blends over time and upsamples smoothly.
- The ground under bright gas (snow powder, steam, white smoke) is no longer shadowed as darkly as under black smoke in raster: the gas's volume shadow passes the light it scatters many times (two-stream total transmittance), as under the path tracer.
- Raster Flow gas's shadow on the ground no longer stair-steps into a hard dark outline at the gas's edge: the shadow map averages four columns per texel and has a texel per two gas cells.
- The top of a deep, bright Flow cloud no longer draws dark under the sky: its sky light's scattering orders count the gas below the point as well as above, and in bright gas reach deeper by diffusion theory (bluer inside under a blue sky).
- Flow gas in strong wind no longer fills `max_blocks` within seconds: blocks the wind moves fast stay allocated only while they hold some smoke, so empty air the wind blows through is freed.
- Flow gas drifts on the weather wind again: the wind reserves the blocks along the plume's path, swept downwind from the sources for as long as the smoke lasts (gas lives only in allocated blocks, and Flow does not allocate ahead of it).
- Flow gas keeps time below 60 frames a second: it catches up with up to eight steps a frame, as Godot's physics does (it took one, so at 30 frames a second it ran at half speed).
- PhysXParticleFluid3D's isosurface casts shadows in the raster renderer (a shadow-only copy of the surface casts it).
- A smooth MPM pour no longer fills with white specks: trapped air is measured against each particle's own velocity field, so only liquid striking liquid makes foam and bubbles.
- `foam_size` set on an MPM fluid no longer rescales its foam spheres twice.
- MPM fluid takes as many substeps as `mpm_stiffness` needs (up to 128) instead of quietly softening the water, which made it sag like a gel in layers at fine particle sizes.
- Emitted fluid leaves at its rest spacing from a round emission sphere, so a high `emission_rate` pours a stream instead of a sheet or a burst.
- PhysXParticleFluid3D's raster shadow copy is left out of the path tracer, where it turned the water black.
- MPM fluid surface no longer shows bright cell outlines: closed air pockets inside the marched surface are smoothed out before meshing.
- MPM fluid streams fall at full speed again: velocity damping is per second instead of per substep, so many substeps no longer hold a falling stream under 1 m/s. Pours now thin and break up as they accelerate.
- PhysXGranular3D (and the sand grains a tyre throws) draws as particles again: the fluid's Isosurface default had turned the grains into a clear-water surface, so thrown sand was invisible.
- Wind obstacles on raised terrain counted their height from the weather's flat ground height, so they cast a lee far too long.
- A `PhysXDestructible3D` (dynamic, intact) moved by a script now goes there at rest instead of snapping back to its body; a `PhysXCloth3D` placed by a script before its first step is rebuilt there.
- Path tracing honours `Environment.ambient_light_source = AMBIENT_SOURCE_DISABLED`: diffuse bounces that leave the scene no longer gather the sky (and a procedural sky's sun glow), so a scene lit only by its sun matches the rasterizer (a white Lambert plane under a 30 degree sun: 0.498 traced, 0.497 raster, 0.500 analytic; before, the sky added 7 to 17%).
- Path traced fire and other glowing gas no longer breaks up into scattered bright specks at a few samples a pixel: the glow is gathered along the whole ray.
- Path tracing: a bounce ray that leaves for the sky no longer adds the weather's traced aerosol haze on top of the sky. The radiance map already holds that aerosol along the whole ray, and with the ambient light off the sky lights nothing, as in raster. A sun-only scene under a hazy WeatherMap3D was 17% too bright at a 30 degree sun and 35% at 12 degrees; it now matches the analytic value (0.502 vs 0.500).
- Path tracing: a mesh whose vertex() samples a texture now follows that texture when it is rewritten in place (`ImageTexture.update`, `texture_2d_update`, a 3D texture update, `texture_replace`), material or global; it kept the shape from before the change until something else moved it.
- The weather contract now answers rainfall, snowfall, canopy water and ground water input from the weather map; snow and rain on bodies read none before.
- Windows and macOS downloads build again after the caves, rocks, avalanche, ground, burnable and waterside changes (ambiguous math overloads and narrowing errors on MSVC and Apple clang).
- Under a thick smoke plume the sky keeps a wide ruddy glow round the sun (the light the smoke scatters forward lights its own forward lobe), and `SkyClock`'s adapting exposure counts the smoke, so the land stays visible in brown light instead of near black.
- Fires' smoke now dims and reddens the sky's sun disc and the sunlight its air scatters (the aureole round the sun, the sky under the smoke), by colour as it already did the sun light. Before, the disc stayed white under a thick plume. New `WeatherSkyMaterial.set_sun_smoke`; `set_aerosol` takes the smoke's share.
- Forest edges no longer repeat one tree: edge trees get more variants and turn more freely. A deer wood's edge is browsed, with its mantle eaten down to hedged thorns. Forest-grown sycamore and horse chestnut now grow leafy crowns instead of dense balls of bare twigs.
- Forest edge trees keep green limbs low on their open side.
- Trees and shrubs: no more "index_count >= index_array_len" errors from LOD levels no smaller than their base.
- Gorse and broom flowers read as flat ochre cards in raster: petals now fold along the midline, each flower turns its own way, golden yellow with a shaded throat, glowing against the light.
- `TreeGenerator.make_woodland` and `make_stand` with one variant built four slope variants of each stream-bank tree (about 47M extra triangles in a European woodland); they now build one.
- Tree crown parts (cones, berries, keys, catkins, flowers) were lit from behind in raster.
- Smooth bark (beech, hornbeam) drew about 1.5 times too bright.
- PhysX vehicles (car, motorcycle, tank) hold still at `Engine.time_scale` 0 instead of printing "timestep must be positive" every frame.
- Path tracing: bands along the depth contours under flat water, where the outer caustics cascades took their photons from every other pixel row and column.
- Path tracing: water surfaces (sea, lakes, pools, rivers) cast their shadows through the water, so sunlight reaching the bed is tinted by the depth it crossed on the way down as well as on the way back up; shallow beds no longer read pale and warm against raster.
- `add_sediment` docs: it is a rate held for 2 seconds after each call; a one-off mass goes to `add_sediment_mass`.
- SkyClock: with its clock held (a still), the sun now dims and reddens as fires' smoke is added or drifts over; it only followed the smoke while time moved. `get_smoke_transmittance()` is 1 with `smoke_haze` off.
- Raster water: the light reaching the bed is cut by the surface's Fresnel transmission on the way in (direct sun at its angle, sky light by its hemispherical average), as path tracing does; clear shallow water no longer reads brighter and bluer in raster than in path tracing.
- Silty and ashy water no longer path traces as glitter: the load enters the medium by the similarity relation (same reduced scattering, broader phase), so it converges. Silt now reads khaki-tan, the same hue in raster (multiple scattering calibrated against the path tracer) and path tracing. White water and falls in loaded rivers take the water's colour (beige in silt, grey in ash).
- Silty water reads khaki to brown instead of pink-orange: the silt's absorption follows measured mineral particles (Babin et al. 2003), and white water on it takes a lighter beige.
- Silty falls and white water: a fall's sheet on a loaded river scatters like the river (a tan curtain in raster and path tracing; path tracing drew dark brown glass), and white water on heavy loads is cream to beige.
- Torn spray sheets on storm crests take the sun's shadow in raster (they ignored it), so a sheet under a cloud or in a cliff's shadow is dark. River and water-surface benches gain `--shadow-bar=Y`, a board whose shadow falls across the water mid-frame.
- Surf foam no longer shows the shore wave grid as rows of short straight bars: PhysXWaterSurface3D reads its shore wave fields smoothly (cubic) and tears the roller's white water with wandering streaks; foam noise uses turned lattices, its holes cluster and its grain is patchy.
- Path-traced open sea no longer turns black toward the horizon: wave detail is filtered by the pixel's footprint under the path tracer too.
- `PhysXWaterSurface3D`: the sea no longer lifts its surface onto land more than a few metres above the still water (swash runs no higher). Land along the edge of its grid carried on outward through the clamped depth texture and stood in the sky over the horizon as thin dark posts.
- Raster silty water matches path tracing (within about 2% at light and heavy loads); it read warm tan where path tracing showed grey-brown.
- Wet prints and splashes ahead of a high or distant camera now show: the contact wetness square centres on the ground the camera looks at, not on the camera.
- A cloud base authored below high ground stays there, so ridges in the cloud still get fog drip.
- WeatherMap3D.advance() now sees canopies and walls added since the last frame.

### Improved
- Trees: beech and other smooth bark read mottled silver-grey up close, with smaller pale lichen crusts; smooth bark on dead wood stays grey; lying logs carry fewer crusts and their moss film lies in drifts; bare wood grain meanders and knots are larger.
- Trees: lying logs show deep drying checks, grain that bends round knots and patchy staining on bare wood; moss shoots fade into the carpet edge; bark stories (`bark_wounds`) are on by default.
- Trees: moss on logs and fallen limbs carries modelled shoots over its cushions; weathered bare wood shows ridges along the grain and a ragged torn bark edge; beech bark has softer folds and lenticels at 40 cm.

## build-f2ad55f2 (2026-11-16)

### Added
- PhysXFlow3D.scattering_temporal (off by default): the raster scattering pass jitters its march every frame and blends over a reprojected, neighbourhood-clamped history, so its step pattern no longer shows as bands at the smoke's edges.
- Trees: `bark_wounds` for lightning scars, frost cracks with ribs, deer and squirrel stripping, callus rolls and resin runs on living stems (off by default while tuned).
- Trees: coppice-with-standards, wood pasture and pollard row stands (`TreeGenerator.STAND_COPPICE_STANDARDS`, `STAND_WOOD_PASTURE`, `STAND_POLLARD_ROW`).
- Trees: winter buds on shoot tips, shaped and coloured by species.

### Changed
- PhysXFlow3D.ambient_scattering: sky light inside the smoke is dimmed by all the gas above each point, read with one fetch from a volumetric shadow map built looking straight down, instead of a 4-step march over 4 m. Tall plumes darken at the base and core as in path tracing.
- PhysXFlow3D raster smoke: the sun's smoke shadow is a volumetric shadow map (optical depth over 32 depth slices). `cast_shadows` reads it at each surface's depth, and `sun_scattering` lights the smoke from it with one fetch per sample instead of a 4-step march, so the whole plume self-shadows, not only the 4 m nearest. The quarter-resolution scattering upsamples with a wider joint bilateral filter, so the texel grid no longer shows at thin edges.
- Trees: catkins are modelled as spiral bract scales, open ones with anther tufts; birch witches' brooms are dense nests of kinked, branching twigs on a lumpy gall.
- Trees: ash keys hang in branched bunches with their seeds; berries hang on stalks with the dark calyx at their tips.
- Trees: rowan, ash and elder leaflets are cut from a texture with a smooth pointed outline, toothed margins, midrib and veins; leaf undersides are paler; rowan bark carries pale lenticels.
- Trees: pollard heads and coppice stools carry modelled burrs (clusters of hard-edged knobs) and deep fissures; poles swell out of their knuckles with folds of bark, carry girdle scars and, once thick, shallow flutes; pollard boles keep thickening every year.
- Trees: willow pollards bear a dense fan of long unbranched withies.
- Trees: hazel has its own glossy grey-brown bark with pale lenticels and peeling skin.
- Trees: coppice stools end their lobes in old rotted cuts under a rolled callus lip; rods swell at their nodes and show hazel's pale lenticels.
- Trees: pollard poles carry branch scars, and the folds where they leave the head are irregular and partial.
- Trees: rowan and elder berries hang in dense domed corymbs on branching red stalks; rowan leaflets are narrow and lanceolate.

### Improved
- Trees: smooth bark (beech) carries lobed crust lichen films, green algae drifts and damp patches instead of a stamped pattern; bare wood shows drawn fibres, growth bands and rays, sharp at 30 cm.
- Trees: sticks are short snapped pieces with kinks at their nodes, litter-brown and resting on the ground; fallen limbs rest on two points.
- Trees: logs settle onto the ground along their length; uprooted logs slope off the root plate and touch down partway out.
- Trees: root plates carry soil clods, stones and torn root stubs, with roots standing out of the soil.
- Trees: smooth bark (beech and the like) carries stemflow tracks, algae and crust lichen films.
- Trees: root plates show crumbly torn soil, a mat of fine roots and uneven, soil-crusted roots; smooth bark carries crust lichen colonies; compound leaves have a stalk.
- Trees: bare wood on logs and snags shows finer grain, rot staining, spalting zone lines and bark beetle galleries.
- Trees: pollard heads carry many small hard burrs over the bark instead of large smooth swellings; coppice stools are less dark.

### Fixed
- Trees: shed sticks and limbs no longer collapse onto their old parent's axis (some were invisible or floated); a lapsed pollard's dead poles end in snapped stubs instead of round balls.
- Trees: lapsed pollards regrow broad crowns from a few leading limbs instead of a fan of whips, and pollards in stands grow open-grown.
- Path tracing: alpha-cut materials with alpha to coverage are traced as alpha-tested opaque surfaces instead of transparent layers, so they no longer lose their indirect light.
- Trees: fallen wood's `tree_deadwood_axis` meta appends points along the stem's centreline with the girth round each, so a bent stick no longer reads as a thick log.

## build-9ed2033b (2026-11-16)

### Added
- Trees: cones, catkins, seed keys and berries in season, mistletoe on its hosts and witches' brooms on birches, as geometry: `TreeGenerator.fruit_cover`, `mistletoe`, `witches_brooms`.

### Changed
- Trees: pollard heads swell with every cut into knobbly, fissured, algae-filmed boles with clustered burrs and water shoots, poles are sawn back to their knuckle with a ringed cut face, and coppice stools spread into a low lobed mound.
- Trees: smooth bark (beech, hazel, hornbeam) carries fine crossways creases and stronger mottling.
- Trees: birch twigs and thin shoots are dark red-brown, turning white with girth.

## build-f0d8c981 (2026-11-15)

### Added
- PhysXFlow3D.ambient_scattering (off by default): in raster, the environment's ambient color and sky light the smoke as they light Godot's volumetric fog, dimmed by the gas above each point, with a multiple-scattering approximation so dense smoke is not black inside.
- PhysXFlow3D.cast_shadows (off by default): in raster, smoke shadows the scene from the sun through a Beer shadow map over the grid (Forward+).
- PhysXFlow3D.sun_scattering (off by default): in raster, directional lights light the smoke through the scattering pass with their shadow cascades, so walls shade it as they shade Godot's volumetric fog.
- Grass thins out under bracken and bramble stands: `PlantSpecies.sward_shade`, read by `GrassField3D.undergrowth_field` through `VegetationField3D.get_sward_shade_at()`.
- Bracken and bramble lay their own litter on the ground under their stands: `PlantSpecies.litter_color`, `VegetationField3D.get_sward_litter_at()`.
- Spatial shaders can set `OPAQUE_PREPASS_THRESHOLD` in fragment(): the alpha from which a `depth_prepass_alpha` fragment writes depth (0.99 by default)
- Hair colour variation: `grey_ratio` for single grey strands, `sun_bleaching` for lengths lightened and warmed toward the tips
- Far hair draws each clump as one thick strand, shaded as the clump, so its colour matches the strands, and hands over per segment once a clump is 2 pixels across or less, so there is no whole-head switch
- PlantGenerator `CLIMBER_IVY_ADULT`: the unlobed ovate leaves of old flowering ivy, for leaves high on mature climbing stems.
- Woodland-floor mushrooms as real geometry (stalk, cap, gills or tubes, ring, warts): fly agaric, cep, chanterelle, brittlegill, clouded agaric and honey fungus presets in `PlantGenerator`, placed by `make_floor` under their host trees, honey fungus beside deadwood, and fruiting from late summer to the first frost after rain.
- `PlantSpecies.patch_ring`: a species fruits along the edge of its patches, as fairy rings and arcs.
- Ferns, mosses, wood sorrel and seedlings gather at the foot of rocks, also in the gaps between their patches: `PlantSpecies.rock_foot`.
- Bracken and bramble stands have a graded margin: shorter plants over their outer metres, leaning out to the open. `PlantSpecies.margin_width`, `margin_scale`, `margin_lean`, `get_patch_depth()`.
- Plants between stones: PlantGenerator presets `PRESET_PARSLEY_FERN`, `PRESET_SPLEENWORT`, `PRESET_STONECROP`, `PRESET_SAXIFRAGE` and `PRESET_THRIFT`, `PlantSpecies.stone_gap` (grow only in the gaps between obstacles), and `PlantGenerator.make_stones()` for scree, river gravel and shingle.
- Moss on stone tops: `PlantSpecies.stone_top`, `VegetationField3D.stone_tops` and `stone_top_normals`; the scree and river sets carry top moss.
- Rocks: `RockGenerator` makes rocks and stones as real geometry, broken along the joints of five rock types (granite, limestone, sandstone, basalt, slate), rounded by transport (Bloore abrasion) and weathered by rock type, with LODs and collision hulls. `RockField3D` places them over a `TerrainGenerator3D` in six layers (rockfall, scree, glacial till, river beds, shores, cliff blocks) by the processes that put them there, streamed round the camera.
- Landscape3D `get_cliff_point()`: where the drawn surface lies over a point once `cliff_relief` has moved it (for water laid on faces); joint blocks stand out more and ledge steps are rounded over 1.5 m.
- TerrainGenerator3D: with `outwash_age` above 0, the basins the last glaciation wore are filled from the bottom up by the sediment their catchments yielded since the ice left, so a main trough gets a flat floor while small cirques keep their lakes.
- TerrainGuide3D: hand-drawn guides the terrain generator reads before erosion: raise (a ridge or peak), lower (a valley or bowl) and bar (hard rock at the curve's heights with a lip and an optional rock-slope apron), so drawn features still drain and look eroded; coast (sea inside or beside a drawn shore, which wanders naturally) and beach (beach width, or none, along a stretch of coast).
- TerrainGenerator3D `guide_preview`: a coarse run (`guide_preview_scale` times the cell size) a moment after a guide is moved or changed, so drawing guides in the editor shows their effect without a full run.
- TerrainGenerator3D touch-ups: a TerrainBrush3D under the generator is applied to the generated ground and the water solved again (`apply_touch_ups()`), so a raised dam backs up a lake and a dug hollow holds a pond; moving a brush re-applies it a moment later.
- TerrainMaterial3D `far_start_from_vegetation`: the far blend ends where a streaming VegetationField3D's placed plants end.
- TerrainMaterial3D `cliff_self_shadow`: ledges of Landscape3D `cliff_relief` shadow each other per pixel in raster (a short march toward the sun through the same relief); spatial shaders gain `sun_direction()` and the fragment output `SUN_SHADOW`.
- TerrainGenerator3D `glacial_converged`: the last glaciation solves its steady ice to near balance (multigrid, mass-conserving face thickness over steep beds) instead of fixed pseudo steps; off by default.
- TerrainGenerator3D glacial erosion: the supercooling stop measures the ice surface slope along the flow, so glaciers no longer cut one-cell slots hundreds of metres deep at the foot of cliffs.
- Trees: snapped ends get a splintered fracture face and torn bark; `TreeGenerator.limb_breakage` breaks limbs on living trees in storms, some left hanging.
- Trees: `TreeGenerator.uprooted` lays a fallen tree with its root plate (soil, stones, torn roots) and gives the mesh meta `tree_root_pit`; stands lay their freshest log uprooted.
- Trees: old ivy carries the unlobed adult leaves high on its stems.
- Streaming vegetation publishes where its placed plants end as the global shader parameter `vegetation_near_radius`; with several streaming fields the nearest end counts.
- Plants and grass keep out of rocks: `VegetationField3D.obstacles` and `GrassField3D.obstacles` (x, ground y, z, radius), e.g. from `TerrainGenerator3D.get_boulders()`.

### Changed
- PhysXFlow3D path tracing: heat glows in proportion to the smoke (the gas emits as much as it absorbs), as it does in raster, instead of by temperature alone; warm smoke no longer glows far brighter in path tracing than in raster.
- GrassField3D: a settings change no longer empties the field or stalls a frame: the old tiles stay drawn until their successors are built (nearest first), the compiled blade shader is kept when its code is unchanged, and the ground tint map is re-baked over several frames (1.5 ms each).
- Grass keeps growing at the thin margin of a bracken or bramble stand; it goes only where the fronds close.
- Hair cards default to 1024 per head (was 256), which hides the strips at mid distance; the bake runs on the worker threads
- Release downloads are named `berlenga-<os>-<arch>-<build>` (export templates `berlenga-export-templates-<build>.tpz`), and the macOS app is `Berlenga.app`.
- Trees: mesh meta `tree_deadwood_axis` carries the lying stem's measured reach along its axis (12 samples after the widest reach and the count).

### Fixed
- PhysXFlow3D light scattering: lights now scatter with a normalized phase and Godot's light energy units, as the path tracer and volumetric fog do, and light reaching through dense smoke includes a multiple-scattering approximation, so raster smoke brightness follows the path-traced one.
- The ground past the grass no longer reads brighter than the blades under a high sun: the far colour is baked at four sun elevations and follows the sun (`grass_ground_far_scale` carries the same change to a terrain's far cover).
- A patchy tall sward over an undercoat no longer leaves a lighter band past its blades: the ground between the tall patches takes the undercoat's far colour past the undercoat's own radius, and the far ground mixes the two by the tall sward's patch cover.
- Hair cards no longer show bright specks, stepped edges or over-lit crowns seen up close
- Hair cards seen up close no longer show a faint outline of the head through the hair
- Hair cards no longer show the head's outline through the hair: their dense cores draw depth first
- Hair cards hold snow as the strands do (far hair showed none)
- Strand hair casts shadows in raster again: on the face, neck, shoulders and ground (`cast_hair_shadows`, on by default)
- Dead bracken stands russet for some weeks before it lodges, instead of collapsing as soon as it browns.
- Moss cushions faced down (their outer surface was the back face) and their texture was about three times darker than measured moss; plants on fallen logs stand on the logs' moss instead of sinking into it.
- Ground plants beside a rock no longer push their leaves through it: they grow smaller and lean away.
- Ground plants of a field under another field's forest (`canopy_field`) no longer grow through its trunks: they keep out of each stem's foot (flare and root ridges), measured from the tree mesh at the soil line.
- Plants on and beside fallen logs follow the log's reach profile from the trees module instead of its widest reach.
- Reprojected sky clouds no longer show a checker or a 4x4 lattice: the march start is interleaved gradient noise shifted every frame, each pixel keeps a running mean clamped to this frame's samples, and the history follows exposure changes.
- Landscape3D cliff relief: vertices gather along the ledge lips and joints (split where the relief strays from the triangle edge), so path-traced ledge shadows are clean lines instead of beads; about 18% more triangles on cliff views.
- Landscape3D cliff relief: ledges are now lit per pixel by TerrainMaterial3D from the same relief the vertices follow, so their shadow edges are clean lines instead of a saw-tooth under a low sun.
- Tall falls: the notch now opens just below the brink instead of cutting through it, so the lake and river stay graded to the lip and no water is left hanging above the rock.
- A streaming vegetation field keeps its near radius published while its data map re-bakes, so the terrain's far blend no longer jumps back for the frames the tiles take.

### Removed
- Textured hair cards and their atlas bake: far hair is always thick strands now. `card_distance` is gone; `cards_enabled` and `card_count` turn on and size the thick strands

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
