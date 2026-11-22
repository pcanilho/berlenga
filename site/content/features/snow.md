---
title: "Snow"
weight: 75
group: "World"
summary: "A snowpack from the weather map that falls, settles, melts, slides and drifts; deep snow with tracks; baked winters and cornices."
---

## Snowpack

- **Snow cover** (`SnowCover3D`): a snowpack from the weather map, with snowfall, settling, melt, sliding off steep slopes and wind drift; snow and rain falling on open water melt into it, and fire melts the snow under it at any air temperature. Sea salt from spray melts shore snow and bare lake ice below freezing, until the brine is diluted or drains away with the melt (`salt_melt`).
- **Dirty snow** (`SnowCover3D.impurities_enabled`; RockField3D `rockfall_dust`): smoke, burnt forest, rockfall dust and other settling dust darken the snow and make it melt sooner, until fresh snow buries the dirt.

  ![Snow on a mountain, raster and path traced](media/snow-mountain.jpg)

  *Snow cover: deep on the cold summit, thinning to patches lower down.*

  ![Time-lapse of snow building up and melting on a mountain](media/snow-mountain.gif)

  *Snowfall and melt: snow builds up down the slopes, then melts back to the summit.*

- **Snowpack on a worker thread** (`SnowCover3D.background_updates`): snowpack updates run off the main thread and swap in when done, so the frame never waits; main-thread share per update about 6 ms down to about 1 ms.

- **Snow climate** (`SnowClimate`, `recipe`; `SnowCover3D.climate_map`): bakes a winter over the whole terrain from the terrain's climate recipe and precipitation map, with snow lines, shaded faces that keep their snow and loaded lee slopes; the cover carries it on with the weather and draws it far away.

  ![A peak after a baked winter, raster and path traced](media/snow-climate.jpg)

  *Snow climate: snow holds on the shaded and lee faces below the snow line.*

- **Fine detail kept in the wide snow map** (always on): drifts, slides and melt worked out near the camera are written back to the wide snowpack, so they are still there when the camera leaves and returns.

- **Shelter** (`SnowCover3D.canopy_enabled`, `shelter_collision_mask`): ground under roofs and overhangs stays bare; tree crowns hold snow and unload it later.

  ![A shelter and snow-laden trees, raster and path traced](media/snow-shelter.jpg)

  *Shelter: bare ground under the roof; tree crowns hold their snow.*

- **Wind moves lying snow** (`SnowCover3D.wind_transport_enabled`, `drift_obstacles`, `drift_relief`): windward shoulders and crests are scoured and the lee fills with wind-packed snow; snow drifts in behind rocks, walls, buildings and vehicles and blows off their tops; the wind shapes large drifts into dunes with steep lee slip faces and flattens small ones.

  ![A ridge after a day of 15 m/s wind: snow transport off, then on in raster and path traced](media/snow-wind.jpg)

  *Wind transport: after a day of 15 m/s wind the windward side is scoured bare and the lee is loaded.*

- **Spindrift** (`SnowCover3D.spindrift_enabled`): in strong wind, dry snow streams off sharp lee crests as plumes that drift downwind and thin as they sublimate.

- **Melt-freeze crusts** (`SnowCover3D.crust_enabled`; `SnowSurface3D.crust_plates`): a thawed surface refreezes into a crust that hardens each cycle; boots stand on hard ones and break thin ones into flat, sharp-edged plates tipped into the hole, with steps between them.

  ![Footprints in snow, soft prints without plates and crust broken into plates](media/snow-crust.jpg)

  *Melt-freeze crust: soft prints without plates, then the crust broken into flat plates tipped into each print.*

- **Suncups** (`SnowCover3D.suncups_enabled`): sunny spring melt digs cups into deep snow; warm cloud, rain or new snow smooths them out.

- **Rotten snow and slush** (`SnowCover3D.slush_enabled`): a wet spring pack bears less, so boots and wheels posthole deeper; melt water it cannot hold ponds at its base over frozen ground as grey-blue glossy slush, drains only downslope and refreezes in frost; prints in it fill with water.

