# Rain: falling, ringing, dripping, wetting, running off

Research note, 18 Nov 2026, written before the code. Goal: every piece of rain, driven by one
weather source (`WeatherMap3D`), on by default, in raster and under path tracing, with no loss of
detail under path tracing. Snow is docs/ideas/snow.md; water bodies docs/ideas/water.md; the
ground's wetness docs/ideas/material-wetness.md.

## What was missing (audit, 18 Nov)

- No falling rain: SnowFall3D draws flakes, nothing draws drops or streaks.
- No rings on water: the sea, lakes, pools, rivers and puddles ignored the rain on them.
- Rivers and lakes kept one discharge and one level whatever the weather.
- No drips from leaves or eaves, no splashes on the ground.
- Leaves never got wet (tree leaves had no wetness at all; ground plants dried with the ground).
- No runoff beyond the ground patch and the live-water windows.
- Rain and snow did not split by temperature with height.

## One source

`WeatherMap3D` owns the precipitation. New on it (all world positions, all on by default):

| Getter | Unit | Model |
| --- | --- | --- |
| `get_air_temperature_at`, `get_air_humidity_at` | deg C, 0..1 | the live air less 6.5 K/km (ICAO standard atmosphere) above the map's ground height (world links) |
| `get_rain_share_at` | 0..1 | Jennings et al. 2018 logistic in the air's temperature and humidity at that height (`rain_fraction()`) |
| `get_rainfall_at`, `get_snowfall_at` | mm/h | the cell's rate times the share, times one less the share |
| `get_canopy_water_at`, `get_canopy_wetness_at`, `get_canopy_drip_at` | mm, 0..1, mm/h | Rutter interception (below) |
| `get_ground_water_input_at` | mm/h | throughfall plus drip under canopy, rainfall elsewhere |

Shader globals: `weather_rain_map` (R precipitation mm/h, G canopy wetness, B drip mm/h) on the
map's rect (`weather_rain_rect`), and `weather_precip_phase` (temperature, lapse K/m, humidity,
ground height) so shaders split rain and snow at any height. The path tracer reads the same
through rt_params 75 to 79. `BaseMaterial3D::get_rain_functions_code()` hands any shader the same
functions, from one GLSL file (`raytracing_rain_inc.glsl`) the path tracer includes, so raster and
path tracing run the same lines.

Rain and snow agree: SnowFall3D draws `1 - share` of the precipitation, RainFall3D draws `share`
(sleet is both), drops stop on the snowpack's surface, and no rings form on snow or ice.

## Step 1: rings on water

Published methods:
- Lagarde 2013 ("Water drop 2b", Remember Me): ring layers, a jittered grid where each cell runs one
  ring at a time, period its life, random phase. Cheap, tileless, the standard in shipped games.
- Tatarchuk 2006 (ATI ToyShop): ripple normal maps animated by a simulation; tiles visibly.
- Simulated height fields (FluidNinja, Derelicts): beautiful but limited to a small window.

The physics: Marshall and Palmer (1948) drop sizes, N(D) = 8000 exp(-L D), L = 4.1 R^-0.21;
Atlas, Srivastava and Sekhon (1973) terminal speed v = 9.65 - 10.3 exp(-0.6 D). The flux of drops
per square metre between two sizes is their product's integral, in closed form (about 3,000 per
second above 0.5 mm at 10 mm/h). Each impact sends out a gravity-capillary packet (Le Mehaute 1988)
near water's dispersion minimum (wavelength about 1.7 cm, 0.23 m/s), height falling with the
square root of radius and viscous damping.

