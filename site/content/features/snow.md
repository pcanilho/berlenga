---
title: "Snow"
weight: 90
summary: "A snowpack from the weather map that falls, settles, melts, slides and drifts; deep snow with tracks; baked winters and cornices."
---

## Snowpack

- **Snow cover** (`SnowCover3D`): a snowpack from the weather map, with snowfall, settling, melt, sliding off steep slopes and wind drift.

  ![Snow on a mountain, raster and path traced](media/snow-mountain.jpg)

  *Snow cover: deep on the cold summit, thinning to patches lower down.*

  ![Time-lapse of snow building up and melting on a mountain](media/snow-mountain.gif)

  *Snowfall and melt: snow builds up down the slopes, then melts back to the summit.*

- **Snow climate** (`SnowClimate`, `recipe`; `SnowCover3D.climate_map`): bakes a winter over the whole terrain from the terrain's climate recipe and precipitation map, with snow lines, shaded faces that keep their snow and loaded lee slopes; the cover carries it on with the weather and draws it far away.

  ![A peak after a baked winter, raster and path traced](media/snow-climate.jpg)

  *Snow climate: snow holds on the shaded and lee faces below the snow line.*

- **Shelter** (`SnowCover3D.canopy_enabled`, `shelter_collision_mask`): ground under roofs and overhangs stays bare; tree crowns hold snow and unload it later.

  ![A shelter and snow-laden trees, raster and path traced](media/snow-shelter.jpg)

  *Shelter: bare ground under the roof; tree crowns hold their snow.*

- **Wind moves lying snow** (`SnowCover3D.wind_transport_enabled`): windward shoulders and crests are scoured and the lee fills with wind-packed snow.

  ![A ridge after a day of 15 m/s wind: snow transport off, then on in raster and path traced](media/snow-wind.jpg)

  *Wind transport: after a day of 15 m/s wind the windward side is scoured bare and the lee is loaded.*

- **Melt-freeze crusts** (`SnowCover3D.crust_enabled`): a thawed surface refreezes into a crust that hardens each cycle; boots break thin ones into tipped pieces and stand on hard ones.

  ![Footprints in snow without a crust, then through a broken crust, raster and path traced](media/snow-crust.jpg)

  *Melt-freeze crust: boots break it into tipped plates instead of soft prints.*

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

## Falling snow

- **Falling snow** (`SnowFall3D`): falling and blowing snow from the weather; flake counts, sizes and fall speeds from snowfall measurements, drawn as motion streaks; roofs shelter, snow blows above the wind threshold, `get_extinction()` feeds fog.

  ![Falling snow round a shelter at full size, raster and path traced](media/snowfall.jpg)

  *Falling snow: streaked flakes, none under the roof.*

  ![Snow falling and blowing round a shelter](media/snowfall.gif)

  *Falling snow in wind.*

- **Snowstorms** (`SnowFall3D.haze_enabled`, `turbulence`): falling and blowing snow haze the air through the weather map (Pomeroy-Male suspension, 118 m visibility at 20 m/s); flakes ride turbulent eddies.

## Cornices and ice

- **Cornices** (`SnowCornice3D`): overhangs on lee crests, with collision; milder ranges grow them too (crest wind speed-up).

  ![A snow cornice overhanging a lee crest, raster and path traced](media/snow-cornice.jpg)

  *Cornice: a lip of snow over the lee side of a crest.*

- **Cornices crack and fall** (`cornice_failure_enabled`): a creeping tension crack, tumbling blocks, debris below.

  ![A cornice with a tension crack, then fallen into blocks below, raster and path traced](media/snow-cornice-fall.jpg)

  *Cornice fall: a crack opens behind the lip, then the lip breaks into tumbling blocks.*

- **Icicles** (`SnowIcicles3D`, `hazy_core`): grown from roof melt water below freezing; clear ice with a hazy core.

  ![Backlit icicles under an eave, raster and path traced](media/snow-icicles.jpg)

  *Icicles: clear ice with a hazy core, longest where the most melt water drips.*

  ![Icicles under an eave, path traced at the default 3 bounces with transmission_bounces](media/icicles-transmission.jpg)

  *Transmission bounces: at the default 3 bounces the icicles stay clear instead of going dark.*

## Sparkle

- **Sparkle**: dry snow glints from ice grains (its own mirror lobe, raster and path traced), lost when wet.

  ![Snow surface close up: dry in raster, dry path traced, and wet](media/snow-sparkle.jpg)

  *Sparkle: dry snow glints in raster and path tracing; wet snow loses it.*

- **Exact path traced sparkle** (`snow_exact_glints`, off by default): every 2 mm ice grain is a facet hit per ray.

  ![Snow sparkle in raster, path traced with the grid model, and path traced with exact grains](media/snow-exact-glints.jpg)

  *Exact sparkle: each ice grain catches the sun on its own, densest round the highlight.*