- **Surface hoar** (`SnowCover3D.hoar_enabled`): frost feathers grow on the snow on clear calm nights and, once snowed over, stay buried as a weak layer.
- **Snow layers** (`SnowCover3D.layers_enabled`): crusts, buried hoar and depth hoar keep their own layers, and rain and melt water seep down through them and pond on crusts and fine-over-coarse barriers, where wet slabs fail; the walls of ruts, prints and trenches show the layers with their own grain, from fine fresh snow and smooth wind slab to glazed crust ledges, dark buried hoar seams and coarse depth hoar at the base.

- **Lake ice** (`SnowCover3D.lake_ice_enabled`): lakes freeze, slower under insulating snow, flood into white snow ice under heavy snow and thaw in spring; clear ice is dark with sparse cracks and walkable from 5 cm; heavier bodies and vehicles break through ice too thin for their weight, and the hole freezes over again.

- **Snow on bodies** (`SnowBodies3D`: `weight_enabled`, `slide_enabled`, `clumps_enabled`): snow lying on a body weighs on it, slides off tilted tops as slabs and drops off in clumps.
- **Roof snow** (`snow_roof` group; `SnowBodies3D.roof_piles`, `roof_slab_bodies`, `ice_dams`; meta `snow_roof_material`, `snow_roof_heat`): roofs hold the share of snowfall building codes give them and shed it by roofing, metal sooner than shingles and heated roofs sooner still; what slides off leaves the eave as separate rigid blocks that break on landing and settle as rounded debris on a soft bank, and heated roofs grow ice dams that hold the snow back.
- **Snow on materials** (BaseMaterial3D `snow_enabled`): surfaces are covered from the snowpack, flattest facets first, raster and path traced; `get_snow_shader_code()` for ShaderMaterials.

  ![Thin snow over the ground and props, raster and path traced](media/snow-props.jpg)

  *Thin snow: ground and props partly covered, raster and path traced match.*

  ![Time-lapse of snow building up on props](media/snow-props.gif)

  *Snowfall on props: the ground covers first, then the flatter tops of the props.*

## Deep snow

- **Deep snow** (`SnowSurface3D`, `snow_deform` group): deep snow as geometry with prints and tracks; bodies sink by weight (Bekker) and rest in their own tracks; tracks refill with new snow and wind; print edges stay clean at fine deform resolutions, with pushed-aside snow sliding at its repose angle.

  ![Prints and tracks in deep snow, raster and path traced](media/snow-tracks.jpg)

  *Deep snow: prints and tracks behind the bodies, raster and path traced.*

  ![Bodies leaving prints and tracks in deep snow](media/snow-tracks.gif)

  *Tracks: bodies sink in by weight as they move.*

  ![Footprints in deep snow at a fine deform resolution, before and after](media/snow-print-edges.jpg)

  *Print edges: spikes and toothed walls before; clean rims now.*

- **Deep snow on the GPU** (`SnowSurface3D.gpu_simulation`): pressing, slumping and refilling run in compute shaders, main-thread cost 1.5 ms down to 0.04 ms with the same prints; the CPU path stays for Compatibility.