Built: Lagarde's layers, one per drop-size class (0.5 to 1, 1 to 2, over 2 mm), with the class's
flux setting how many cells are live. Impacts beyond one ring per cell, and rings finer than the
pixel, add their slope variance to the roughness instead (Bruneton, Neyret and Holzschuch 2010's
split of resolved and unresolved slopes): heavy rain dulls a pond's reflections as it does in life
and no ring aliases. The path tracer draws every ring (footprint 0). On the sea and lakes
(PhysXWaterSurface3D), rivers and live water (PhysXRiver3D, PhysXLiveWater3D) and the ground's
standing water (GroundSurface3D's ruts and puddles).

## Step 2: falling rain (RainFall3D)

Published methods:
- Garg and Nayar 2006 (SIGGRAPH, photorealistic rain streaks): a drop seen through a camera
  exposure is a streak of length v times exposure; its brightness is the refracted environment.
- Tariq 2007 (NVIDIA rain), Wronski 2014 (AC4 rain occlusion), Lagarde 2013: camera-following
  volumes of particles, occluded by a top-down depth map.

Built: a GPU particle box round the camera, sizes drawn from Marshall-Palmer, each drop relaxing
to terminal speed in the weather's wind plus eddies (`weather_wind()`, as SnowFall3D) so rain slants
with the wind; streaks one exposure long, refracting (transmission, IOR 1.33) under path tracing.
A top-down height field (GPUParticlesCollisionHeightField3D) keeps rain out from under roofs and
trees. Splashes where a drop lands: a crown when the Yarin and Weiss (1995) splash parameter
K = We Oh^-0.4 is over 2,100, a Worthington jet on water, otherwise nothing. The rain also thickens
the air: extinction from the same drop sizes (geometric optics, Q = 2), about 1.5 per km at
10 mm/h, set on the weather's haze.

## Step 3: rivers and lakes rise

PhysXRiver3D `rain_response` (on): every 10 s it reads WeatherRunoff3D's discharge at its upstream
end and adds it to its own; when that moves by a tenth it rebuilds, and depth and speed follow by
Manning as they already do. Lakes with `level_from_flows` add the runoff reaching them (less what
their rivers bring) to their inflow, so their level settles higher in a storm and their outlets run
harder. Follow-up: the lake's own storage (level-pool routing, the Puls method) would add the hours
of lag a large lake has; today the lag comes from the catchment's reservoirs only.

## Step 4: drips and splashes

Published methods: Tate's law (a pendant drop leaves at about 4.5 mm from a leaf tip or a gutter's
edge); Rutter et al. 1971 and 1975 canopy interception, the standard in hydrology: storage C fills
with the rain caught (1 - p of it, free throughfall p = 0.25), drains as D = Ds exp(b (C - S)) when
past capacity S (Ds = 0.0014 mm/min, b = 3.7 per mm, S = 1 mm), and evaporates at the wet canopy's
rate (Penman with the aerodynamic term about four times a field's: Stewart 1977, Gash 1979).

Built: the weather map keeps C per cell (`get_canopy_*`). RainFall3D drips from canopy cells at
their drip rate and from eaves it finds by casting down a grid round the camera (roofs with open sky
above and drop below), the eaves draining a roof store over `roof_drain_time`. The drips keep falling
after the rain stops, as under real trees.

## Step 5: wet plants

Leaves stay wet while the canopy holds water, after the ground has begun to dry, and the water
stands on waxy leaves as beads (contact angles 90 to 150 degrees on most leaves: Wang et al. 2014)
rather than as a film: small bright caps (Lagarde's "Water drop 4a"). Built as `rain_surface_beads()`
(5 mm cells, contact radius 0.6 to 1.3 mm, on upward faces), faded to its mean cover finer than the
pixel in raster and drawn at full detail under path tracing. On ground plants (PlantGenerator),
their impostors, and tree leaves and falling leaves (BaseMaterial3D `wetness_canopy`).

## Step 6: world-scale runoff (WeatherRunoff3D)

Published methods: Priority-Flood depression filling (Barnes, Lehman and Mulla 2014), D8 steepest
descent flow directions (O'Callaghan and Mark 1984), the SCS curve number for the share that runs off
(NRCS NEH 630, chapter 10), TR-55 shallow concentrated flow speed v = 4.918 sqrt(s) ft/s, and
Leopold and Maddock (1953) hydraulic geometry for channel speed, v = 0.63 Q^0.1. Routing as linear
reservoirs, implicit in time so any step is stable.

Built: a 4 km grid (256 cells) round the camera, baked from the terrain's heights; each step takes
`get_ground_water_input_at`, the curve number's runoff, and routes it down the D8 tree. Read by the
rivers and lakes above, and folded into the weather map's wetness so ground with runoff flowing over
it is wet where the rain did not fall (both renderers, no new parameters).

## More systems rain should reach (scan, 18 Nov)

- Rain occlusion for the ground and puddles (Wronski 2014, AC4; Far Cry 6): the shelter height field
  RainFall3D already renders could keep the ground dry under roofs and dense trees. Next.
- Dust: rain suppresses PhysX Flow dust and wets sand (sand stage 4 pore water already follows the
  weather); vehicle dust should stop on wet ground.
- Rain on snow: liquid share on a snowpack should densify and melt it (snow thread).
- Gutters and downpipes: eaves found by RainFall3D could feed pipe outlets and splash pads.
- Rain audio: impact sounds by surface (leaves, water, roofs, ground) from the same impact flux.
- Windscreens and glass: drops and runs on vertical glass (Lagarde's "Water drop 4b", streaks).
- Wet characters and cloth (PhysX wet cloth exists; worn clothing is PhysX's open card).
- Mud: rain fills ruts (mud reads the ground's standing water) and softens the clay.