- **Bow wave** (`velocity` on `SnowSurface3D.press()`, `press_box()`; automatic for bodies in the deform groups): fast bodies push the rim ahead (full at 4 m/s), leaving side berms and an end mound, mass kept; snow and ground.

  ![A sled's track in snow with berms and a mound at its end, raster](media/snow-bow-wave.jpg)

  *Bow wave: a sled's track with raised berms along its sides and a mound where it stopped.*

- **Rut walls give way** (`SnowSurface3D.wall_collapse`): rut and trench walls taller than their layers can hold shed blocks into a lumpy debris toe, sooner where a vehicle works the rut.

  ![A deep rut in a snowfield whose walls have shed blocks onto its floor, raster](media/snow-rut-walls.jpg)

  *Rut walls give way: an undercut rut wall with a scalloped top and a toe of fallen blocks along its foot.*

- **Packed-snow roads** (`SnowCover3D.traffic_enabled`, `get_grip_at()`): wheels and feet pack the snow they pass, spinning wheels polish it to an ice glaze, and tyre grip falls with it.

## Falling snow

- **Falling snow** (`SnowFall3D`): falling and blowing snow from the weather; flake counts, sizes and fall speeds from snowfall measurements, drawn as motion streaks; roofs shelter, snow blows above the wind threshold, `get_extinction()` feeds fog.

  ![Falling snow round a shelter at full size, raster and path traced](media/snowfall.jpg)

  *Falling snow: streaked flakes, none under the roof.*

  ![Snow falling and blowing round a shelter](media/snowfall.gif)

  *Falling snow in wind.*

- **Sleet** (`SnowFall3D.sleet_enabled`): near 0 C falling snow clumps into large wet flakes; above it the flakes melt as they fall, smaller, faster and greyer.

- **Snowstorms** (`SnowFall3D.haze_enabled`, `turbulence`): falling and blowing snow haze the air through the weather map (Pomeroy-Male suspension, 118 m visibility at 20 m/s); flakes ride turbulent eddies.

## Cornices and ice

- **Avalanches** (`SnowCover3D.avalanches_enabled`, `SnowAvalanche3D.trigger()`, `triggers_enabled`, `powder_enabled`, `forests_enabled`, `wet_enabled`, `glide_enabled`, `water_enabled`, `crowns_enabled`): slabs release where the snowpack's stability index fails, or under people, sleds, vehicles and falling cornices, also remotely along buried crusts, leaving a crown wall, flanks that fade downslope and a grey scoured bed; they run out as a Voellmy flow that picks up snow on the way and piles debris where it stops, and fast dry ones throw up a powder cloud that settles as fresh snow. Closed forest anchors the slab, and stems brake the flow and stop snow among the trees; the powder cloud snaps thin stems and crowns, and dense flow breaks or uproots trunks and carries them downslope (`TreeWind3D.avalanche_enabled`), and the stand starts again there, so avalanche paths stay young forest (`stand_damage`). In spring, meltwater weakens the pack; wet slides run slower and shorter and pile up deep in lumpy debris of snowballs and granules, and on a wet base the whole pack glides and releases to the ground. A slide onto a lake runs on over thick ice, breaks through thin ice and pushes a wave across open water.

- **Cornices** (`SnowCornice3D`): overhangs on lee crests, with collision; milder ranges grow them too (crest wind speed-up).

  ![A snow cornice overhanging a lee crest, raster and path traced](media/snow-cornice.jpg)

  *Cornice: a lip of snow over the lee side of a crest.*

- **Cornices crack and fall** (`cornice_failure_enabled`): a creeping tension crack, tumbling blocks, debris below.

  ![A cornice with a tension crack, then fallen into blocks below, raster and path traced](media/snow-cornice-fall.jpg)

  *Cornice fall: a crack opens behind the lip, then the lip breaks into tumbling blocks.*

- **Icicles** (`SnowIcicles3D`, `hazy_core`, `curtains`, `wind_direction`): grown from roof melt water below freezing; clear ice with a hazy core, tapering like real dripping icicles, leaning with the wind they grew in, with drops at dripping tips and curtains where neighbours fuse; they hang in clusters under drip points that wet and dry over the days, with stubs and bare gaps between.

  ![Backlit icicles under an eave, raster and path traced](media/snow-icicles.jpg)

  *Icicles: clear ice with a hazy core, longest where the most melt water drips.*

  ![Icicles under an eave, path traced at the default 3 bounces with transmission_bounces](media/icicles-transmission.jpg)

  *Transmission bounces: at the default 3 bounces the icicles stay clear instead of going dark.*

- **Rime** (`SnowRime3D`, `fog_water`, `droplet_diameter`; `WeatherMap3D.rime`): in freezing fog, rime grows on props and twigs as feathers pointing into the wind, and ground in freezing cloud turns white on the windward side of plants, rocks and cliffs until a thaw sheds it.

## Sparkle

- **Sparkle**: dry snow glints from ice grains (its own mirror lobe, raster and path traced), lost when wet.

  ![Snow surface close up: dry in raster, dry path traced, and wet](media/snow-sparkle.jpg)

  *Sparkle: dry snow glints in raster and path tracing; wet snow loses it.*

- **Exact path traced sparkle** (`snow_exact_glints`): every 2 mm ice grain is a facet hit per ray.

  ![Snow sparkle in raster, path traced with the grid model, and path traced with exact grains](media/snow-exact-glints.jpg)

  *Exact sparkle: each ice grain catches the sun on its own, densest round the highlight.*

- **Sparkle in path traced reflections** (always on): snow seen in a mirror, water or a window glints too, sized by each ray's cone, and light bounced off snow carries its glints.
