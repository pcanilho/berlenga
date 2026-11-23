# Snow: falling, accumulating, deforming, tied to the weather map

Research note, 10 Nov 2026. Written before any code. Goal: snow that falls from the weather
map, settles by physical rules (altitude, temperature, slope, wind, shelter), melts, and is
deformed by characters and vehicles; one system for a street and for a mountain range, in
raster and under path tracing, with no loss of fine detail under path tracing.

## What we already have

- `WeatherMap3D` (docs/ideas/weather.md): per-cell wind, cloud cover, rain rate, water film,
  puddles, base `temperature` and `humidity`; shelter and canopy maps (#212); global shader
  uniforms `weather_state_map`, `weather_map_rect`. No precipitation phase: everything falling
  is rain today.
- `TerrainDataMap3D` (docs/ideas/terrain-data-maps.md): slope, curvature, flow, wetness,
  distance to water, baked per tile. Exactly the inputs the accumulation rules need.
- `TerrainDeformer3D` / Terrain3D by name (docs/ideas/terrain.md): authoring-time height
  edits; the note already says per-frame marks (footprints, ruts, snow trails) are a separate
  shading-plus-local-collision topic. This is that topic.
- BaseMaterial3D wetness (docs/ideas/material-wetness.md): the model for a material reading
  the weather map identically in raster and path tracing.
- Displaced surfaces under path tracing (#182, docs/ideas/displaced-hit-varyings.md): the
  BLAS is built from the displaced vertices, so displaced snow is real traced geometry.
- Grass trample field plan (docs/ideas/grass.md, stage 2.2): a camera-following texture that
  a compute pass fills by splatting PhysX bodies and capsules. Snow trails need the same splat.
- Random-walk SSS (#189) for snow's subsurface look under path tracing.

## Published methods

### Deformable snow in shipped games

- **Batman: Arkham Origins (Barre-Brisebois, GDC 2014).** One heightmap per snow surface,
  `min(512, surface/4)` texels, FP16/FP32, about 2 to 4 MB in total. Deformers are found by a
  quadtree and rendered into an ankle-high orthographic frustum from below the surface; the
  DX11 path keeps a minimum height and a projected displacement (so additive capture and
  snow banks work). Each accumulation pass subtracts a small amount, so trails refill. Raster
  only: relief mapping on consoles, tessellation plus displacement on PC (real geometry so
  shadows and AO follow the trail). Under 1 ms GPU for the update; about two surfaces updated
  per frame, nearest first. No persistence (listed as future work).
- **Rise of the Tomb Raider (Michels, Sikachev et al., SIGGRAPH 2015 Labs R&D).** Terrain
  heightmap + snow heightmap + deformation heightmap; dynamic objects write the deformation
  map; the snow mesh samples it, moves its vertices and reorients normals; the deferred
  variant clamps the deformation by the snow depth. Goals listed: trail-edge elevation and
  "fill in blizzard". Compares against Assassin's Creed III (render-to-vertex-buffer
  tessellation, persistent tracks, no edge elevation, no refill).
- **God of War Ragnarok (Surricchio, GDC 2023 "Reinventing the Wheel for Snow Rendering";
  80.lv interview).** Dropped screen-space parallax for real displacement with hardware
  tessellation. Physics capsules carve the displacement each frame; particles read it on the
  GPU to bounce. A GPU-driven pass spawns persistent mesh chunks (clumps) along trails by
  artist rules. Two draws per terrain mesh: a compute cull picks the sub-chunks inside the
  displacement area for the tessellated draw, the rest stays on the plain path.
- **Red Dead Redemption 2.** Deep snow and mud with trails and tessellated displacement;
  no detailed public talk found, only forum analysis. Not used as a source.
- **Unreal / Unity.** No built-in snow. The common project setup is a render target or Runtime
  Virtual Texture of trails around the player, sampled by the landscape material for
  displacement (Nanite tessellation in UE5) and a top-down depth or occlusion mask for cover.
  Same shape as Batman.
- **Godot addons.** Only tutorial-grade trail shaders (SubViewport camera under the ground,
  render target into a displaced plane). None has accumulation, melt, persistence or path
  tracing; nothing to reuse.

Common shape: a heightfield of snow depth, a camera-following deformation field written from
below by contacts, real displaced geometry, refill over time. Missing everywhere: physical
compaction (trails are painted depths, not mass-conserving), weather-driven depth, world-scale
accumulation, path tracing.

### Snow cover generation (graphics)

- **Fearing, "Computer modelling of fallen snow" (SIGGRAPH 2000).** Particles shot upward from
  each surface to find what sky it sees (occlusion, flutter, wind), then small simultaneous
  "avalanches" by a local stability test move snow off steep and unstable spots. The
  reference for shelter-plus-stability.
- **Festenberg and Gumhold, "Diffusion-based snow cover generation" (CGF 2011).** Snow
  deposition as diffusion of granular material over the scene; handles bridges and
  overhangs, local scheme. Useful for authored hero scenes, too heavy for a live open world.
- **Foldes and Benes (2007), Hinks and Museth (2009)**: occlusion by ambient-occlusion style
  sky visibility; wind-driven build-up. Same idea as Fearing at lower cost.

### Snow hydrology (the rules a mountain should follow)

- **Rain or snow.** Jennings and Molotch (HESS 2019), after Jennings et al. (Nature Comms
  2018): binary logistic model on air temperature and relative humidity,
  `p_snow = 1 / (1 + exp(-10.04 + 1.41 Ta + 0.09 RH))`, snow when `p_snow >= 0.5`. Humid air
  snows at warmer temperatures than dry air; a single 0 C threshold is wrong by up to a few
  degrees. Air temperature with altitude: standard lapse rate, 6.5 C per km.
- **New snow density.** Hedstrom and Pomeroy (1998): `rho_new = 67.92 + 51.25 exp(Ta / 2.59)`
  kg/m3 (about 70 to 200 kg/m3), so cold snow falls deep and light, warm snow wet and dense.
- **Compaction.** Settling toward a maximum density with time (CLASS, Verseghy 1991: an
  exponential approach toward about 300 to 450 kg/m3). Depth = SWE / density, so a pack sinks
  while keeping its water.
- **Melt.** Temperature-index (degree-day) melt (Hock 2003), with a radiation term for sun on
  slopes (Hock 1999, enhanced temperature index), which is what puts snow on north faces and
  takes it off south faces. Rain on snow adds melt and wets it.
- **Gravitational redistribution.** SnowSlide (Bernhardt and Schulz 2010): cells steeper than
  `Sm = 25 deg` holding more than their snow-holding depth (decreasing exponentially with
  slope, minimum 5 cm SWE at 75 deg and above) pass the excess to lower neighbours, processed
  highest first on the snow surface, mass conserving. Gives clean rock faces and filled
  gullies.
- **Wind redistribution.** Winstral et al. (2002, 2013): `Sx`, the maximum upwind slope within
  `dmax` (about 100 to 300 m) along the wind direction, averaged over a 30 deg window.
  Positive = sheltered (drifts, cornices on lee ridges), negative = exposed (scoured). Liston
  and Sturm (1998, SnowTran-3D) for the full saltation / suspension budget; blowing starts
  above a threshold wind of about 7 m/s for fresh snow (Li and Pomeroy 1997).
- **Interception.** Hedstrom and Pomeroy (1998): canopy holds snow up to a load set by leaf
  area and branch strength, unloads with warmth and wind; less snow under trees.

### Simulation of snow as a material

- **Stomakhin et al., "A material point method for snow simulation" (SIGGRAPH 2013).**
  Film-quality; offline. A local MPM patch could later drive close-up chunks and clumps, the
  PhysX module's granular solver is the path if we ever want it. Not needed for trails.
- **Terramechanics (Bekker 1969).** Pressure-sinkage for vehicle wheels in snow; with a
  compaction density gives rut depth and rolling resistance. Gives cars and characters real
  sinkage instead of a painted depth.

### Appearance

- Albedo from grain size and age (Wiscombe and Warren 1980; decay with age as in CLASS):
  fresh about 0.85 to 0.9 falling toward 0.6 to 0.7 for old wet snow.
- Subsurface scattering: blue in trails and edges (Batman faked it with a sky tint); under
  path tracing the existing random-walk SSS does it physically.
- Sparkle: Deliot and Belcour, "Real-time rendering of glinty appearances using distributed
  binomial laws on anisotropic grids" (EGSR 2023), stable across distance and anti-aliased.
  Earlier: Studio Gobo (2015) jittered-grid sparkle. Under path tracing glints are sampled
  by the same microfacet distribution; denoiser must keep them (check, rule: PT keeps detail).

## Design

A new `snow` module (every platform), no edits in weather or water files.

1. **`SnowCover3D`: the snowpack.** Camera-following map like `WeatherMap3D` (for example
   512 m at 1 m) plus an optional world tier (for example 32 m cells over the whole terrain).
   Per cell: snow water equivalent, density, age, liquid water. Each tick (CPU,
   deterministic, `time_scale` from the weather map): air temperature at the cell height by
   lapse rate, phase by Jennings, new snow by Hedstrom-Pomeroy density, compaction, degree-day
   melt with a slope-aspect sun term (cloud cover from the weather map), interception from the
   canopy map, shelter from the shelter map, SnowSlide on slopes from `TerrainDataMap3D`,
   Winstral `Sx` drift and scour. Out: global uniforms `snow_map`, `snow_map_rect`,
   `get_snow_depth_at()`, `get_snow_at()`, melt water per cell for wetness and rivers.
   **Climate bake** for mountains: run the same model over a season at high `time_scale` on
   the world tier and save it per tile, so a range has a believable snow line, gullies and
   cornices on load; the live model then carries it forward.
2. **Snow on every material.** `BaseMaterial3D.snow_enabled` (and `snow_coverage_bias`), like
   wetness: thin cover by surface normal, `snow_map` depth and sky visibility; raster and path
   traced from the same code. Roofs, cars, rocks, branches.
3. **Deep snow geometry on terrain.** `SnowSurface3D`: a displaced clipmap mesh on top of the
   terrain (Terrain3D by name or the HeightMapShape3D), depth = pack depth minus deformation;
   dense tiles only near contacts (God of War's split draw). Traced geometry under path
   tracing via #182, so trails cast real shadows and keep their edges.
4. **Deformation.** A camera-following deformation field (for example 64 m at 2 to 4 cm, like
   the grass trample field and shared with it: one PhysX contact splat, two consumers). Each
   contact compacts the column below it toward a compaction density (mass conserving: depth
   left = depth x rho / rho_compact), pushes a rim to the sides (the trail edge RotTR wanted),
   Bekker sinkage for wheels. New snowfall refills by adding SWE on top (Batman's refill, but
   physical). Trails persist: tiles leaving the window are written into the snowpack's own
   depth so they remain, coarser, when you return. Local PhysX heightfield patch so feet and
   wheels sit in the trail.
5. **Falling and blowing snow.** GPU particles in a box around the camera, flakes falling at
   0.5 to 1.5 m/s (Locatelli and Hobbs 1974), carried by `weather_wind` with gusts, stopped by
   the shelter map; blowing snow above the threshold wind near the ground and off ridges.
   Lit by the scene under path tracing (rule: lit effects under PT).
6. **Appearance.** Snow BRDF with age-based albedo, SSS, glints (Deliot-Belcour), wetness when
   melting.

Hooks for other systems: vegetation reads `snow_map` for snow load (bend, white tops) and
interception writes the canopy; grass is buried under depth; water gets melt into rivers;
cloth and hair can carry a snow mask; the terrain generator's masks feed the climate bake.

### Needs from sibling systems (asked through the coordinator)

- Weather: snowfall must not count as rain for the water film. Either the map exposes a
  precipitation phase, or `SnowCover3D` reports the snow fraction back. Melt water should
  feed the film and puddles.
- Grass: share the contact splat (one pass, one texture, two consumers).
- Terrain generation: snow climate inputs (aspect, elevation, Sx can be baked with its masks).

## Cost and checks

- CPU: snowpack tick like the weather map (512 x 512 cells, a few ms per tick at a low rate,
  spread over frames; benched in the cloud). SnowSlide and Sx bake per tile.
- GPU: deformation splat under 1 ms in Batman on 2013 consoles; ours is one compute pass.
  Displaced clipmap only dense near contacts. GPU timings go on the PC queue.
- Every step: raster and path traced 100% crops of the same frame; trails, rims and glints
  must survive path tracing and the denoiser.

## Stages (one PR each)

1. Snowpack model + snow on materials + bench (`--test=snow`): falls, settles, melts, slides.
2. Deep snow surface + deformation + collision (shares the grass contact splat).
3. Falling and blowing snow particles.
4. Mountain climate bake on the world tier (wind drift, cornices).
5. Appearance: glints, SSS, wet melting snow (built).

## Sources

Barre-Brisebois, Deformable Snow Rendering in Batman: Arkham Origins, GDC 2014. Michels, Sikachev et
al., Labs R&D: Rendering Techniques in Rise of the Tomb Raider, SIGGRAPH 2015. Surricchio,
Reinventing the Wheel for Snow Rendering, GDC 2023 (God of War Ragnarok). Fearing, Computer
Modelling of Fallen Snow, SIGGRAPH 2000. Festenberg and Gumhold, Diffusion-Based Snow Cover
Generation, CGF 30(6) 2011. Jennings and Molotch, HESS 23, 2019; Jennings et al., Nature
Communications 9, 2018. Hedstrom and Pomeroy, Hydrological Processes 12, 1998. Verseghy, CLASS, Int.
J. Climatology 11, 1991. Hock, J. Hydrology 282, 2003. Bernhardt and Schulz, SnowSlide, GRL 37,
2010. Winstral, Elder and Davis, J. Hydrometeorology 3, 2002. Liston and Sturm, J. Glaciology 44,
1998. Li and Pomeroy, J. Applied Meteorology 36, 1997. Stomakhin et al., A Material Point Method for
Snow Simulation, SIGGRAPH 2013. Bekker, Introduction to Terrain-Vehicle Systems, 1969. Wiscombe and
Warren, J. Atmos. Sci. 37, 1980. Locatelli and Hobbs, J. Geophys. Res. 79, 1974. Deliot and Belcour,
Real-Time Rendering of Glinty Appearances using Distributed Binomial Laws on Anisotropic Grids, HPG
2023. Gunn and Marshall, J. Meteorology 15, 1958. Magono and Nakamura, J. Meteor. Soc. Japan 43,
1965. Nemes et al., Snowflakes in the atmospheric surface layer, J. Fluid Mech. 814, 2017. Pomeroy,
J. Glaciology 35, 1989. Pomeroy and Gray, Water Resources Research 26, 1990. Rasmussen et al., J.
Applied Meteorology 38, 1999. Garg and Nayar, Photorealistic Rendering of Rain Streaks, SIGGRAPH
2006. Richardson, Stochastic simulation of daily precipitation, temperature, and solar radiation,
Water Resources Research 17, 1981. Cooper, The absorption of radiation in solar stills, Solar Energy
12, 1969. Kasten and Czeplak, Solar and terrestrial radiation dependent on the amount and type of
cloud, Solar Energy 24, 1980. Yu et al., Wind conditions for snow cornice formation in a wind
tunnel, The Cryosphere 17, 2023. Hancock et al., Quantifying seasonal cornice dynamics using a
terrestrial laser scanner in Svalbard, Norway, NHESS 20, 2020. Naito and Kobayashi, Experimental
study on the occurrence of cornice, Low Temperature Science 44, 1986. Zirr and Kaplanyan, Real-time
Rendering of Procedural Multiscale Materials, I3D 2016. Hedstrom and Pomeroy, Measurements and modelling of snow interception in the boreal forest, Hydrological Processes 12, 1998 (cited above for density). Roesch, Wild, Gilgen and Ohmura, A new snow cover fraction parametrization for the ECHAM4 GCM, Climate Dynamics 17, 2001. Taylor and Lee, Simple guidelines for
estimating wind speed variations due to small-scale topographic features, Climatological Bulletin
18, 1984. Warren, Optical properties of snow, Reviews of Geophysics 20, 1982. Pomeroy and Male, Optical properties of blowing snow, J. Glaciology 34, 1988. Pomeroy and Male, Steady-state suspension of snow, J. Hydrology 136, 1992. Schmidt, Vertical profiles of wind speed, snow concentration, and humidity in blowing snow, Boundary-Layer Meteorology 23, 1982. Kraichnan, Diffusion by a random velocity field, Physics of Fluids 13, 1970. Panofsky and Dutton, Atmospheric Turbulence, Wiley 1984. Schneider, The Real-Time Volumetric Superstorms of Horizon Forbidden West, GDC 2023. Hillaire, Physically Based and Unified Volumetric Rendering in Frostbite, SIGGRAPH 2015. Wang and Wade, Rendering Falling Rain and Snow, SIGGRAPH 2004 sketch. Langer, Zhang, Klein, Bhatia, Pereira and Rekhi, A Spectral-Particle Hybrid Method for Rendering Falling Snow, EGSR 2004.

## Stage 1: the snowpack and snow on materials (built 10 Nov 2026)

`modules/snow`: `SnowCover3D`, plus a Snow section on BaseMaterial3D. Bench `docs/bench/snow`
(`--test`, `--view=mountain|props`).

**The snowpack.** A square of cells that follows the camera (512 m at 2 m by default), each with
snow water equivalent (SWE), density, liquid water and albedo. Per update, for each cell:

- Air temperature from the weather map's, minus 6.5 C per km above its ground height.
- Precipitation (the weather map's rain rate) split by the Jennings logistic model; the
  probability is used as the snow share, so sleet is a mix and the change is gradual.
- New snow at the Hedstrom-Pomeroy density, mixed into the pack by depth.
- Settling toward the CLASS maximum density (450 dry, 700 wet, less for shallow snow), albedo
  ageing toward 0.70 dry or 0.50 wet, both e-folding in 100 h; 10 mm of new snow restores 0.84.
- Melt above 1 C by Pellicciotti et al. (2005), `M = TF T + SRF (1 - albedo) G`, TF 0.05
  mm/h/C, SRF 0.0094 mm m2/h/W. G is the clear-sky beam from the sun light (Meinel and Meinel
  1976) on the cell's own slope plus 10% diffuse, through the weather map's cloud shadow, or the
  Kasten-Czeplak (1980) cover factor when the map bakes no shadow. That is what keeps north faces
  white and clears south faces.
- Rain and meltwater held up to 5% of the ice, the rest leaving as runoff (`get_runoff_at()`, for
  wetness and rivers later); refreezing below 0 C at TF.
- SnowSlide every `slide_interval` (10 min of weather time): from the highest snow surface down,
  ground steeper than 25 deg keeps `50 mm x exp(0.074 (75 - slope))` of SWE and passes the rest to
  lower neighbours by their drop. Two departures from the paper, both for metre cells: what a cell
  holds follows the ground's slope (a deposit can be as deep as the cell is wide and would hide
  it), and snow never routes back up onto higher ground, which made terraces on a 60 deg face.
- Winstral Sx (dmax 100 m, a 30 deg window in 5 deg steps), baked on worker threads when the wind
  turns more than 10 deg. Above 7 m/s (Li and Pomeroy 1997), falling snow is weighted by
  `1 + 0.6 clamp(Sx / 15 deg, -1, 1)`, normalised over the map so drift moves snow without making
  any.

Out: `snow_map` (RGBA half: depth m, albedo, wetness, density / 1000) and `snow_map_rect`, and
`get_depth_at()`, `get_swe_at()`, `get_density_at()`, `get_albedo_at()`, `get_wetness_at()`,
`get_runoff_at()`.

**Snow on materials.** `snow_enabled`, `snow_amount`, `snow_max_slope` (60 deg),
`snow_from_weather`; the same lines in the raster shader and in the path tracer's material
evaluation (rt_params 45 to 49 carry the map, like wetness's 39 to 43):

- Covered share from depth and density, Niu and Yang (2007):
  `tanh(depth / (2.5 z0 (rho / 100)))`, z0 = 0.01 m.
- No snow past `snow_max_slope` on the mesh normal, full 20 deg below it.
- While the cover is partial it goes to the flattest facets of the normal map first (Fearing's
  local stability test, per facet), so thin snow lies in patches that follow the surface's own
  detail, the same in both renderers; near full cover it buries them.
- Snow albedo from the pack, ice reflectance (F0 0.018), roughness 0.7 dry to 0.35 wet, normal
  map flattened under the snow.

**Checks (`--test`, all pass).** Jennings 50% temperatures 3.93, 2.65, 1.38 C at 50, 70, 90%
humidity (formula exact); Hedstrom-Pomeroy densities exact; 20 mm of snowfall kept to 0.01 mm
through settling; temperature melt 6.0 mm a day at 5 C with 4.7 mm held as liquid; low sun from
the east melts the east flank (5.9 mm) more than flat ground (0.8) and the west flank (0);
snow line 408 m against 404 m from the lapse rate; a 60 deg face holds 152 mm (SnowSlide's
value), the foot gains, mass kept exactly; drift over a ridge at 14 m/s puts 4x more on the lee
flank, map mean kept at 20.00 mm.

**Cost (cloud CPU, 4 cores).** One update of 256 x 256 cells: 1.1 ms, 1.2 ms with drift, 2.1 ms
when a slide pass runs. At the default 2 updates a second that is under 0.5% of a 60 Hz frame
budget on average. The Sx bake (about 240 ms of work) runs on worker threads and the previous Sx
serves meanwhile. Material cost: one texture read and a few ALU per pixel where snow is enabled;
GPU timing queued for the PC.

**Known limits, for later stages.** Snow on a material reads the ground's map, so a surface
under an overhang shows the snow of the cell above (a top-down occlusion map fixes this, as for
rain). Cells entering the moving map take the nearest edge state until the world tier exists.
Weather counted snowfall as rain for its water film: fixed on the weather side (#219), which
also takes the pack's runoff as the film's input under the pack.

## Stage 2: deep snow geometry and deformation (built 10 Nov 2026)

`SnowSurface3D` (modules/snow). Methods and where we depart from them:

- **Geometry, God of War Ragnarok's choice over parallax.** A grid around the camera is lifted in
  its vertex shader to the ground plus the pack depth plus the deformation, so snow has real
  volume: it casts shadows, buries the feet of walls and is traced as geometry (displaced
  surfaces are traced from their capture, docs/ideas/displaced-hit-varyings.md). Instead of
  hardware tessellation (Godot has none) the grid is a tensor grid, 12.5 cm over the 32 m
  deformation field and growing by a fifth per step to 2 m at 48 m (309 x 309 vertices): no
  T-junctions, so no cracks and no stitching. The fragment stage recomputes the normal from the
  6.25 cm deformation texels plus 4 mm and 1.5 mm wind ripples, so print edges stay sharp
  between vertices in both renderers. Snow thinner than 3 cm stays the material effect of stage
  1; the surface tucks 5 cm under the ground there, and tapers to the ground over the last 8 m.
- **Deformation field, Batman AO / RotTR.** A 32 m field at 6.25 cm (512 x 512, dent and rim in
  half floats, 1 MB) follows the camera in 4 m tiles. Unlike those games the trail is not a
  painted depth: a pressed column keeps `1 - f` of the snow it loses as compaction, up to 450
  kg/m3, and pushes `f = rim_share` (0.3) into a rim ring half the print's radius wide. The
  deepest possible print is `D (1 - rho / rho_c) / (1 - f rho / rho_c)`, which comes out of the
  mass balance, so fresh powder takes deep prints and an old dense pack almost none.
- **Sinkage, Bekker (1969).** Loaded bodies sink by `z = (p / (kc / b + kphi))^(1 / n)` with
  Wong's snow values (n 1.6, kc 4.37, kphi 196.72): an 80 kg person on a 0.035 m2 boot sinks
  0.23 m in fresh snow. Round bodies and wheels solve for their own contact patch (it grows with
  sinkage) in a few fixed-point steps. Rigid bodies use their mass, characters 75 kg, any node a
  `snow_mass` meta; other nodes press to their underside.
- **Shapes.** Spheres and capsules press a bowl, upright cylinders a flat disc, lying cylinders
  a wheel track as wide as the tyre, boxes an oriented flat print (a box per foot gives boot
  prints in the walking direction).
- **Collision.** A heightfield patch (16 m at 12.5 cm) follows the deformed surface at 15 Hz, so
  bodies stand on the snow and sit in their own prints.
- **Refill and decay.** Half of each new snowfall settles into prints (the rest covers
  everything evenly); wind above the blowing threshold fills prints and planes rims; as the pack
  settles toward the compacted density the step shrinks to nothing; melt takes it with the pack.
- **Persistence.** Tiles leaving the field go to a cache (1024 x 16 KB by default, oldest out)
  and come back when the camera returns. Wind while away is not replayed.
- **Shared movers with grass.** The `grass_trample` group is in the default groups: one set of
  movers presses grass and snow. The grass keeps its own field; one splat feeding both is left
  for when grass moves its trample to the GPU.

**Checks (`--test`, all pass).** Boot sinkage 0.23 m at 22.4 kPa; surface on the pack to 1 mm;
a full press stops at 0.334 m (formula 0.334), column at 450 kg/m3; rim holds 30.0% of the
moved volume; collision ray hits the print floor to 1 mm; a boot box turned 90 deg prints along
its length only; a 25 kg ball dropped on the snow sinks 6.7 cm into its own print; a walking
character leaves a 0.21 m trail; the trail is the same after the field leaves (4 tiles cached)
and returns; 0.1 m of new snow fills prints by 0.05 m; after two weeks of settling the print is
at the mass limit; after a warm month there is no print and no snow.

**Cost (cloud CPU).** A physics step with four bodies pressing: 0.86 ms (press, refill,
collision patch, texture upload). GPU: a 190k-triangle displaced mesh with a handful of texture
reads per vertex and per pixel; under path tracing its capture and BLAS refit each frame. Both
timed on the PC queue.

**Known limits.** Prints are pressed straight down (no bow wave ahead of a fast body). The mesh
follows the pack's ground, not overhangs: snow under a roof or a bridge reads the cell's depth
(same as stage 1). Thin snow on the ground is the material's, deep snow the mesh's; the switch
at 3 cm is hidden by the taper but a terrain mesh much coarser than 0.25 m can poke through
thin snow.

## Stage 3: falling and blowing snow (built 10 Nov 2026)

`SnowFall3D` (modules/snow). Two GPU particle systems in a box that follows the camera, driven by
the snowfall rate and temperature of `SnowCover3D` and the wind of the weather map.

- **How many flakes, Gunn and Marshall (1958).** Snowflake size spectra are exponential in the
  melted diameter, `N(D) = 3.8e3 R^-0.87 exp(-2.55 R^-0.48 D)` per m3 per mm, R in mm/h of water.
  The count above the smallest visible flake times the box volume sets how many of `max_flakes`
  are alive: 26 flakes per m3 at 1 mm/h, rising with the rate.
- **How big, Magono and Nakamura (1965).** A flake's diameter from its melted size, `D = max(Dm,
  Dm^3 / 2)` mm, so heavy snow brings large aggregates; the spectrum is cut at 3.2 mm melted.
- **How fast, Locatelli and Hobbs (1974).** Aggregates fall at `0.8 D^0.16` m/s (0.8 to 1.2 m/s),
  and flutter sideways a few tens of cm/s (Nemes et al. 2017) instead of falling straight.
- **Wind.** The particle shader calls the weather map's own `weather_wind(p, h)`, so flakes follow
  the same gusts as grass, trees and rain, and slow under canopy and in the lee of shelters.
- **Shelter.** A heightfield collider follows the camera and catches flakes on roofs and under
  eaves; the box's floor is the snow top sampled from the pack, so flakes end on the snow.
- **Streaks, not sprites.** Each flake is a quad built in the particle shader: it faces the eye
  and is stretched along its screen motion over `streak_time` (1/60 s), with its brightness
  divided by the stretch so energy is kept, and a size floor of one pixel with alpha taking the
  rest. Rain renderers do the same (Tatarchuk, ToyShop 2006; Garg and Nayar 2006). No billboard
  flag, so the path tracer traces exactly the quad the raster draws. Flakes fade in from 0.4 to
  1.2 m so none fills the lens. The material is lit with a wrapped diffuse and a strong back
  term, since a flake is a loose ice lattice that lets light through.
- **Blowing snow, Li and Pomeroy (1997); Pomeroy (1989); Pomeroy and Gray (1990).** Dry snow
  starts to move above `u_t = 9.43 + 0.18 T + 0.0033 T^2` m/s at 10 m (8 m/s at -10 C). The
  amount follows the saltation flux, `u^3 (1 - u_t^2 / u^2)`, normalised at 1.5 u_t. Grains hop
  in a saltation layer `0.08436 u*^1.27` m deep (u* from the 10 m wind over z0 0.5 mm); a share
  lifts into suspension and drifts higher in stronger wind. Snow only moves where the pack is
  erodible: deeper than 2 cm, dry and lighter than 350 kg/m3. Stage 2 already fills prints with
  wind above the same threshold.
- **Visibility, Rasmussen et al. (1999).** `get_extinction()` integrates the flakes' cross
  sections over the size spectrum (extinction 2 x area for large particles), and
  `get_visibility()` is Koschmieder's 3.912 / sigma: about 2 km at 1 mm/h and 170 m at 3 mm/h,
  in the range Rasmussen measured. The bench feeds it to fog; a game sets its fog from it.

**Checks (`--test`, all pass).** 26.0 flakes per m3 at 1 mm/h; visibility falls with rate and
lands in Rasmussen's band; fall speeds 0.80 and 1.16 m/s for 1 and 10 mm flakes; blowing
threshold 7.96 m/s at -10 C and 9.43 m/s at 0 C; flux ratio 0 below threshold and 1 at 1.5 u_t;
a scene at -6 C snows; no blowing in light wind; a gale gives full blowing; nothing falls or
blows at +6 C.

**Cost.** CPU: setting shader parameters each frame and a 64 x 64 sample of the snow top every
4 m of travel or 2 s. GPU: up to 120k falling and 30k blowing alpha-blended quads, and under path
tracing their capture each frame. Both timed on the PC queue.

**Known limits.** Blowing snow does not yet add to the extinction (fog stays the snowfall's).
Falling snow does not add depth itself; `SnowCover3D` keeps the mass budget, so the flakes and the
pack agree by construction but not flake by flake. Flakes stop at the shelter heightfield, which
sees roofs from above only. Under path tracing a flake's alpha is a stochastic hit, so the faintest
streaks (flakes blurred by gusts) need the denoiser or more samples; at 16 samples without one
the flakes are there but fewer read than in raster.

## Stage 4: mountain climate, snow line, drifts and cornices (built 10 Nov 2026)

`SnowClimate` (a resource), `SnowCover3D.bake_climate()` and the world tier, `SnowCornice3D`
(modules/snow). A range should have a winter's snow on it when the game loads, laid by the same
rules that carry it on: the snow line where the lapse rate puts it, shaded faces white and sunny
ones bare, lee slopes loaded and cornices on the crests.

- **A winter's weather, WGEN (Richardson 1981).** The generator behind every hydrology weather
  generator since: wet and dry days from a two-state Markov chain (chance of a wet day after a dry
  and after a wet one), exponential amounts on wet days, the day's mean temperature a seasonal
  harmonic between the January and July means plus a lag-one autoregressive residual (cold and
  warm spells), wet days a degree cooler and dry days warmer by as much again over the wet share,
  a daily cycle on top. Wind: Weibull speed (shape 2) around a mean, stormier on wet days,
  direction scattered around the prevailing one. Sun: declination (Cooper 1969) and hour angle at
  the latitude, the beam through the air mass (Meinel) and the clouds (Kasten and Czeplak 1980).
- **The bake.** An internal cover over the whole terrain (for example 4 km at 16 m) steps the
  stage 1 model through the season in 3 hour steps, driven by the generator instead of the weather
  map: rain or snow by Jennings, more precipitation higher up (orographic gradient, 6% per 100 m
  by default, the order measured across the Alps), settling, melt by temperature and by the sun
  on each slope, SnowSlide, and drift by Winstral's Sx, baked once per 22.5 degrees of wind
  direction and reused. October to April over 256 x 256 cells takes 3 to 5 s.
- **The world tier.** With a baked climate the cover keeps that coarse pack as a second square,
  carried on by the live weather every eighth update (3.7 ms on 256 x 256 cells). Cells entering
  the near square start from it, then SnowSlide on the finer slopes sheds what they cannot hold.
  Beyond the near square materials read it: `snow_world_map` and `snow_world_rect` globals, the
  near square faded in over its outer tenth, in the raster shader and the path tracer alike
  (`rt_params` 50 to 54), so a range far off shows its snow line.
- **Cornices, Yu et al. (2023); Hancock et al. (2020).** Wind-tunnel cornices only grow between
  the threshold wind and twice it, fastest about 40% above it; below there is no transport,
  above it erosion wins. Per crest cell (the crest not overtopped upwind, the ground falling at
  30 degrees or more within two cells downwind) a cornice grows along the wind at up to 1.6 cm an
  hour (the field rate) times that band, from loose dry snow upwind (light, or fresh on top), to
  12 m at most; past twice the threshold it is scoured back, and in thaws it wastes back 0.5 mm an
  hour per degree (a snow degree-day factor over a dense slab). The wind at the crest is sped up by
  1.6 H / L (Taylor and Lee 1984; H the crest above the ground 64 m around, at most 60%), so
  milder, coarser ranges grow them too (stage 5 fix). `SnowCornice3D` sweeps a profile along each run of crest cells near the
  camera: a slab out from the crest top, as thick as it is long at the root and thinning to a lip
  that sags as it grows (field cornices; the wind-tunnel slab thickens more slowly than it
  lengthens), its roots reaching through the crest's snow to the ground, with collision.
- **Loose snow.** Stage 3's blowing snow and the cornices count a pack as loose when it is light
  (under 350 kg/m3) or its surface is fresh (albedo above 0.78, about two days), since a settled
  pack under new snow still blows.

**Checks (`--test=climate`, all pass).** Ten years of the generator: January mean -1.4 C against
-1.0, wet-day share 0.454 against the chain's 0.462, 9.0 mm on a wet day against 9.0; the noon
sun at 46 N 20.6 deg at midwinter and 67.4 at midsummer. A winter on the 700 m mountain (0.5 C
in January at its foot): nothing below 100 m by April, half covered from 400 m, all covered from
600 m; north faces 272 mm against 4 on south faces at 250 to 500 m; the lee (east) 232 mm against
82 windward at 300 to 600 m; the near square starts within 0.2% of the world tier. A cold ridge
with westerlies: 3.9 to 4.2 m cornices along the lee crest pointing east, none windward, none
with too little wind, none in gales; the mesh sweeps 125 pieces and a ray halfway out stops on
the overhang above the face in 20 places of 20.

**Cost.** The bake: seconds, once (at edit time or on first load). The world tier: 3.7 ms of
worker-thread time every eighth update of the near square, so about 0.5 ms a second at the default
rate. The cornice mesh: rebuilt every 64 m of camera travel. GPU: two more texture reads where a
material has snow, both renderers; timed on the PC queue.

**Known limits.** Drift redistributes falling snow (Winstral's Sx); snow already on the ground is
not carried, so drift and cornices are as coarse as the world tier's cells. The near square's
detail (prints, its own drift) does not feed back into the world tier. Cornices on a crest of
like cells come out alike; they waste back but do not break off as slabs. The world tier's
precipitation away from the weather map takes the map's edge.

**Raster and path traced.** The mountain matches at 100%: the same snow line, patches and
shaded faces, the path tracer's shadows crisper. Under the cornice the path tracer lights the
undercut with light bounced off the snow face below, as real undercuts glow; raster, without
bounce light, leaves it dark.

## Stage 5: sparkle, light through snow, wet snow (built 10 Nov 2026)

In `BaseMaterial3D`'s snow pass, `SnowSurface3D` and the path tracer alike.

- **Sparkle (glints).** Dry snow flashes where single ice grains are smooth facets turned to mirror
  the sun. The established real-time technique counts facets in the pixel's footprint over a
  multiscale grid (Zirr and Kaplanyan 2016; Deliot and Belcour 2023, which Unreal's Substrate
  glints use). Ours is that idea at one facet per grid cell, so it runs the same with or without
  derivatives (the path tracer's user shaders have none): the footprint is the pixel's angle times
  the distance, the cell the grain size (2 mm) doubled until it covers the footprint, up to six
  levels (about 13 cm cells). A quarter of the grains are facets; a cell holds one with the chance
  of its grain count, its normal GGX-tilted (alpha 0.4) from the snow's, and its weight is the
  share of the cell it covers, falling as one over the square root of the facets a cell would hold
  (the contrast of a binomial count). The facet is drawn as a second, near-mirror lobe: the coat
  lobe, which in raster takes `NORMAL` before the normal map as its normal, so the facet becomes
  `NORMAL` and `NORMAL_MAP` is solved to land the snow's own shading back on its normal (an earlier
  try that tilted the whole shading normal made the diffuse noisy at low sun). Ice F0 0.018 is 0.45
  of the coat's 0.04, times the weight; coat roughness 0.5 (alpha 0.05). The path tracer does the
  same on camera rays, with a new `coat_normal` on its material result (custom shaders' coats now
  also lie on `NORMAL` before the normal map, as in raster).
- **Ice specular.** Godot's F0 is 0.16 specular squared, so ice's 0.018 is specular 0.335; stages 1
  to 4 had 0.22 (F0 0.008). Fixed on all snow surfaces, flakes and cornices.
- **Wet snow.** A film of melt water (the pack's liquid, already smoothing the roughness from 0.7 to
  0.35) puts the glints out: gone at 30% of the water the pack can hold. Trampled snow keeps 30%.
- **Light through snow.** Snow scatters light millimetres to a few centimetres deep (Warren 1982).
  Tried on the cornice as subsurface scattering at strength 0.5 (5 mm under the path tracer's
  random walk): no visible change at the scale of a slab, and grain in the path traced undercut
  at 16 samples, so it is off by default. The cornice mesh is now closed at the end of every run,
  so a material that wants it can turn it on; the path tracer already lights the undercut by
  bounce off the snow below.
- **Showcase fixes (from the landscape bench).** Cornices: the thaw loss was ten times too strong
  and crests saw the valley wind; with both fixed a ridge at -4.5 C in January on 16 m cells grows
  3 m cornices. `get_depth_at`, `get_swe_at`, `get_density_at`, `get_albedo_at` and
  `get_wetness_at` read the world tier outside the near square.

**Checks.** `--view=sparkle` prints the share of pixels a fifth brighter than the ring around them:
0.63% on dry new snow against a low sun in raster, 0.28% path traced at 16 samples (the path
tracer averages the cells under a pixel, as TAA would), 0% after six hours of thaw. `--test=climate`
adds a milder ridge on 16 m cells: 3.1 m cornices.

**Cost.** One hash, a few lines of maths and a 3 x 3 inverse on snow pixels that glint, the coat
lobe on those pixels; GPU time on the PC queue.

**Known limits.** One facet per cell, not a binomial count per direction, so at a distance the
glints thin out rather than turn into a smooth sheen (the rough lobe carries that). Bounce rays in
the path tracer see plain snow. A material's own clearcoat is replaced on glinting pixels.

## Follow-up 1: snow under roofs and trees (built 10 Nov 2026)

- **Overhangs.** The pack lies on whatever its downward cast meets first, so a roof in the cast already
  carries the roof's snow. Until now materials under it still read that cell and showed snow on the
  ground beneath. `SnowCover3D` now publishes a catch map (`snow_catch_map`, the snow map's rect, RGBA
  float): the height the pack lies at, the canopy's load over its capacity, the canopy closure. A
  surface 0.4 to 0.8 m or more under that height is under cover and stays bare. The same
  rule runs in BaseMaterial3D, the path tracer (`rt_params` 55) and SnowSurface3D (no deep snow under a
  roof, in the mesh and its collision). With a height source (terrain), roofs join the cast through
  `shelter_collision_mask` (off by default, as before).
- **Tree crowns, Hedstrom and Pomeroy (1998).** Over WeatherMap3D's canopy (the vegetation writes it;
  `get_canopy_closure_at`), a canopy of closure C holds I = (I* - I0)(1 - exp(-C P / I*)) of a fall
  P, up to I* = 5.9 (0.27 + 46 / rho_s) LAI kg/m2, LAI from C by the vegetation's own Beer's law
  (1 - exp(-0.5 LAI)). It unloads onto the snow below toward thaw and in wind (Roesch et al. 2001, the
  CLM5 rates: I (T - 270.15) / 1.87e5 and I u / 1.56e5 per second) and drips off in a thaw. Surfaces
  1.5 to 3 m or more above the pack under a canopy (the crowns, no colliders) show the canopy's load
  instead of the ground's pack. `canopy_enabled` (on), `get_canopy_load_at`, `get_canopy_capacity_at`.

**Checks (`--test=shelter`, in `--test`).** A roof 3 m up catches the snow (catch 3.1 m under it, 0 in
the open, 12 mm on it); with a height source the roof is found only with `shelter_collision_mask`.
A stand of closure 0.86 holds 8.9 mm of a 20 mm fall (capacity 17.2 mm), 11.1 mm reaches the ground
(field interception of 30 to 60% of a storm in dense conifers); the water is kept to the kilogram; a
day at -1 C in 6 m/s wind unloads all but 0.1 mm onto the snow below. `--view=shelter` renders a shed
and spruces, raster and path traced alike.

**Cost.** One more RGBA float texture read where a material has snow; the canopy is read again every
sixteenth update (one lookup per cell). **Limits.** No sublimation of held snow (Pomeroy et al. 1998:
up to a third in sunny cold climates); one canopy layer; drip-line build-up off roofs not modelled.

## Follow-up 2: snowstorms (built 10 Nov 2026)

How others do it: Horizon Forbidden West's superstorms (Schneider, "The Real-Time Volumetric
Superstorms", GDC 2023) and Frostbite's unified volumetrics (Hillaire, SIGGRAPH 2015) carry a storm's
depth in a lit participating medium, with particles only near the camera. Wang and Wade ("Rendering
Falling Rain and Snow", SIGGRAPH 2004) and Langer et al. ("A Spectral-Particle Hybrid Method for
Rendering Falling Snow", EGSR 2004) fill the distance with textures or spectra instead, which the path
tracer cannot see, so here the medium carries it.

- **Haze, through the weather map's hook (#225, #226).** SnowFall3D pushes the falling snow's
  extinction (stage 3: Gunn-Marshall flakes, geometric optics) as uniform haze, and the blowing
  snow's as a layer over the snow under the camera, 150 m around, re-pushed as the camera moves.
  The layer is Pomeroy and Male's suspended-snow profile (PBSM, 1992): eta(z) = 0.8 exp(-1.55
  (z_r^-0.544 - z^-0.544)) kg/m3 over the saltation top z_r = 0.05628 u*^1.27, extinction by
  geometric optics over 40 um grains (Pomeroy and Male 1988; Schmidt 1982), as an exponential
  layer fitted between 0.2 and 2 m (0.68 m deep). Single-scatter albedo 0.97, lit by the sky. Both
  renderers draw both parts. `haze_enabled` (on).
- **Gusts and eddies.** Flakes and lifted snow ride the weather wind plus turbulence: six of
  Kraichnan's random Fourier modes (Kraichnan 1970, divergence-free), 2 to 14 m, frozen in the mean
  wind (Taylor) and slowly turning over, sigma_u = 2.5 u* (Panofsky and Dutton 1984), weaker within
  half a metre of the snow. `turbulence` (1).
- **Deeper drifting.** The lifted wisps spread through the layer's depth; the blowing box grows with it.

**Checks (`--test`).** Visibility at 1 m in blowing snow: 21 km at 10 m/s (drifting below eye
level), 760 m at 15, 118 m at 20 (a blizzard: under 400 m in 15.6 m/s or more), 35 m at 25; the
layer holds 14 times as much snow at 0.2 m as at 2 m; a storm at 18 m/s puts 0.019 /m on the
weather map at 1 m over the snow (snowfall 0.009, blowing 0.013) and the snowfall's alone at 20 m.
`--view=snowfall --wind=20` renders it, raster and path traced.

**Cost.** Six cosines per particle per frame for the eddies; the haze is the weather map's (an
environment fog density and one fog volume). **Limits.** Beyond the fog volume's 300 m only the
falling part hazes the distance; the layer is flat over the 150 m around the camera.

## Follow-up 3: the deform field moves to the ground module (built 10 Nov 2026)

Footprints in bare ground (docs/ideas/ground-deform.md) need the same field, so the field (dent and rim
arrays, the tile cache, pressing with mass conservation, the body scan) moved to `modules/ground`
(`GroundDeformField`), with the ground query `SnowCover3D` used (`GroundHeightQuery`). `SnowSurface3D`
keeps its own rules (snow depth and density decide the deepest print, snowfall and wind refill it) and
behaves as before; its checks pass unchanged.

## Follow-up 3d: crusts that boots break through (built 10 Nov 2026)

**What is known.** The snow classification (Fierz et al. 2009) names the crusts a walker meets: the
melt-freeze crust (MFcr), "at most a few centimetres thick", whose "strength increases with number of
melt-freeze cycles"; rain and sun crusts (IFrc, IFsc), thin glazes of ice; wind crusts, "hard but
usually breakable". Layered snowpack models (SNOWPACK, Lehning et al. 2002; Crocus, Vionnet et al.
2012) follow every layer's grains, liquid water and refreezing; a crust is a layer whose wet grains
froze into polycrystals. Hardness in the field is the hand test (Fierz et al. 2009, Table 1.4): the
object that goes in at 10-15 N, fist (F), four fingers (4F), one finger (1F), pencil (P), knife (K),
or ice (I), with ram resistances of 0-50, 50-175, 175-390, 390-715, 715-1200 and over 1200 N. Shear and
tension tests (Jamieson and Johnston 1990; Abe 2004) give strength by density, but for layers in the
pack, not for a crust loaded through.

**What we built.** Only the top crust is kept, per snow cell: its thickness, melt-freeze cycles, the
snow over it, and whether it is wet.
- A surface wetted by melt or rain that refreezes becomes a crust. The refrozen water thickens it, at
  250 kg/m3 of density gained in the layer it freezes in (our estimate), up to 5 cm. Frozen through, it
  counts one more cycle. While wet again it bears nothing.
- New snow and snow sliding down bury it; melt takes the snow over it first; the snow over it settles
  with the pack. When the snow over a buried crust gets wet, a new top crust starts there.
- Bearing pressure from the hand test: 12 N over each object's face (fist 100, four fingers 20, one
  finger 2, pencil 0.5, knife 0.1 cm2, our estimates) bounds each hardness; the geometric centres of
  the bands are F 0.5, 4F 2.7, 1F 19, P 120, K 540 kPa, ice 1.2 MPa. A first-cycle crust is 1F, each
  further cycle half a step harder (48 kPa after two, 120 after three). Thinner than 2 cm, it bears in
  proportion to its thickness (punching shear grows with it).
- `SnowSurface3D`: a body's pressure on its contact patch against the crust. Under it, the body stands
  on the crust and only the snow over the crust compacts. Over it, or driven through at a set depth, it
  breaks: a crust on soft snow is a plate on an elastic foundation, which cracks radially from the load
  and around it near its characteristic length l = (E h^3 / (12 (1 - nu^2) k))^(1/4) (Hertz 1884;
  Hetenyi 1946). With E ~ 50 MPa and k ~ 1e5 N/m3 (our estimates) a 2 cm crust has l ~ 0.13 m, growing
  as h^(3/4). The pieces out to a jagged crack line (seven radial sectors, each its own reach) tip into
  the hole, their inner edges down by up to half its depth, and the rim heaps in 12 cm plates.
  Everything goes through the shared deform field, so both renderers trace it.
- An adult's boot (75 kg on 0.035 m2, 21 kPa) breaks a first-cycle crust and stands on a second-cycle
  one: the classic spring morning, where the walker on the hard crust at dawn starts breaking through
  once it thaws (wet crust bears nothing).

**Checks** (`docs/bench/snow --test=crust`, also in `--test`, 8 of 8): the bands give 19, 48 and
120 kPa; new snow has no crust; ten hours at +8 C then fourteen at -8 C make a 1.5 cm first-cycle crust
(wet in the thaw, bearing 0) bearing 14 kPa; a walker breaks it (0.18 m deep), and 25 cm from the
trail's middle the surface is 1.2 cm down, varying 40% along it (cracked pieces tipped in; 0 without a
crust), the rim holding no more than the pushed share; a second thaw and frost give 3 cm, two
cycles and 48 kPa, and the walker leaves no print; 10 cm of new snow over it: the walker compacts that
(0.055 m, the limit to the crust 0.057 m) and stops; with `crust_enabled` off the same walker sinks
0.21 m. The other snow checks (`--test`, `climate`, `shelter`) and the ground checks pass unchanged.

**Shots.** `--view=trail --close --crust=1` against `--crust=0` (boots and balls through a 2 cm crust:
wider holes with tipped pieces and lumpy rims) and `--crust=2` (no boot prints), raster and path traced.

**Not built.** Deeper crusts (only the top one is kept); wind crusts (wind packing, follow-up 4 moves
lying snow); the glaze's look (a shinier surface on a sun or rain crust); crust sound. A wet crust
bearing a little would need wet-snow strength (not found).

Sources: Fierz, Armstrong, Durand, Etchevers, Greene, McClung, Nishimura, Satyawali and Sokratov, The
International Classification for Seasonal Snow on the Ground, IHP-VII Technical Documents in Hydrology
83, UNESCO-IHP 2009. Lehning, Bartelt, Brown, Fierz and Satyawali, A physical SNOWPACK model for the
Swiss avalanche warning, Part II: Snow microstructure, Cold Regions Science and Technology 35, 2002.
Vionnet, Brun, Morin, Boone, Faroux, Le Moigne, Martin and Willemet, The detailed snowpack scheme
Crocus and its implementation in SURFEX v7.2, Geoscientific Model Development 5, 2012. Jamieson and
Johnston, In-situ tensile tests of snowpack layers, J. Glaciology 36, 1990. Abe, Shear strength and
angle of repose of snow layers including graupel, Annals of Glaciology 38, 2004. Hertz, Ueber das
Gleichgewicht schwimmender elastischer Platten, Annalen der Physik 258, 1884. Hetenyi, Beams on
Elastic Foundation, University of Michigan Press 1946.

## Follow-up 4: the wind moves lying snow (built 10 Nov 2026)

**What is known.** SnowTran-3D (Liston and Sturm 1998; in SnowModel, Liston and Elder 2006) is the
standard: a mass balance dS/dt = -div(Qs + Qt) - sublimation on the snow grid, with saltation Qs
(Pomeroy and Gray 1990) and turbulent suspension Qt (Pomeroy and Male 1992's PBSM profile), driven by a
wind that MicroMet (Liston and Elder 2006) weights over the terrain by its slope along the wind and its
curvature, W = 1 + gs Os + gc Oc. The threshold rises with density: u*t = 0.10 exp(0.003 rho) (Liston
et al. 2007). Tabler (1991) measured total transport in the lowest 5 m in Wyoming as
Q = U10^3.8 / 233847 kg/(m s). Winstral's Sx (already used for falling snow) classifies shelter but moves
nothing that is already down.

**What we built** (`SnowCover3D.wind_transport_enabled`, on by default, near square and world tier):
- Flux per cell from the 10 m wind times MicroMet's weight (slope and curvature each scaled to +-0.5
  over the map, gs = gc = 0.5, curvature over `wind_curvature_length`, 20 m): saltation by Pomeroy and
  Gray plus PBSM suspension integrated to 5 m (tabled by wind speed), faded in from the threshold so the
  flux has no step there (our smoothing). Threshold: the higher of Li and Pomeroy by temperature and
  Liston's by density.
- Donor-cell transport: each cell sends its flux through its downwind faces, at most the snow it can
  give, in substeps of at most 2 mm through a face. Snow comes in at the upwind edge of the map as from a
  cell like the edge one and leaves at the downwind edge (an open map, so a map-wide total is kept only
  when the edges match).
- Laid down wind-packed at 300 kg/m3 (our pick within the 250-400 of wind slabs), burying a crust; taken
  from the top. Nothing moves under a frozen crust, in wet or thawing snow, or under trees (closure over
  0.2).

**Checks** (`--test=transport`, also in `--test`): the flux is 0.048 kg/(m s) at 15 m/s and 0.162 at
20 m/s against Tabler's 0.126 and 0.376 (within a factor of 3; at 10 m/s, 0.008 against 0.027, nearer
the threshold his fit, which has none, stays higher); no transport at the threshold, in snow at +0.5 C,
or in 450 kg/m3 snow at 9 m/s; six hours of 15 m/s across a 100 m ridge with 40 mm of new snow scour
the windward shoulder from 40 to 11 mm and load the lee below the crest from 40 to 135 mm (157 kg/m3),
the even flank between passing it on unchanged; water kept to 0.000% on the symmetric map; a frozen
crust holds everything. One update of 128 x 128 cells in that gale, transport included: 2.7 ms (CPU).
The Sx drift check now runs with transport off (it checks the falling snow alone, on an open map).

**Shot.** `--view=windblown` (`--transport=0` for the same day without): a day of 15 m/s over the ridge
leaves the windward flank bare and the lee white, raster and path traced alike.

**Not built.** Sublimation of blowing snow (PBSM's largest loss in open terrain); MicroMet's turning of
the wind around hills; drifts reshaping the ground the wind sees (snow surface instead of ground);
sastrugi and ripples (under a cell).

Sources: Liston and Sturm, A snow-transport model for complex terrain, J. Glaciology 44(148), 1998.
Liston and Elder, A meteorological distribution system for high-resolution terrestrial modeling
(MicroMet), J. Hydrometeorology 7, 2006; A distributed snow-evolution modeling system (SnowModel), J.
Hydrometeorology 7, 2006. Liston, Haehnel, Sturm, Hiemstra, Berezovskaya and Tabler, Simulating complex
snow distributions in windy environments using SnowTran-3D, J. Glaciology 53(181), 2007. Pomeroy and
Gray, Saltation of snow, Water Resources Research 26(7), 1990. Pomeroy and Male, Steady-state suspension
of snow, J. Hydrology 136, 1992. Li and Pomeroy, Estimates of threshold wind speeds for snow transport
using meteorological data, J. Applied Meteorology 36, 1997. Tabler, Snow transport as a function of wind
speed and height, Cold Regions Engineering, ASCE 1991.

## Follow-up 5: exact sparkle under the path tracer (built 10 Nov 2026)

Stage 5's sparkle is a footprint grid: one facet per cell, sized to the pixel, the same in raster and
path tracing. A path tracer can do better: it already shoots many rays through a pixel, so if every grain
is a facet the samples integrate the grains in the footprint themselves, at any distance and on every
bounce. The glint papers (Jakob, Hasan, Yan, Lawrence, Ramamoorthi and Marschner 2014; Yan, Hasan, Jakob,
Lawrence, Marschner and Ramamoorthi 2014; Chermain, Sauvage, Dischler and Dachsbacher 2020) count facets
in a footprint to get that integral cheaply; their reference is the brute-force one, every facet traced,
which is what this is. Atanasov and Koylazov (2016, V-Ray's glints) do the same per-ray facet sampling
in production.

- **The facet.** Each 2 mm grain cell (hashed from the world position) holds one facet, its normal
  GGX-sampled with alpha 0.4 around the snow's normal, as stage 5's facets.
- **Specular on its own normal.** The path tracer's material result gets a `specular_normal`: when set,
  the base specular lobe (evaluation, sampling and its pdf) lies on it while the diffuse keeps the
  shading normal. Snow sets it to the facet, at roughness 0.22 (alpha 0.05, a smooth ice face). An
  earlier try carried the facet on the coat lobe, as stage 5 does: snow came out 7% darker (mean
  luminance 0.576 against 0.623), because the coat dims the diffuse under it.
- **For custom shaders.** A new spatial fragment output, `SPECULAR_NORMAL` (view space, like `NORMAL`),
  sets it; raster ignores it. `SnowSurface3D` uses it, and `snow_glint_apply_specular()` in
  `get_snow_shader_code()` gives other snow shaders the same (the terrain material).
- **Wet and trodden snow** fade to the plain rough lobe exactly as stage 5's glints do.
- **Toggle.** The global shader parameter `snow_exact_glints` (rt_param 57). It shipped off at first and has
  been on by default since 16 Nov 2026 (every feature on; tuning comes later). At the samples a CPU check can
  afford the result is grainy (8% of pixels glint at 16 samples against 0.1% for the grid), which the
  denoiser is there to settle. Off, the path tracer keeps stage 5's grid.

**Checks.** `--view=sparkle --spp=16 --glints=exact` (and `--view=trail ...` for `SnowSurface3D`): mean
luminance 0.634 against 0.624 for the grid and 0.619 in raster (with the samples spread over the
pixel, as main now does; 0.604 against 0.623 before); the deep snow of the trail view 1% brighter than
with the grid; a glitter path toward the sun in both. All snow and ground checks pass.

**Cost.** One hash and a GGX sample per snow hit, the same BRDF work; GPU time and the look with Ray
Reconstruction on the PC queue before it can be on by default.

Sources: Jakob, Hasan, Yan, Lawrence, Ramamoorthi and Marschner, Discrete stochastic microfacet models,
SIGGRAPH 2014. Yan, Hasan, Jakob, Lawrence, Marschner and Ramamoorthi, Rendering glints on
high-resolution normal-mapped specular surfaces, SIGGRAPH 2014. Chermain, Sauvage, Dischler and
Dachsbacher, Procedural physically based BRDF for real-time rendering of glints, Pacific Graphics 2020.
Atanasov and Koylazov, A practical stochastic algorithm for rendering mirror-like flakes, SIGGRAPH 2016
Talks.

## Follow-up 6: cornices that fall, and icicles (built 11 Nov 2026)

### Cornice failure

Field studies on Svalbard measured what a cornice does before it falls. Vogel, Eckerstorfer and
Christiansen (2012) found that a tension crack opens behind the cornice, where the shallow snow of the
plateau meets the thick cornice. The slab tilts out about a fixed pivot and the crack opens almost
linearly, at 0.4 to 2.8 cm a day. It opened at 8.6 cm a day in the days before a complete failure.
Failure came four to five weeks after a crack opened, most often in the June thaw, with rain and warm
storms as triggers. Hancock, Eckerstorfer, Prokop and Hendrikx (2020) scanned cornices with lidar and
found most falls coincided with winter storms that loaded them; one fell after rain.

- **The load.** Treat the overhang as a cantilever that thins from its root to the lip. At the root it
  is as thick as it is long, which is stage 4's profile. Its bending stress at the root is `rho g L`:
  450 kg/m3 wind-packed snow, plus the weight of its liquid water. The tension crack cuts into the
  section. A crack open to a tenth of the length (a tilt of about 6 degrees) takes 60% of it, so the
  stress rises as `1 / (1 - 0.6 s)^2`.
- **The strength.** Sommerfeld (1971) gives the most snow holds in tension:
  `log10 sigma = -4.3 + 3 log10 rho`, with sigma in 100 Pa, which is 457 kPa at 450 kg/m3. A slab
  metres across fails at its weakest flaw. McClung's (1979) large samples hold much less, so we take a
  tenth. Melt water weakens the bonds, by `1 / (1 + 20 theta)`; that factor is our pick. A cold,
  uncracked cornice therefore falls at 10.4 m, near the longest field cornices (stage 4 caps growth at
  12 m).
- **The creep.** The crack opens at `L sigma / eta`. Here eta is the snow viscosity of Crocus
  (Vionnet et al. 2012):
  `eta0 rho / 250 exp(0.1 (0 C - T) + 0.023 rho) / (1 + 60 theta)`.
  For a 4 m cornice that gives 0.86 cm a day at -5 C, 1.42 at 0 C and 4.9 wet, the range measured at
  Gruvefjellet. With the crack 30 cm open, wet, it reaches 16 cm a day.
- **Filling.** Snow the wind lays on the cornice fills its crack as fast as the slab grows. That is our
  pick: cracks open in calm, mild spells, as observed.
- **The fall.** When the stress passes the strength, the slab beyond the crack falls: 0.9 L^2 of
  profile per metre of crest. It lands as 500 kg/m3 debris on the first cell downwind where the ground
  flattens under 30 degrees. The debris goes into both the world tier and the near square.
  `SnowCover3D` emits `cornice_failed(position, direction, length, mass)`. Near the camera,
  `SnowCornice3D` breaks the slab into 3 to 16 rigid chunks that tumble down the face: rounded,
  irregular balls a third of the cornice's length across, with convex colliders. They are removed after
  `debris_lifetime`. The drawn cornice shows its crack: the roof opens and the slab tilts out about a
  pivot low in its root.

**Checks** (`--test=failure`):
- The crack rates above.
- A cold cornice falls at 10.4 m.
- A baked ridge at 1 April, left in calm at -10 C, has its longest cornice (5.7 m) open its crack 7.4 cm
  in a week and fall on day 20. In a calm thaw at +1.5 C it falls on day 5.
- The debris lands 144 m out below the face.
- With failure off, none fall.

**Shots.** `--view=cornice --crack=4` shows the crack across a cornice's roof after four days of thaw.
`--view=cornice --fall` shows the first cornice to fall in that thaw (day 5), seen from the lee two
thirds of a second later, its chunks tumbling down the face below the bare crest.

Over a baked winter, cornices now fall and regrow. On the cold alpine ridge the longest at 1 April is
5.7 m (it was 4.2 m without failure, since cracks filled by accretion let others grow longer before
they fall). On the mild 16 m ridge they are 3.1 m on 1 March and gone by 1 April after its March
thaws, so a showcase wanting cornices there should end its bake by 1 March.

### Icicles

`SnowIcicles3D` grows icicles along an edge: a line of points, such as an eave or a ledge. They grow
where melt water runs off the snow above into air below freezing.

- **The water.** It comes from `SnowCover3D.get_runoff_at()` at the edge, times `catchment_length`. A
  heated roof adds `heat_loss` (W/m2) melting the snow from below, which is how icicles and ice dams
  form on houses. Water gathers into runnels, so each place along the edge gets `3 r^2` of the mean.
  Some places grow long icicles; many stay short.
- **Growth.** This follows Makkonen (1988), with the two stages of Szilder and Lozowski (1994). Heat
  leaves the wet ice at 0 C by:
  - convection, taking the larger of forced and free: Ranz-Marshall and Churchill for the tip's drop,
    Churchill-Bernstein and Churchill-Chu for the wall;
  - evaporation into the air's humidity, by the Lewis relation;
  - long-wave radiation at `4 sigma T^3`.

  While water reaches the tip, the tip lengthens as a tube of spongy ice holding 26% liquid. The tube is
  as wide as the 5 mm pendant drop: `dL/dt = 2 q / (rho_i Lf 0.74)`, 1.05 cm/h at -10 C in still air
  and 4.3 cm/h at 2 m/s. The wall thickens as its wetted half freezes the running film. That share,
  0.25, is our pick (0.5 until 20 Nov). It puts length growth at 16 times diameter growth, the middle of the 8 to 32
  Maeno, Makkonen, Nishimura, Kosugi and Takahashi (1994) measured (0.5 gave the low end, 8.2, and stubby 1:4 icicles). A balance over the whole wall gives
  4. Water the wall and tip do not freeze drips. Once the wall freezes all the water, the icicle only
  thickens. Above 0 C the same coefficients melt it back. An icicle thinned to the drop's size at its
  root falls.
- **Look.** Each icicle is a cone with ten sides, from its root radius down to the drop (Szilder and
  Lozowski: close to conical). The material is clear ice: transmission, IOR 1.31, specular 0.335 for
  1.8% reflectance, no absorption over a few centimetres. Centimetre ripples along it come from a
  normal map (Ueno 2003; Chen and Morris 2013: about 1 cm whatever the icicle).
- **Look, revised (11 Nov).** The first shots read as dark, opaque cones. Three causes, three fixes:
  - Ice that holds impurities (roof water does) is hazy, not from air but from pockets of impure
    water caught in layers as the icicle thickens (Ladan and Morris 2022). So each icicle has a
    hazy core: a cone of 0.4 of its radius over 0.8 of its length (the tip, the newest ice, stays
    clear), white, rough 0.5 and back lit so light comes through it (`hazy_core`, `core_material`).
    It is opaque so the clear ice around it refracts it in raster as under path tracing.
  - The ripple slope drops from 0.12 to 0.06 (a ripple about 0.1 mm high on its 1 cm). At 0.12 the
    path tracer bent alternate ripples onto the sky and onto the eave, black and white stripes.
  - Even at 0.06 every cone still showed regular stripes under path tracing. Painting the eave red
    turned the stripes red: they are the eave refracted, cut into bands by the ripples, because a slope
    of only 0.01 swings a ray through the cone between the eave and the sky. Without the ripple map
    the eave shows as one smooth patch. Ripples grow with the icicle (Chen and Morris 2013), so they now
    do here: none below 10 cm, full slope at 50 cm, smooth over the last 4 cm to the tip, and a random
    phase per icicle so neighbours' bands do not line up. The strength rides in UV.x across a 64 by 32
    ripple map, wide so that mipmaps blend only close strengths.
  - A solid cone bends what is behind it upward: under an eave the path tracer shows the wood's
    shaded underside through the ice, which needs a fourth bounce for the sky lighting it. At the
    default 3 bounces it was black. Fixed (11 Nov) with a transmission budget apart from the bounce
    count, as Cycles keeps one: after the first bounce, crossing a transmissive surface or reflecting
    inside it is free, up to `rendering/pathtracing/transmission_bounces` (default 4, rt_param 58)
    extra segments. At 3 bounces plus 4 the icicle view matches 6 plain bounces within noise, in
    about 70% of the time (79 s against 113 s on the software renderer). Raster refracts at the front face only, so it shows the sky
    where the path tracer shows the eave; the ice reads as clear in both.
  - The bench view is now backlit, at the icicles' height, against a snowy land (`--close` for 100%).

**Checks** (`--test=icicles`):
- The rates above. Colder and windier grow faster; nothing grows above 0 C.
- An edge with three sunny days at -4 C (melt for six hours, then a night at -8 C) grows icicles up
  to 22 cm, 1.1 kg of ice from 32 kg of water. The rest dripped.
- A trickle the wall can freeze thickens them without lengthening them.
- Twelve hours at +4 C melt the longest back from 22 to 13 cm.

`--view=icicles` shows a shed's eave after the same three days, in raster and path traced.

**Not built:**
- Supercooled water and the slower length growth Maeno et al. found with more water.
- Sunlight melting icicles below 0 C.
- Icicles falling as rigid bodies.
- Icicles along cornice lips and rock.
- A cornice's own mass taken from the snow it is built from (stage 4 adds it).

Sources:
- Vogel, Eckerstorfer and Christiansen, Cornice dynamics and meteorological control at Gruvefjellet,
  Central Svalbard, The Cryosphere 6, 2012.
- Hancock, Eckerstorfer, Prokop and Hendrikx, Quantifying seasonal cornice dynamics using a
  terrestrial laser scanner in Svalbard, Norway, NHESS 20, 2020.
- Sommerfeld, The relationship between density and tensile strength in snow, J. Glaciology 10(60),
  1971.
- McClung, In-situ estimates of the tensile strength of snow utilizing large sample sizes,
  J. Glaciology 22(87), 1979.
- Vionnet et al., The detailed snowpack scheme Crocus and its implementation in SURFEX v7.2, GMD 5,
  2012.
- Makkonen, A model of icicle growth, J. Glaciology 34(116), 1988.
- Szilder and Lozowski, An analytical model of icicle growth, Annals of Glaciology 19, 1994.
- Maeno, Makkonen, Nishimura, Kosugi and Takahashi, Growth rates of icicles, J. Glaciology 40(135),
  1994.
- Ueno, Morphological instability of the solid-liquid interface in crystal growth under supercooled
  liquid film flow and natural convection airflow, Physics of Fluids 15, 2003.
- Chen and Morris, On the origin and evolution of icicle ripples, New Journal of Physics 15, 2013.
- Ladan and Morris, Pattern of inclusions inside rippled icicles, Physical Review E 106, 054211, 2022.

## Follow-up 7: one climate with the terrain (built 11 Nov 2026)

The winter bake had its own recipe, apart from the ClimateRecipe the terrain generator's rivers, the data
map's climate fields and the plants use. Now `SnowClimate.recipe` takes that ClimateRecipe and
`SnowCover3D.climate_map` a TerrainDataMap3D with its climate baked.

- From the recipe: latitude, reference height, January and July means, the day-to-day spread (the
  generator's residual), and the wet-day wind (the recipe's wind is the one its precipitation follows; dry
  days blow `storm_wind_factor` softer). Each month's wet-day amount is the recipe's year of precipitation
  times the month's share (its seasonality harmonic) over the Markov chain's share of wet days, so the
  WGEN chain (Richardson 1981) keeps its persistence and the totals match the recipe. The bake's lapse
  rate follows the recipe's winter and summer values (Rolland 2003) day by day. The diurnal range and the
  wet/dry chain stay the SnowClimate's own: the recipe has no daily statistics.
- From the climate map: its precipitation field (Smith and Barstad 2004, summed over the wet-day wind
  rose) over the recipe's flat-ground precipitation scales what falls on each cell, in place of
  `precipitation_gradient`, in the bake, the world tier and the near square (and
  `get_snowfall_rate_at()`).
- On the bench mountain (700 m, 9 m/s westerly, the recipe's 1000 s conversion and fallout times) the
  field peaks 500 m in the lee of the summit: the advection length (about 9 km) is longer than the hill,
  so it spills over, as linear theory says small hills do. The snow follows: windward over lee snow
  water at 350-550 m goes from 1.00 to 0.46. Bigger ranges get their windward maximum back.

**Checks** (`--test=climate`): ten years from the recipe give its January mean (1.6 C against 2.0), its
year (1185 mm against 1200) and its seasonality (December 125 mm, June 58 mm); the lapse rates; the
mountain gets more than flat ground; an east wind mirrors the field; the bake's snow follows it. The
landscape bench's gap "SnowClimate is set apart" is now a pass. Its check "cornices grow on the lee
crests around the peak" failed before this change too (0 pieces since the gentler valley layout).

## Follow-up 8: clean print edges at fine deform settings (11 Nov 2026)

At 768 texels over 24 m (3.1 cm) prints showed spikes and toothed walls. Three causes, three fixes:
- Rim snow from one step landed in the hole of the step before, and thin walls of untrodden snow were left
  between overlapping steps with rim heaped on both sides; at 6 cm texels the coarser field smeared them,
  at 3 cm they stood as spikes. The pushed snow is loose, so it now slides at its repose angle (45 degrees,
  Abe 2004) with the granular ground's Sumner erosion step (`GroundDeformField.slump()`), rim only: the
  compacted walls stand, crust plates stay as heaped.
- Footprints were stamped by texel centre in or out, a staircase along a turned boot. They are now drawn
  antialiased from the signed distance to the edge over half a texel (half, so a boot two texels wide
  still sinks to full depth in the middle).
- Triangles coarser than the texels (the 6.25 cm close mesh on a 3.1 cm field) sampled walls on and off.
  The vertex shader now averages four taps over its spacing, the displacement prefiltered to the mesh's
  rate as geometry clipmaps do; the fragment's normal keeps the texels' detail.

Cost: the step with four rolling bodies goes from about 1.0 to 1.6 ms of CPU (slumping owed tiles for a
second after each press). All snow and ground checks pass.

## Follow-up 9: raster refraction out through the far side (11 Nov 2026)

Raster transmission refracted once at the front face and carried the ray on to whatever was behind, as
for water over a pool's floor. A solid needs the second surface. The established real-time answers:
Wyman's image-space refraction (2005, the back faces' depth and normals in their own pass) and Filament's
"solid" refraction (the object as a sphere of a given thickness: exit point thickness * -dot(N, T) along
the refracted ray, exit normal normalize(dot(N, T) T - N/2), refract back out). The second needs no extra
pass, so `BaseMaterial3D.transmission_thickness` (glTF KHR_materials_volume's thicknessFactor, now read
and written by the glTF importer and exporter) uses it; the exit ray then travels to the depth behind,
and absorption runs over the path inside only. 0 keeps the old behaviour. Icicles use 4 cm.

On the bench's eave the change is small: the icicles bend the view a little more and read slightly
clearer. The path tracer's view of the eave's underside through the ice comes from light reflected
inside the cone, which a screen-space method cannot follow, so that difference stays.

## Follow-up 10: the bow wave (11 Nov 2026)

A body slow enough compacts what it meets; a fast one cannot and shoves it ahead, the ploughing seen in
front of tyres, sled runners and blades in deep snow (the CRREL cold-regions mobility models, Richmond,
Shoop and Blaisdell 1995, count it as a separate ploughing resistance). In `GroundDeformField.press()`,
with the body's velocity (from its motion between frames for bodies in the deform groups):
- the rim's weights lean forward by 1 + 3 cos(angle to the heading) and reach one ring width further
  ahead, both scaled by speed / 4 m/s (full above);
- rim snow the body meets again is loose: at full speed all of it is pushed on, not 30%, so the wave
  rides ahead, grows, spills to the sides as berms and is left as a mound where the body stops.
Mass is kept (only where the pushed share lands changes). The constants are chosen for the look, not
measured. Checks: a flat foot at 5 m/s leaves 0.176 m of rim ahead against 0.033 m behind, 30% of the
dent; driven on 1.5 m, the highest rim in front is 0.194 m against 0.101 m beside the track. Bench:
`--view=trail --sled [--close]`.

## Follow-up 11: the near square feeds the world tier (11 Nov 2026)

The world tier seeded the near square, but nothing went back: drifts, slides, sheltered and sunny faces
worked out on the fine cells were lost once the square moved on and the cells came back from the coarse
pack. Nested weather models solve the same thing with two-way nesting: the fine grid's state, averaged
over each coarse cell it covers, replaces the parent's there (WRF's feedback). `SnowCover3D` now does
that before each world update and before the square shifts: water and liquid as area means, density
as the column's mass over its depth, albedo weighted by water; crusts and cornices stay the world's.
Only world cells the square covers whole are fed. Check (`--test=climate`): 30 mm of fresh snow on the
square shows in the world tier under it (373 to 384 mm, the square's 394 mm less an hour of the world's
own update), nothing changes beyond it.

## Follow-up 12: sparkle on reflected and bounced rays (11 Nov 2026)

Stage 5's footprint grid needs a footprint, and the path tracer had one only for camera rays (pixel angle
times distance), so snow seen in a mirror, a window or water showed the plain rough lobe. The path tracer
already carries ray cones from the camera through every bounce for texture levels (Akenine-Moller,
Nilsson, Andersson, Barre-Brisebois, Toth and Karras 2019, "Texture Level of Detail Strategies for
Real-Time Ray Tracing"; with the JCGT 2021 follow-up), so the grid now sizes itself by that cone on every
ray after the first. Camera rays keep the pixel footprint, as raster. Exact glints (follow-up 5) were
already per ray on every bounce and are unchanged.

Check: `--view=sparkle --mirror --spp=16`, an overhead mirror showing the snow ahead toward the sun. In
the mirror the snow went from flat (standard deviation 4.5 of 255) to speckled (4.8) at the coarse grid
level its 10 m path resolves; exact glints there give 16.2. The snow seen directly is unchanged (mean
difference 0.2 of 255, sample noise). Image: fork/snow/stage5/mirror_glints_before_after_exact.png.

## Follow-up 13: the print field on the GPU (built 11 Nov 2026)

**Where the time goes (cloud CPU, 4 cores, `--view=trail`, 4 bodies).** Each physics step of
`SnowSurface3D`: texture upload 0.7 ms (512 x 512 dent and rim turned to half floats and sent whole),
slumping 0.4 to 0.8 ms, the collision heightfield 0.4 ms a step on average (1.6 ms every fifteenth of a
second), pressing 0.07 ms. `SnowCover3D`: 5 ms per update of 256 x 256 cells, twice a second, and the world
tier 5.5 ms every eighth update, both blocking one physics step (a hitch, not a steady cost).

**How others do it.** Batman: Arkham Origins (Barre-Brisebois, GDC 2014) renders the actors from below
the snow into an orthographic heightmap, accumulates it ping-pong and blurs it, under 1 ms of GPU on the
consoles of 2013 with no readback. Rise of the Tomb Raider (Michels, SIGGRAPH 2015) does it with compute
and tessellation, independent of the snow mesh. Ghost of Yotei (Sucker Punch, 2025) renders particles and
geometry into a displacement buffer. All keep the field on the GPU and draw from it directly. Ours also
conserves mass (compaction plus a rim, Sumner, O'Brien and Hodgins 1999), slumps to the angle of repose,
breaks crusts, shoves a bow wave and gives bodies collision, so it is compute rather than a depth render,
and the CPU gets a copy back for collision and queries.

**Design.**
- The field (dent, rim, slump owed) lives in GPU storage buffers. A blit writes the RGBA half-float
  texture the material samples (wrapped as `Texture2DRD`, a stable RID): no upload.
- Pressing: the CPU still reads the bodies, their path and their Bekker sinkage, and sends a list of
  stamps. Per stamp, in order (overlapping steps of one path must see each other): a contact pass, a press
  pass that sums the pushed volume with an integer atomic, the crust crack pass, and the rim pass that
  spreads it. Same maths as `GroundDeformField::press`.
- Columns (the undisturbed surface, the deepest dent, crust) come from textures the cover already has
  (snow map: depth and density; catch map: the roof height) plus a new small crust map, and the surface's
  ground texture. No per-texel CPU calls.
- Slumping: the CPU version moves snow texel by texel in place (Gauss-Seidel). On the GPU it is two
  gather passes per step (Jacobi): each texel works out what it gives and how it splits; each texel then
  takes its neighbours' shares. The excess is the same function on both sides, so mass is kept exactly.
  Slumping owed is a per-texel timer instead of per tile.
- Relaxing (refill by new snow, wind, settling) is one pass when the cover updates.
- Moving the square: a copy pass shifts the field by whole tiles; tiles leaving go to a tile cache on the
  GPU (same size limit and least-recently-used rule as the CPU cache), tiles coming back are copied in.
- The CPU copy: a pass writes the collision heights (ground plus cover plus dent and rim, the same as
  `_surface_at`) and the field is read back asynchronously at the collision rate, so collision,
  `get_dent_at`, `get_rim_at`, `get_moved_volume` and the world-tier feed lag by a frame or two.
  `press()` returns the volume measured against that copy. `sync_deformation()` blocks for tests.
- `gpu_simulation` (default on) falls back to the CPU path where there is no RenderingDevice
  (Compatibility renderer, headless).
- Next: the same for `GroundSurface3D` (fresh channel, slow mud), then the cover and world tier cells
  (each cell's update is independent; drift and slides are stencils).

**Built.** `modules/snow/snow_deform.glsl` (one compute shader, a pass per push constant) and
`SnowDeformGPU` (the render-thread side; both moved to the ground module on 16 Nov 2026 as
`ground_deform.glsl` and `GroundDeformGPU`, shared with `GroundSurface3D`, docs/ideas/sand.md stage 0); `SnowSurface3D.gpu_simulation` (on by default),
`is_gpu_active()`, `sync_deformation()`. Pressing runs every stamp of a step in order inside one
workgroup, so the sums (volume moved, pushed, deepest, the rim weights) are exact tree reductions with no
float atomics. Slumping uses the CPU's step, half the largest excess (Jacobi with the same share is
stable here because only rim moves and only downhill). GPU memory at 512 x 512: 8 MB of field (two
buffers for the slumping passes), 2 MB scratch, 2 MB texture, 1 MB read back, 16 MB for 1024 cached tiles
(as the CPU cache). `GroundDeformField::StampSink` lets the field send its presses elsewhere;
`GroundSurface3D` is unchanged (CPU).

**Results (cloud CPU device, lavapipe).** The physics step's main-thread time: 1.52 ms on the CPU path,
0.04 ms on the GPU path in `--view=trail`, 0.19 ms in the test's four-body walk (unpacking the read-back
copy, at 15 Hz). The same walk: dent 1.3064 m3 and rim 0.1817 m3 on the GPU against 1.3037 and 0.1858 on
the CPU. With the walker set on the surface it left (`_walk` waits for the read back), the crust test
matches to the millimetre (12 mm down beside the hole, rim 0.0156 of 0.3285 m3 against 0.3286), and a boot
sinks 0.209 m on both. A path-traced `--view=trail` frame differs by 0.02 of 255 on average. All snow
checks pass on both paths (`--test` under a window for the GPU, `--headless` for the CPU; `--gpu=0` forces
the CPU). GPU time is on the PC queue.

**Known limits.** Collision and the queries lag the GPU by the read back (about three rendered frames);
the CPU path lagged by up to 1/15 s already. Outside the cover's near square the GPU sees no snow
(the CPU path reads the world tier there; the field is always well inside the square).

## Follow-up 14: cover updates in the background (built 11 Nov 2026)

**Problem.** `SnowCover3D` stepped its cells in the physics frame that started an update: 256 x 256 cells took
5 to 7 ms on the main thread twice a second (rows 2 ms on four threads, drift, slides and cornices 3 ms, the
textures' bytes), and every 8th update the world tier added 4 to 6 ms more. Each was a hitch.

**Technique.** The standard way engines take slow simulation off the frame: run the step as a job on a worker
thread against inputs fixed when it starts, keep readers on the last *published* state, and swap the new
state in when the job is done (double buffering; Andersson, "Parallel Futures of a Game Engine", Frostbite
2010; Gyrling, "Parallelizing the Naughty Dog Engine Using Fibers", GDC 2015; Unreal's async tasks for its
landscape grass maps). Readers see the pack one update late, which at 2 Hz is far below anything visible,
and nothing on the main thread waits.

**Design.**
- `_update` is split into three parts. `_update_begin` runs on the main thread: the inputs (sun, wind, Sx)
  and a snapshot of the weather map (`WeatherMap3D::get_snapshot()`, #247: its rain, cloud cover and cloud
  shadow as shared buffers with their sampling parameters, so the cells read the weather as it was when
  the update started while the map moves on). What cannot be snapshotted (the canopy closure from the
  weather's shelter, the terrain climate's precipitation) is read into per-cell arrays on the main thread
  when cells are sampled anew, and a sixteenth of the rows is refreshed each update.
  `_update_cells` runs on any thread: rows, slides, wind transport and cornices. `_update_end` runs on the
  main thread: runoff to the weather map and the counters. The in-frame path calls all three, so both paths
  run the same code on the same inputs.
- Published copies: every field a getter, `SnowSurface3D` or `SnowCornice3D` reads has a published twin
  (`pub`). The twins share their buffers (Godot's `Vector` is copy-on-write with an atomic count), so
  publishing costs nothing. The worker's first write to a field copies it on the worker, and the main thread
  keeps reading the old buffer, untouched.
- One task carries the near square and, on its turn, the world tier: the square's state feeds the world,
  then the world cells step (a cornice failing there drops debris into the square), then both textures'
  images are built. The main thread only hands the images to the textures.
- Everything that changes the cells on the main thread (`advance`, `add_snow`, `clear`, a shift of the
  square, a climate bake, a new climate) waits for the task first. The square does not follow the camera
  while a task runs: it moves when the task lands, a frame or two later.
- `background_updates` (on by default) switches it off, and `sync_update()` waits for a task in flight.

**Results** (`docs/bench/snow --test=background`: a sunny, windy, snowing mountain at 256 x 256 cells with a
64 x 64 world tier every 3rd update, run through the same physics frames both ways):
- The pack comes out bit-identical: 1723 samples over both tiers, water to the gram.
- Getters return the same values on every frame an update is in flight.
- Main-thread cost per update falls from 4.5 to 6.0 ms (worst 5.8 to 6.9 ms) to 0.3 ms (worst 0.4 ms), split
  between the frame that starts it and the frame that swaps it in. The worker takes 7 to 9 ms. (With the
  weather still gathered on the main thread, the first version, 79e42843, left 0.9 to 1.2 ms.)

**Limits.** A setter changed while an update is in flight may apply to it or to the next one. Holding a
weather snapshot makes the map copy each of its arrays once on its next update (32 KB each at 128 x 128).

## Follow-up 15: snow on open water and lake ice (built 18 Nov 2026)

Snow, rain and water read one world. Research first:
- **Rain or snow.** One split formula for the whole engine: `WeatherMap3D::rain_fraction(celsius,
  humidity)`, the humidity-shifted wet-bulb ramp the weather map already used. Snow's
  `snowfall_fraction` is now `1 - rain_fraction`, so the two can never disagree about the same air.
- **Live air.** Snow reads `get_air_temperature()` and `get_air_humidity()` (the weather's air, after
  fronts and the day cycle) instead of the recipe's base values, in the cover, the falling snow and the
  icicles.
- **Snow on open water.** The cover finds water through the `TerrainDataMap3D` at `climate_map` (its
  water surfaces and rivers: `get_water_depth_at` over 5 cm). On an open-water cell, snow and rain melt
  into the water as they land and leave as runoff; nothing lies there. Falling snow ends at the water
  surface, not the lake bed (`get_water_level_at`).
- **Lake ice: Stefan's law with insulation** (Leppäranta 1993, Ashton 1989). Ice grows from the
  freezing-degree time with the heat path through the snow on top and the air above:
  `h' = sqrt(a^2 + 2 k_i (-T) t / (rho_i L)) - k_i R0`, `a = k_i R0 + h`,
  `R0 = d_s / k_s + 1/H`, `H = 20 W/m^2K`, integrated exactly over each update. Snow conductivity from its
  density (Sturm et al. 1997: `k = 0.138 - 1.01 rho + 3.233 rho^2`, rho in g/cm^3), so new powder
  insulates far more than old pack.
- **Snow ice.** When the load of snow pushes the ice under the waterline (negative freeboard
  `((1000 - 917) h - SWE) / 1000`), the flooded layer of snow freezes into ice (Leppäranta 1983,
  Saloranta 2000).
- **Holding.** Under 2 cm the ice is treated as open water; from 2 cm it holds snow; from 5 cm the
  surface (`SnowSurface3D`) lifts the ground to the water level, so the walker stands on the lake.
- **Thaw.** Above freezing, once the snow is gone, ice melts by the degree-hour factor (`TF t / 917`).
- **World tier.** Ice is seeded from the world cells and fed back up, like the pack.
- `lake_ice_enabled` (on by default); `get_ice_thickness_at`, `get_water_level_at`.

**Results** (`docs/bench/snow --test=water`): bare-ice growth matches the closed form; 20 cm of new snow
cuts growth below a quarter; a winter on a test lake grows 32.8 cm, as the relation gives for its
-14.3 C air at the lake's height; open water takes snow with no pack; ice over 2 cm holds a pack; heavy
snow floods 32.9 cm of ice to 84.2 cm; twelve days at 12 C clear the ice.

**Next.** Draw the ice (dark clear ice to grey with thickness, snow on top) with collision beyond the
surface's near field, and keep it through the climate bake.

## Follow-up 16: lake ice drawn and walkable; white ice; fire (built 18 Nov 2026)

**Drawing.** `SnowLakeIce3D` (added by `SnowCover3D` while `lake_ice_enabled`) draws the ice of the near square
and the world tier as one smooth sheet at the water level plus its freeboard, `(1 - 917/1000) h` (floating
ice stands out of the water by 8% of its thickness). The sheet runs one cell past the ice under the shore, so
its edge is where the terrain meets it, not the cells' steps. Its look follows the field:
- Clear (congelation) ice is dark: the deep water shows through, albedo about 0.1 (Bolsenga 1969), and
  smooth (a mirror for the low sun).
- White (snow) ice from flooded snow scatters off its bubbles: albedo about 0.46, rougher. The cover now
  keeps the white share (`get_white_ice_thickness_at`); the thaw takes it first, from the top.
- Thermal cracks: a jittered Voronoi wall pattern, bent by noise, with only some walls cracked (a hash of
  the plate pair), plates about 9 m with a few splitting at 2.5 m in ice over 10 cm. Pale (frozen bubbles),
  fading by 90 m, widened with distance so they do not alias. Hashed with pcg2d, so it never repeats.
- Snow on it is the cover's, through the shared snow functions, as on any material.
- Collision: a trimesh of the cells that bear a walker (5 cm) within `collision_radius` (160 m) of the camera,
  rebuilt only when that set changes. Near the camera `SnowSurface3D` lays its deep snow on the ice top.

**Thaw.** Bare lake ice decays by degree-days at about 0.5 cm per C day (Bilello 1980; Ashton 1983), which
holds the sun and warm water too; snow's melt factor alone gave a fifth of that.

**Climate bake.** The bake keeps the ice and its white share (`SnowClimate.ice`, `white_ice`), so the world
starts the season with its lakes frozen.

**Fire.** `WorldContracts.fire().heat_at()` (W/m2 over the ground) melts the pack whatever the air: 0.334 MJ a
kilogram, 92.8 W/m2 per mm of water an hour. Read for every cell on the main thread each update, and only
while a fire is burning.

**Results** (`--test=water`, `--test=fire`): flooding leaves 51 cm of the 84 cm white; the sheet covers the
lake with 8242 walkable cells and a ray stops on its top to the centimetre; ten days at 7.7 C take 9.4 cm,
all of it white ice; a month later the lake is open and nothing is drawn. 928 W/m2 for 2 h at -8 C melts
the energy's 20 mm, the snow beside it untouched. `--view=lake` shows the lake from its shore.

**Snow contract.** `WorldContracts.snow()` (`WorldSnow`, modules/world) is what other systems read: `depth_at`
(m), `density_at` (kg/m3), `wetness_at` (0..1), `surface_at` (m, prints near the camera from `SnowSurface3D`),
`crust_at` (kPa) and `lake_ice_at` (m, white share). `SnowCover3D` produces it; tests stub it with
`WorldContracts.set_snow()`.

## Follow-up 17: sleet (built 18 Nov 2026)

Rain and snow share one split (`WeatherMap3D::rain_fraction`): rain draws its share (the rain thread's
`RainFall3D`, with pellets), `SnowFall3D` draws the snow's. Near and above 0 C the falling snow changes as
it does in the field:
- **Aggregates.** Above -5 C the crystals' quasi-liquid surface makes them stick; aggregates peak just
  below 0 C at 2 to 3 times the size of cold flakes (Hobbs, Chang and Locatelli 1974). `aggregate_factor`:
  1 at -5 C and colder, 2.5 at 0 C, smooth between. The same water falls as fewer, larger flakes.
- **Melting.** In air above 0 C the melted share of a falling flake follows the same ramp as the split
  (`melted_share`): what has gone all the way is rain, the rest is partly melted. A melting flake collapses
  toward its drop (size from the flake to the melted diameter with the share squared) and keeps the slow fall
  of snow until most of it has melted, then speeds up to the drop's (Mitra et al. 1990; drop speed
  9.65 - 10.3 exp(-0.6 D), Atlas et al. 1973). `wet_fall_speed`.
- **Look.** Melt water fills the flake: it scatters less, so it shows greyer and clearer.
- `sleet_enabled` (on by default); `get_melted_share`, `get_aggregate`.

**Results** (`--test=sleet`): x1.00 at -10 C, x2.46 at -0.5 C; melted share 0 at -1 C, 0.27 at 1 C, 0.86
at 3 C; a 2 mm flake falls at 1.00 m/s dry, 0.98 half melted, 6.55 melted. `--view=snowfall --air=C` shows
dry snow at -6 C, big aggregates at -1 C and wet sleet streaks at 1.5 C.

## Follow-up 18: snow on moving things (built 19 Nov 2026)

Before this, a moving body with a snow material showed the ground map's snow wherever it stood, so a car
driven out of a drift kept a full roof at any speed. Now each moving thing keeps its own load.

**What carries snow.** `SnowBodies3D`, added by `SnowCover3D` (`bodies_enabled`, on), tracks every
`RigidBody3D` (vehicles too), `AnimatableBody3D`, PhysX vehicle and `PhysXCloth3D`, plus the `snow_catch`
group (`snow_ignore` opts out; characters only through the group). It finds them as they enter the tree.

**The load, per body, at 10 Hz:**

- Seeded on first sight from the ground's cover (`WorldSnow` depth, density, wetness) if the sky is open.
- Falling snow (`WorldWeather.snowfall_at`) adds `swe / rho_new` (Hedstrom and Pomeroy 1998), only where an
  upward ray from the body's top finds open sky (checked once a second).
- Wind strips it above the blowing threshold (Li and Pomeroy 1997), counted as the air *past* the body
  (wind minus the body's velocity), so driving fast is wind too. The threshold rises with wetness
  (x(1 + 4 wet)) and settling (x sqrt(rho / 150)), since wet or settled snow sticks (an estimate).
- Jolts above 0.5 g shed it at 3 per second per g over (an estimate, the same idea as canopy unloading).
- Thaw melts it at the cover's degree-day factor, fire at 0.334 MJ/kg (`WorldFire.heat_at`), and melt and
  rain wet it; cold refreezes the water. It settles toward 300 kg/m3 over two days and its albedo ages.

**Drawing it.** `BaseMaterial3D.snow_on_body` declares `instance uniform vec4 snow_body : instance_index(12)`
(depth m, albedo, melt water, 1 + density / 1000; slots 0-11 are the mud's). When set, it replaces the
weather cover with Niu and Yang's (2007) `tanh(depth / (0.025 rho / 100))`, and the shared weight and
apply functions do the rest. The path tracer reads the same slot (`rt_material_snow_on_body`, bit 25 of
`subsurface_texture`). Only bodies' materials declare it, because every instance whose material has an
instance uniform takes 16 slots of the global buffer: `SnowBodies3D` turns `snow_on_body` on for the
materials of the bodies it tracks (`auto_materials`). A material that had no snow gets snow from the body
only, so untracked instances sharing it stay bare. MaterialKey's bitfields are full, so the flag rides
in the key's flags' top bit.

**Checks.** `docs/bench/snow -- --test=bodies` (10 checks): found by itself; seeded in the open, bare under
a roof; snowfall at the new snow density, none under a roof; a breeze keeps it and a 20 m/s gale strips it;
25 m/s of driving strips it; a crawl keeps it and shaking strips it; a thaw melts and wets it; fire melts
it; untracking clears it. `--view=bodies` shows three crates after six hours of snow (left out, just
driven in, from a shed), raster and path traced alike.

**Next.** Weight on the body (its mass), snow sliding off a tilted roof as a slab, and clumps falling
off as `SnowClumps3D` pieces when a load sheds.

## Follow-up 19: weight, slabs and clumps from bodies (built 19 Nov 2026)

- **Weight.** Each body's load mass is depth x density x the top area of its meshes' world bounds. Every physics step, `apply_force` presses it on a free `RigidBody3D` at the middle of its top. Check: 10 kg of snow on a weightless 10 kg body gives 4.93 m/s after 0.5 s, against 4.93 expected.
- **Slabs.** On a tilted top (the body's up axis against the world's), the load slides as one slab under Mohr-Coulomb. The shear rho g d sin(theta) has to exceed the bond c (250 Pa dry, times (1 - wet)^2) plus friction rho g d cos(theta) tan(phi), where phi falls from 25 to 15 degrees with wetness. The same form is used for the cornice crack. With 15 cm at 150 kg/m3: dry snow holds on a 40 degree roof, wet snow slides at 40 degrees but not at 20, and 40 cm of dry snow slides at 80 degrees. Cloth is excluded, since it has no single top.
- **Clumps.** Snow that falls off (shaken, blown or slid) lands on the snow through `SnowClumps3D.drop(at, radius, volume, wet)`. The amount is limited by how well the snow holds together (`SnowClumps3D.cohesion`), and the clump sizes are log-normal, about 4 cm dry and 8 cm wet (debris granules, Bartelt and McArdell 2009). Slabs land past the low edge, and shaken snow lands in a ring round the body. Check: a crate tipped 75 degrees in deep snow sheds its 30 cm as 64 clumps.
- **Cloth.** SnowBodies3D writes `snow_load` (kg/m2) to a `PhysXCloth3D` once the cloth has that property; the change is asked of the PhysX thread. The load adds to the solver's areal density, next to the water term, so loaded fabric sags.

## Follow-up 20: avalanches (built 19 Nov 2026)

`SnowAvalanche3D`, added by `SnowCover3D` (`avalanches_enabled`, on).

**Slab.** The slab is the new snow since it last bonded. It is tracked per cell from the cover's new snow, has a mass-weighted age and bonds after a week. The other slab is the snow over a buried crust, with facets on top of the crust as the weak layer.

**Release.** A slab releases on 28-55 degree slopes when the stability index S = tau_strength / (rho g h sin cos) drops below 1 (Foehn 1987). The strength is 14.5 (rho / 917)^1.73 kPa (Jamieson and Johnston 2001) with a weak-layer density of 100 kg/m3, or 150 on a crust. Sintering raises it by (1 + age / 48 h), about double in two days. The fracture spreads to neighbours with S < 1.2. Check: at 38 degrees, 50 cm holds (S 1.32), a metre fails (0.66), and the same metre two days old holds (1.32).

**Flow.** The flow is depth-averaged Voellmy on the cover's grid (Voellmy 1955; Salm 1993; RAMMS, Christen et al. 2010), with mu 0.2 and xi 1500 m/s2.
- Gravity acts down the snow surface. The surface includes the flow itself, which supplies the pressure term.
- Mass and momentum move by donor-cell transport under CFL 0.4.
- Above 5 m/s the flow entrains new snow at 0.005 m per m/s per s.
- It deposits where it is slower than 0.5 m/s and the slope cannot beat mu.
- The moving mass lives in the cover's cells and is written back at 5 Hz, so every reader sees it move.
- At the end, two mass-conserving passes smooth away the grid's checkerboard. Debris breaks into clumps near the camera, only in cells the flow crossed faster than 5 m/s, so the scoured release zone is left clean.
- Free rigid bodies in the flow take 1/2 rho u^2 over their width times the flow depth.

**Checks.** `--test=avalanche`, 6 checks, on a 38 degree, 80 m face over a flat run-out:
- 30 cm of new snow stays.
- One more metre releases: 20800 m3, with 1127 m3 entrained.
- The flow runs 143 m at up to 15 m/s.
- Water is kept to 0.2%; the small loss is snow that leaves the grid.
- About 2 m of debris piles 40 m out from the toe.
- A trigger releases a slab.

**View.** `--view=avalanche [--before]`. The dark specks on the face in the raster shot are the ground showing through the 1 cm left on the scoured bed. In this bench all the snow is new, so the slab runs to the ground. That is not a rendering fault (the specks are the same with `--glints=grid`).

**Limits and next.**
- Built in follow-up 22: the higher-order scheme (HLL, as RAMMS uses). Speed is set by the flow's depth (see there).
- Built in follow-up 23: the powder cloud, in PhysX Flow as a heavy gas fed by the flow's front.
- Built in follow-up 21: vehicles, boots and cornice failures add stress (Foehn's skier term).

## Next round (proposed 19 Nov 2026): heavier snow on bodies, rain on ruts, avalanches

**Heavier snow on bodies.**

- *Weight:* SnowBodies3D adds the load's mass to the body. That mass is depth × density × the top area facing the sky, where the area is the body's AABB top, or a downward ray grid for a better estimate. The load is applied as a constant force at the top centre, so a loaded trailer handles differently and a laden branch-like prop dips.
- *Cloth sag:* the same load goes on PhysXCloth3D as extra mass on the particles facing up. PhysX cloth takes per-particle inverse mass.
- *Slab sliding:* on a tilted face the load slides as a slab once the shear (rho g d sin θ) passes the bond strength. Wet snow on warm metal bonds weakly. Start from the cornice failure criterion we already have (follow-up 7), which uses the same Mohr-Coulomb form.
- *Clumps:* shed snow (wind, jolts, slides) leaves as SnowClumps3D pieces, the ground thread's parked clumps (4dd45be3), instead of just disappearing. A shake's mass balance goes to clumps, and wind's goes to the blowing-snow flux.

**Rain on ruts.** RainFall3D reads WorldContracts::snow()->surface_at, so drops stop in prints and ruts. This touches a weather file and needs Paulo's line.

**Avalanches.** Slab release where the load on a 30-45° slope exceeds the weak layer. We already have the crust and burial state from follow-up 4, which can serve as the weak layer. The run-out uses a depth-averaged Savage-Hutter or Voellmy flow on the cover grid (RAMMS uses Voellmy with mu around 0.155 and xi 2000 m/s²). The flow deposits snow, buries plants and bodies through the snow contract, and throws powder into the fog.

## Follow-up 21: triggers from people, sleds, vehicles and cornices (built 19 Nov 2026)

Paulo picked all three of round 3 (19 Nov), triggers first. `SnowAvalanche3D.triggers_enabled`, on.

**Load.** Every body in `trigger_groups` (the groups SnowSurface3D prints: `snow_deform`, `grass_trample`, `world_contact`) that stands on at least `min_slab` of snow loads the slab under it, every physics frame.
- The load is a line load P: the body's weight over its footprint, a metre at least. An 80 kg skier gives Foehn's 0.78 kN/m.
- Mass comes from rigid bodies, `character_mass` for characters, and the `snow_mass` / `ground_mass` meta. Vehicle wheels carry their share of the vehicle.
- What the body has sunk in, read from `WorldSnow.surface_at` (its print), brings the load nearer the weak layer. A boot in powder triggers more easily than a ski.

**Index.** The added shear is Flamant's line load on an elastic half-space, as Foehn (1987) applies it to a slope: 2 P sin(t + a) sin^2(t) cos(t) / (pi D), peaked over t. On the flat it is 0.207 P / D. The trigger index is strength / (rho g h sin cos + delta_tau), for the weakest layer (new snow or a buried crust's facets). Under 1, the slab releases.

**Spread.**
- From the load, the fracture runs through new slab on avalanche terrain while that slab's natural index stays under `propagation_index` (1.5), or under 1.2 times the index where it opened. A uniform storm slab goes as a whole.
- Along a buried crust the collapse (a whumpf) runs anywhere the crust does, flat ground included, up to `collapse_range` (60 m). It releases the steep cells it reaches whose index is under 1.5: a remote trigger (Johnson et al. 2004; the propagation saw test, Gauthier and Jamieson 2008).
- A collapsed crust does not collapse again.

**Cornices.** A failed cornice (`SnowCover3D.cornice_failed`) lands on the slope below it at twice its weight along its length.

**Recent snow.** The cover now hands every update's new snow and hours to the avalanche as the update ends, and `add_snow` does too. Before, snow added between two checks, or the hours of a multi-step `advance`, could be missed.

**Background updates.** While an avalanche runs and a background cover update has the cells, the flow waits and catches up afterwards (at most 0.25 s), so the frame never waits for the update. The background-update bench switches avalanches off, since a fresh load there can avalanche and the two runs it compares would then diverge.

**Checks.** `--test=triggers`, 5 checks, on the 38 degree face:
- Flamant's 0.207 P / D.
- 60 cm of fresh snow: index 1.10 alone, 0.45 under a person. A person standing on it releases 9600 m3.
- A metre of snow five cold days old (now 0.49 m at 202 kg/m3): index 1.13 under a person, who stands on it, and 0.75 under a 400 kg sled, which releases it.
- A week-old slab, 0.57 m over a buried crust: on the flat, index 1.72 under a person (holds) and 0.45 under 3 kN/m. That load collapses the crust and the face 20 m away releases 1650 m3.
- Snow from the weather map builds the slab update by update: 0.164 m fell in 6 h and all of it counts as new slab (re-checked after the world-links fix 499de57a, which makes the weather map answer snowfall_at for SnowBodies3D too; the body checks still pass and the crates get the ground's 0.147 m in the bodies view).
- With `triggers_enabled` off, nothing triggers.

**Limits.**
- Loads are static. A jump landing or a fall (about twice as much, Schweizer and Camponovo 2001) would need the body's vertical speed.
- Wheels load one at a time, not summed across the vehicle.

## Follow-up 22: second-order flow transport (built 19 Nov 2026)

The flow's advection is now finite volumes of second order, the way RAMMS solves the same Voellmy equations (Christen et al. 2010). It replaces donor-cell transport.
- MUSCL reconstruction with the minmod limiter of the mass and the velocities at each face (velocities, not momenta, so a thin edge cannot reconstruct a wild speed).
- HLL Riemann fluxes (Harten, Lax and van Leer 1983; Toro 2001) with wave speeds u +- sqrt(g h), and the dry-bed speeds u - 2c / u + c at a front.
- Heun's two stages in time. The CFL is on the fastest wave, |u| + c, at 0.2 of a cell each way.
- Gravity and the earth pressure stay in the source step, as the gradient of the snow surface with the flow on it, so snow at rest stays at rest. Friction stays a separate step.

**Speed.** The test avalanche peaks at 15.6 m/s, which is 84% of Voellmy's steady speed sqrt(xi h (sin - mu cos)) for the flow depth and slope where it ran fastest (18.5 m/s). It was 15.4 m/s with donor cells. The flow here is only about 0.4 m deep (1.2 m of 100 kg/m3 snow becomes 300 kg/m3 flow), and Voellmy's speed goes as the square root of depth. So the 20-30 m/s of measured avalanches needs their deeper flows (1-3 m), from bigger releases and entrainment, not a different scheme. The new scheme's gain is a front that does not smear: it keeps its depth, and so its speed, where donor cells thinned it. The toe deposit is now 1.57 m at 40 m out, against 1.96 m before (the deposit spreads more evenly).

**Check.** `--test=avalanche` adds one check: the peak speed is within 20% of the steady speed for its depth (`get_max_speed_terminal()`). Water is still conserved to 0.1%.

## Follow-up 23: the powder cloud (built 19 Nov 2026)

A fast dry avalanche lifts a suspension cloud off its front. Measured clouds hold 3-10 kg/m3 of snow against air's 1.2, run ahead of the dense core, and leave a thin powder layer past the debris (Vallee de la Sionne: Sovilla et al. 2015, front eruption currents; Turnbull and McElwaine 2008; RAMMS::EXTENDED's second, suspension layer fed by the dense core, Bartelt et al. 2016). `SnowAvalanche3D.powder_enabled`, on.

**Intake.** On the flow's leading slope, the cells whose downstream neighbour carries less, a dry front faster than `powder_speed` (10 m/s) blows `powder_share` (0.3) of its snow into the air for each 10 m it runs, per m/s over that speed. "Dry" means the released slab holds no liquid water (under 0.1% of its mass), so a wet slide makes no cloud. The mass leaves the flow, so the cover loses it there.

**Cloud.**
- The intake gathers in 8 m bins and becomes parcels every half second.
- Each parcel keeps the front's velocity, slowed by the air (8 s e-folding). It spreads at 4 m/s up to 40 m across and settles out over `powder_settle` (6 s).
- What settles is laid on the cover in a cone over the parcel's disc, at 70 kg/m3 and fresh albedo, five times a second.
- Every kg that goes up comes down, and water is conserved.
- Free bodies inside a parcel take 1/2 rho_c u^2, where rho_c is the parcel's mass over its volume, capped at 10 kg/m3 (`get_powder_density_at`).

**Look.** `SnowPowderCloud3D`, a child of the avalanche, puts sphere emitters (24, 5 m, overlapping the 8 m bins) on the strongest bins, each fed at least a third of the strongest so the front rises as one wall in a PhysXFlow3D of its own. It is a heavy gas (`smoke_buoyancy` -0.8, `scattering_strength` 1 so raster matches path tracing after the Flow parity fix 8579bb21) with an updraft of 0.3 times the front's speed (the eruption current), resting on the cover's height source as Flow's terrain floor and drifting with the weather's wind. It reaches the PhysX module by class and property names only, so the snow module builds and runs without it, with the same physics and no cloud drawn.

**Checks.** `--test=powder`, 5 checks:
- The test avalanche lofts 14.5 t, at most 11.4 t in the air at once, and all of it settles within 40 s.
- The cloud settles snow 167 m from the release, past the dense flow's 143 m.
- A thin powder layer, about a millimetre, lies at 90 m.
- Water is conserved (7864 t to 7854 t; the dense flow's edge losses).
- After a thaw (wetness 0.1), the same avalanche makes no cloud.

**View.** `--view=avalanche --powder [--at=S]`. The avalanche runs in real time so Flow steps the cloud with it. The raster shot 9 s after release shows one continuous billowing wall over the run-out, casting its shadow on the snow. A first tune, with 12 emitters fed by rate, drew three separate puffs and failed the docs' parity check against PT. For path tracing, the bench steps under raster and freezes the scene for the traced still.

**Limits.**
- This avalanche peaks at 15.6 m/s, so the cloud is modest: the intake grows with speed above 10 m/s.
- Trees in the blast zone are not broken yet. The trees thread's thrown-tree path could take the parcel pressure (5-10 kPa breaks trunks).
- Flow needs its SDK built (`flow_sdk=`), which also works in the cloud.

## Follow-up 24: forests hold, brake and take pressure from avalanches (built 19 Nov 2026)

Paulo's round-4 pick, forests first. `SnowAvalanche3D.forests_enabled`, on by default. Canopy closure is the cover's `cell_canopy` (the weather map's canopy, refreshed a sixteenth of the rows each update).

- **Anchoring** (Teich et al. 2012; Bebi et al. 2009): the weak layer's strength is multiplied by 1 + 4a/(1 - 0.9a), with a = clamp((closure - 0.2)/0.3). Nothing changes under 0.2. It is 5 times stronger at 0.5, and from 0.5 nothing releases. This applies to natural release (the seed cell and the fracture's spread), to the skier index and to the fracture a load opens. A bug found here: the natural seed was picked before anchoring, so a forest cell could still start a release.
- **Braking** (Feistl et al. 2014): under canopy the flow meets extra dry friction 0.2 x closure, and the stems detrain snow at K x closure / u kg/m2/s with K = 100 Pa. The detrained snow is laid where the stems took it. Slow flows lose most and stop among the trees, fast big ones run through.
- **Pressure**: the flow records 0.5 rho u^2 per cell (`get_pressure_at` now, dense flow then powder; `get_peak_pressure_at` for the run). Trunks of 30-50 cm break from about 10-20 kPa (Bartelt and Stoeckli 2001), and the powder cloud's 1-5 kPa takes crowns. Breaking the trees belongs to the trees thread. It needs a WorldSnow contract getter for the pressure, requested from world-links.
- Checks (`--test=forest`): the storm that releases the bare face leaves a closed forest (closure 0.86) in place, index 5e5 even under a person. A forest strip on the run-out stops the debris (10 cm and more) at 26 m against 40 m in the open, with 2,668 m3 detrained. The toe takes 20 kPa.

## Follow-up 25: wet spring avalanches and glide (built 19 Nov 2026)

Round 4, second part. `SnowAvalanche3D.wet_enabled` and `glide_enabled`, both on by default.

- **Water weakens the pack.** This is the LWC index of Mitterer et al. (2011): the mean liquid water content over 3% by volume, which is roughly what layers hold, with wet activity from about 0.33. Here the index is the cell's liquid over what the cover holds (`liquid_capacity`). The weak layer's strength is multiplied by `wet_weakening`^index, with a default of 0.25: about 0.63 at a third and a quarter when saturated (shear strength falls steeply with water, Yamanoi and Endo 2002). It applies to natural release, to the skier index and to the spread of a fracture.
  - A first try on volumetric content barely moved light new snow (100 kg/m3 holds little water by volume), so the index is on the share held.
- **Wet flow.** The slab's liquid share over capacity sets the flow's wetness, and the Voellmy parameters lerp from dry to wet: mu 0.2 to 0.4, xi 1500 to 1000, density 300 to 450 kg/m3 (wet snow avalanches, Naaim et al. 2013). A yield stress `wet_cohesion` of 300 Pa at full wetness sits on the base as C/(rho h), and the stop test includes it. Thin margins stop first and stand as levees while the core runs on. Bartelt et al. (2015) model cohesion in RAMMS and find it shortens run-out and narrows the flow. Their N0 near 1000 Pa enters the activation energy, not as an added stress, so 300 Pa here is the additive equivalent. Wet flows make no powder cloud, as before.
- **Glide.** Water at the base is the condition: the pack drains (runoff) with its liquid over half of what it holds, on slopes over 15 degrees (none under, McClung and Clarke 1987), outside canopy over 0.5. The rate is `glide_rate` (0.5 m/day) x wetness x the slope term (sin t - sin 15)/(sin 35 - sin 15) x min(depth, 2 m). Observed glide runs from mm to tens of cm a day, accelerating before release (Jones 2004; Ancey and Bain 2015). A crack opens at 5 cm (`glide_crack_opened`, `get_glide_at`). At `glide_release` (1 m) the cell and the neighbours past half of it release full depth.
  - Drawing the crack (a dark gap in the surface at the top of the gliding pack) is a later visual step.
- Checks (`--test=wet`):
  - 60 cm of new snow holds in frost (index 1.10) and releases on its own in a 6 h thaw, the flow 13% wet.
  - Triggered dry, the same slab runs 15.3 m/s and 97 m and piles 1.29 m. Wet (38%), it runs 10.0 m/s and 67 m and piles 2.29 m.
  - Rain on a bonded 1 m pack opens glide cracks after 10 h and releases full depth after 58 h (13,000 m3, 95% wet). There is no glide on the flat.

## Follow-up 26: avalanches into water (built 19 Nov 2026)

Round 4, third part. `SnowAvalanche3D.water_enabled`, on by default. Lakes are the cover's water (`cell_water`, from the terrain data map), whose ground the cover lifts to the surface.

- **Ice or water.** A flow cell over a lake checks the ice there. Ice of 2 cm and more (`ICE_HOLD`) carries the flow unless the cell's flowing mass is more than Gold's breaking load (`ice_breaking_load`, P = A h^2). If it is, the cell is cut out of the ice (`IceBreak`, the same path a heavy body takes) and the drawn ice is rebuilt.
- **Into the water.** Over open water (or broken ice) the snow leaves the flow and stays in the cover's cell. The cover melts snow on open water into it as runoff (follow-up 15), so no mass is made or lost. Each cell adds to the entry's mass, momentum, thickness, impact-zone depth and the slope of the land beside it. The depth is the deepest of the next four cells ahead, since the slide plunges past the shoreline where the depth is nothing.
- **The wave.** It uses Heller and Hager (2010): the impulse product P = F S^1/2 M^1/4 cos(6/7 alpha)^1/2 alone predicts a granular slide's wave. The near-field height is H_M = 5/9 P^4/5 h. It spreads in a basin as H = 3/2 P^4/5 cos^2(2 gamma/3) (r/h)^-2/3 h (Heller et al. 2009, after Huber and Hager 1997), never more than 0.8 h (breaking). The leading period is T = 5.5 M^0.05 (r/h)^0.36 sqrt(h/g) (Heller and Spinneken 2015).
  - One wave for each entry, which ends after 3 s with no snow going in.
  - `avalanche_entered_water(position, direction, mass, speed, wave_height)`, `get_wave_height_at`, `get_wave_period_at`, and the static formulas.
- **The water side.** The existing `WorldWater.impulse(position, radius, strength)` is a local splash with no units, so the snow side does not call it. A wave contract call (height, period, direction and width in metres and seconds) has been requested from the water owners. Until then, water systems read the signal.
- Checks (`--test=waves`):
  - The formula gives P 1.250 and H_M 6.64 m for F 2, S 0.5, M 1 at 45 degrees in 10 m of water. At 300 m it is 1.86 m ahead and 0.46 m to the side, with a period of 18.9 s.
  - The storm avalanche into an open lake at the face's foot puts 585 t in, 0.09 m thick over 148 m at 10.4 m/s, into 4.2 m of water: one wave, 0.53 m near the entry and 0.25 m 60 m out.
  - 65 cm of ice carries it with no wave and no breaks. 4 cm of ice breaks in 20 cells and 580 t goes in.

## Follow-up 27: the debris and surface look; avalanche pressure in the snow contract (19 Nov 2026)

Docs flagged four things on the wet slide bench: spiky grey cones at the toe, a crown drawn as a row of dark dots, dark specks mid-slope, and faceted low-poly foreground snow.

- **Cones** were clumps stacking. Each stop event dropped blocks on its cell, and a cell that stopped again and again piled them into cones metres high. Now the granules are dropped once per cell when the run ends, on the settled debris, over the whole cell. They are sized as avalanche granules (`SnowClumps3D.drop` size_scale 2 dry to 5 wet, so about 10 cm dry and wet snowballs of about 40 cm; Bartelt and McArdell 2009) and use the flow's own wetness.
- **Debris relaxation**: after the two smoothing passes, the debris slumps to its angle of repose, 32 degrees dry to 40 wet. This is talus relaxation, the thermal-erosion pass of Musgrave et al. (1989), a quarter of each excess per pass, up to 16 passes. A lone stopped cell no longer stands as a spike.
- **Facets** came from bilinear depth between 2 m cells, which creases along every cell edge. The drawn surface (`SnowSurface3D`, GPU) and its collision and deformation columns (CPU, `SnowCover3D.get_surface_depth_at`) now use Catmull-Rom over 4 x 4 cells (Keys 1981), clamped to the four nearest cells' range so it never rings. It still passes through every cell value, so no detail is lost.
- **Crown dots and specks** were bare ground. The bench storm lay on bare ground, so the scar and thin patches showed earth. The `--wet` and `--triggered` views now lay an old, bonded 60 cm pack first, so the slab leaves a white bed and a step.
- **Pressure contract**: `SnowCover3D` answers world-links' `WorldSnow.avalanche_pressure_at` and `avalanche_peak_pressure_at` from its avalanche node. The forest test reads 20 kPa through it. The peak is per run (it resets when the next avalanche releases).

## Follow-up 28: the wet deposit's shape; path-traced surface refresh (19 Nov 2026)

Docs: the wet debris covered the toe evenly at one density with no tongue, lobed edge or pile-up, so it read as gravel.

- **Lobes:** wet snow's friction varies with its water and grain. A front with patchy friction breaks into lobes and fingers (fingering in granular fronts, Pouliquen et al. 1997; the lobate snouts of wet avalanches, Bartelt et al. 2012). mu varies +-25% x wetness through smooth value noise over about 3 cells (integer hash). Dry flows are unchanged.
- **Granules** gather where the debris is thick (snout, levees, pile-ups): none under 30 cm, full share from 1.5 m.
- **Recent snow settles with the pack.** It was tracked as the depth that fell and only capped by the pack, so a storm that settled read as the whole pack, and a release scoured to the ground (the dark crown dots). Now it scales with the pack's shrinkage between updates, not counting new snow, released slab or entrained snow.
- **Bench:** the `--wet` view thaws 12 h at +5 C (35% wet), `--toe` frames the tongue from above, and the deep-snow mesh covers the run-out (extent 160 m).
- **Path tracing:** the tracer recaptures a mesh's vertex() output when a material parameter changes, not when a texture it samples is rewritten in place. The deep-snow surface's displacement reads the cover's snow texture, so under PT the deposit stayed flat. `SnowSurface3D` now bumps a `snow_serial` uniform whenever the cover updates. Any PT shot of the surface after a change before this (e.g. the round-2 avalanche pair) may show the old surface.

## Follow-up 29: crown walls; rubble over the whole deposit (19 Nov 2026)

Docs: the granules sat in one dense oval, the lobe was smooth pillows, and the crown read as thin bright straight lines.

- **The oval** was two limits. Granules are kept only inside the deep-snow print field (`deform_extent`, 32 m around the follow target), and they were dropped only where the flow arrived faster than 5 m/s, which the slow wet snout never does. Now granules go on every debris cell over 10 cm outside the release zone (`released_cells`). Their share is 0.35 + 0.65 x max(margin, (distance/run-out)^3): margins are cells whose neighbours hold much less debris, and the snout is the far end. Debris sizes spread wider (log-normal sigma 0.75), and wet debris carries up to 2.5 times the share. The bench widens the print field to 96 m.
- **Crown walls**: 2 m cells cannot hold a vertical step, so the crown is geometry. Every edge between a released cell and one that kept its snow (crown and flanks, not the foot) is chained over cell corners into lines. The lines are rounded with Chaikin corner cutting (three passes) and wander by up to a third of a cell through value noise, then stand up as a wall from the bed to the snow surface beside it (a ragged top, +-2 cm). It is a `MeshInstance3D` child with a snow material and both faces drawn, and it casts shadows. New snow buries it: it goes at 80% of its height. Up to 8 are kept (`crowns_enabled`, `get_crown_count`).
- **Irregular fracture**: a load's release reaches +-15% of its radius over about 12 m, not a circle. The bench triggers inside the face, so the crown breaks across the slope below the ridge.

## Follow-up 30: a slab's outline, a scoured bed, a softer powder cloud (19 Nov 2026)

Docs: the release outline was a closed ring (a cookie-cutter hole), only its top edge read as a wall (the rest a dotted line of dark slivers), and the bed inside was as smooth and white as the snow around. The powder cloud was dark and mottled in both raster and PT (PhysX checked it: the gas's own density, not a renderer bug).

Research: a slab avalanche leaves a crown (a tensile fracture standing the slab's full depth, across the slope), flanks (shear fractures down the fall line on either side, losing height toward the foot) and a stauchwall at the foot, which the slab rides out over and so leaves no wall to see (McClung and Schaerer 2006, The Avalanche Handbook, ch. 3). The bed is the old surface the slab slid on, the weak layer's faceted grains on an aged crust, greyer than the new snow (aged snow albedo 0.65-0.7, Gardner and Sharp 2010). Ice grains barely absorb visible light (single-scattering albedo above 0.999, Warren 1982), so a powder cloud is lit through by many scatterings and reads bright and soft.

- **Outline**: a trigger releases a slab shape, not a disc: a crown across the slope 0.65 radii above the load (bowed a little downslope at its ends), flanks a radius out on either side drawing in a little toward the foot, the foot 0.8 radii below. The crown zigzags in shallow steps (a run every third of a radius, each set up or down the slope by up to 0.12 radii and slanting one way or the other), the flanks each wander by up to 0.2 radii over about a third of a radius, and all edges carry a finer +-8% wobble over about 12 m.
- **Walls**: every edge between a released cell and one that kept less than half as much is chained (none dropped, so the lines run unbroken). Each point's height is the step there times a fade: full in the upper half of the release, nothing at its foot, and nothing on edges facing up the slope. The wall stands a sixth to a half of a cell inside the released cell, where the cover's rounded step has dropped, so its face shows. A cap runs from its top back to the snow outside, so the edge stays sharp. The face takes the snow's albedo (vertex colour, linear), and its base shades to the weak layer's grey.
- **Bed**: released cells take the old surface's albedo (0.66), streaked +-0.06 down the fall line (value noise one cell across, nine along). The bed was already lower by the slab's depth.
- **Natural releases**: the fracture's spread was depth first, so when it hit its 4000-cell cap it left every other row behind (a striped bed with a wall on every row). It is breadth first now, in all three spreads, so a capped release is a compact patch around its seed.
- **Powder**: white (0.98, 0.99, 1.0) and smoother (vorticity 8 to 4). A thinner gas (density 0.4-0.7, emitters capped lower) was tried: it left see-through ghosts, not a soft white cloud, so the density stays. The grey is the renderer's albedo (below), and raster lights it by single scattering only.
- **Asked of PhysX**: the path-traced volume's single-scattering albedo is fixed at 0.9 (`physx_flow_3d.cpp`), which loses a tenth of the light at every scatter; an optically thick snow cloud then goes grey. A property for it (snow would set 0.99) is requested through the coordinator.
- **Bench**: the avalanche view's lower sky is snow under this sky's own light (horizon 0.53, bottom 0.33-0.46), not the default brown (brown walls) nor a brighter white (it lit raster's vertical walls like lamps, a glowing band PT did not show). The slide runs under the old 30 degree sun (the thaw, so the wet run is unchanged); the shot is taken with the sun 55 degrees up from behind the ridge, which rakes the 38 degree face (from 30 degrees the whole face was in shade, so the walls had no light to show by): the crown stands in its own shade and throws shadow down the bed, in both renderers.
- **Path tracing**: the wall's triangles were wound against their normals. The path tracer treats the winding's side as the front and flips a double-sided normal hit from behind, so the walls came out unlit; each quad is now wound to its normal.

## Follow-up 31: the powder cloud along the whole head (19 Nov 2026)

- **Head**: the eruption current comes from the head, the fastest part of a dry avalanche (Sovilla et al. 2015). In the flow the head's very edge ploughs the snow ahead and slows below the powder speed, so only the slab's sides (where the flow spreads sideways) fed the cloud: a few separate puffs. A cell now also counts as the head when the cell ahead of it is slower than the powder speed (the last fast snow before the ploughing edge), so the cloud rises along the whole front.
- **Albedo**: PhysXFlow3D.scattering_albedo (PhysX 6568b3d2, raster to match 7b6f7ea0) is 0.99 for the powder: ice grains barely absorb (Warren 1982).
- **Spikes under the running flow**: on the slow lateral margins single cells stopped and dropped their whole load (1-3 m) as columns while their neighbours crept on; the snow surface drew them as cones. A stopping cell now comes to rest over its neighbours too (half on itself, an eighth on each side, as the end of the run smooths the deposit), and every flush lets stopped debris slump to its angle of repose (talus relaxation, 4 passes), even under moving snow.
- **Bench**: `--view=avalanche --powder` releases a wide dry slab in front of the camera (`trigger` at -40, radius 60). Left to itself the face let go off to both sides of the frame, and its cloud with it.

## Follow-up 32: the wave into the water; benches that hold under load (19 Nov 2026)

- **WorldWater.wave**: each impulse wave (follow-up 26) now goes to the water as `WorldWater.wave(position, heading, height, period, width)` (Valence 35b34268): the near-field height H_M crest to trough, the period a slide's width out from the entry, and the slide's width. The ripple solver runs it out at sqrt(g h); water without one makes a splash. The slide's width is now the span of its entry cells across its heading; counting cells over-counted it by the length the slide ran on over the water (166 m against 84 m for the 80 m test slab). Test: a WorldWater stub records the wave (`--test=waves`).
- **Background timing check**: compares the median main-thread cost per update, not the worst, so a render running alongside cannot fail it.
- **Powder bench**: an old pack under the storm (the full-depth release showed bare ground, a black hole in the slope), the camera back to hold the whole front, the deep-snow geometry over the whole face, and the still taken at 6 s while the front still feeds the cloud (by 9 s it is decaying into separate puffs).

## Follow-up 33: one cloud along the front; a mountainside for the powder shot (19 Nov 2026)

- **Lanes**: the cloud's intake is gathered in 8 m lanes across the run (the release's fall line), each emitter set at its lane's leading take, fed by the lane's mass. Gathered in 8 m squares, the slab's sides (where the flow spills sideways) stood as columns of their own and the centre had little: three chimneys, not one billowing front.
- **Slope**: `--powder` shoots an `avalanche_bowl` ground: the face steepens to about 45 degrees and eases out into the run-out without a break, the ridge rises on above it (no plateau), the sides rise into spurs (a cirque) and the ground rolls (3 m of noise over about 80 m). The tests keep the plain 38 degree face.

## Follow-up 34: the cloud as the head of a gravity current; benches that step Flow with the slide (19 Nov 2026)

- A powder avalanche's head stands about twice the body's height and sheds billows off its top (Simpson 1997,
  gravity currents; Turnbull and McElwaine 2007 on the Vallee de la Sionne radar profiles). Each lane's emitter now
  lifts by its share of the strongest lane with a fresh random lift every half second (billows of varied height),
  sheds its air at half the front's speed (the wake trails back up the track) and rides on with the front between
  windows (a streak, not a puff left every half second). No lane draws more than twice the median: no side column.
- The bench powder shots were wrong at the root: Flow steps once a rendered frame, but on a slow renderer the
  avalanche took up to 8 physics ticks a frame, so the gas simulated an eighth of the time and the "cloud" was the
  emitters' trail (the bolster and the chimney docs saw). The powder view now runs one tick a frame, frames the
  cloud from three-quarters to the side (`--head-on` keeps the old view) and sets Flow's scattering to full
  resolution for stills. On a real GPU at frame rate the game never had this problem.
- Trail bench: the walker's stand-in capsule joined world_contact (every PhysicsBody3D does since 9edd6953) and
  ploughed a trench through the prints; it is excluded, its boots print as before.
- Done in follow-up 38 (geometry plates). Was open: a broken crust's plates read only as wider funnels with a few angular pieces from 1.5 m
  (`--view=trail --prints-close --crust=1`); the pieces are linear ramps per crack sector, smoothed by the 6 cm
  field. Crisper plates (a step at each sector edge, a lip where a plate tips) are a follow-up.
- Next-round candidate (icicles): done in follow-up 37.

## Follow-up 35: crown walls without slits (20 Nov 2026)

- From afar the crown read as a dotted line, also with the powder cloud hidden. Close up, the wall had slits
  through to the sky: the outline is chained into lines, and the walk breaks a line wherever three or four edges
  meet at a corner. Each line set its end point off the corner along its own edge's normal, so two lines meeting
  there stood their ends up to half a cell apart. Every corner's point is now the mean of all edges meeting
  there (normal, height, albedo), so lines meet end to end. What is left at the crown's steps is the step's own
  face in shade. Bench: `--no-gas` hides the powder cloud and lifts the one-tick lockstep (the slide and its
  crown in about a minute); `--crown` frames the crown from 14 m down the bed, `--crown-far` from 40 m.

## Follow-up 36: the deep-snow surface's normal in one pass (20 Nov 2026)

- Speed pass, step 1. The surface took its normal from central differences of its height, so each pixel ran
  the 16-cell Catmull-Rom depth four times (64 reads) and each vertex five times (80), plus the ground, catch
  and deformation reads at every tap. The depth's gradient now comes from the same single pass, from the
  derivatives of the Catmull-Rom weights (the analytic derivative of a cubic filter, as in Sigg and Hadwiger,
  GPU Gems 2 ch. 20); the fine deformation, ground and edge fade keep their cheap central differences. Deforming
  snow in shipped games reads one height per tap the same way (Michels and Sikachev, deferred snow deformation
  in Rise of the Tomb Raider, GPU Pro 7; Barre-Brisebois, Batman: Arkham Origins, GDC 2014).
- Same image: against the old shader, the trail close-up differs by a mean 0.0 levels and the crown view by
  0.04. Bench whole-frame GPU time (lavapipe, `--time=10`): trail close-up 1484 -> 898 ms, avalanche crown view
  2757 -> 1407 ms. The bench's `--time=N` prints the mean GPU time over N frames.
- Step 2: the wind ripples' share of the pixel normal comes from the noise octaves' analytic gradients (value
  noise derivatives, Quilez) in one evaluation instead of four (12 noise lookups to 3). Image within 0.07
  levels; trail close-up 898 -> 826 ms, crown view 1407 -> 1345 ms.
- CPU side, from `--test` on lavapipe's host: a 256 x 256 cover update runs on a worker (6.7 ms there) and costs
  the frame 1.3 ms mean, 3.2 ms worst, twice a second; a deep-snow surface step with four rolling bodies 0.08 ms.
  Nothing there worth trading detail for. Next candidates if a GPU bench on the PC shows the surface still
  high: a mip of the deformation map for the vertex prefilter (one fetch for four) and the 9-tap bilinear form
  of the Catmull-Rom depth for the vertex height (Sigg and Hadwiger; TheRealMJP's 9-tap filter).

## Follow-up 37: icicles shaped as they grow (20 Nov 2026)

Research: the ideal dripping icicle (Short, Baygents and Goldstein 2006, Phys. Fluids 18, 083101) is a
parameter-free shape whose radius away from the tip goes as the distance from the tip to the power 3/4,
r = 4/3 a^(1/4) z^(3/4); most lab icicles scatter round that exponent, tap-water ones with a bulge high up
(Chen and Morris 2013, arXiv 1008.1922). Icicles bend when their root moves, when the film runs down one side,
and in wind (Maeno, Makkonen and Takahashi 1994, Ann. Glaciol. 19): drag on the 2.44 mm pendant drop over its
weight gives tan(lean) = 3 rho_air Cd U^2 / (8 rho_w r g), about 0.01 U^2, and they measured leans well past it.

- **Shape**: drawn as the ideal icicle, root radius 0.913 of the growth model's cone so the drawn ice holds the
  same mass (a 3/4 power profile holds 0.4 R^2 L against the cone's R^2 L / 3), rings closer toward the tip.
- **Lean**: the axis is kept every 2 cm as the tip lays it down. Each new step leans downwind by twice the drop's
  drag (at most 45 degrees) plus the site's own lean from its film's side (up to a slope of 0.08, our pick), so
  an icicle that grew through a windy afternoon and a calm night is bent, not tilted. Melting takes the points
  back. `wind_direction` (local, horizontal; `advance()` sets it from the weather's wind), `get_icicle_offset`.
- **Drops**: a tip with water left over after it froze what it could hangs a pendant drop (2.5 mm water sphere,
  the ice's clear material), and melting tips drip too. `is_icicle_dripping`.
- **Curtains**: where neighbours hang closer than the thinner one's radius, the film bridges the gap: a web of
  ice 0.6 of the thinner icicle thick from one wall's surface to the other's, down to where the gap opens past
  that (`curtains`, on). First built from axis to axis, inside both icicles: the path tracer takes every front
  face as air into ice, so ice drawn inside ice refracted twice and showed white wedges (docs caught it). Where
  walls themselves overlap the same limit holds; the bench's dense eave keeps sites 5 cm apart (roots 4 cm).
- **Ripples as geometry**: Chen and Morris (2013, arXiv 1301.4734) measured a 1 cm wavelength whatever the
  conditions and amplitudes from under 0.1 mm (pure water) to a few millimetres (salty), roof melt behaving like
  tap water; natural icicles 7-10 mm (Maeno et al. via Ueno 2011). Long icicles now carry them in the mesh, four
  rings a wavelength. 0.25 mm (slope 0.16) read as a threaded screw and sawtoothed raster outlines (docs), where
  photographs show faint, slightly irregular banding in refraction: now 0.03 mm full grown (our pick; slope 0.02,
  already a visible swing in what the ice refracts), each wave 0.5-1.5 times that, spacing wandering up to 15%,
  fading over the lower 40% toward the tip. Docs' ~1 mm figure is within the salty lab range, not roof melt.
- **Slender**: the wall's wetted share is now 0.25 (length grows 16 times diameter, the middle of Maeno's 8-32);
  a week's dense eave grows 0.5 m icicles 4 cm across at the root (1:12). The hazy core stays a column at most
  3 mm across (scaled with the profile it widened into a white wedge at thick roots).
- Bench: `--test=icicles` adds plumb in still air (steepest 0.078), 27 degrees in a 5 m/s wind, tips dripping
  while they grow; `--view=icicles --wind=4` leans them, `--dense` (a week of melt, 5 cm sites) shows ripples and curtains.
- **Drip points** (docs: evenly spaced, same shape, same length read as repetition): melt drains through the
  pack in flow fingers and down the roof in rivulets (Waldner, Schneebeli et al. 2004; Marsh and Woo 1984) that
  reach the eave at drip points; icicles grow from each one's water flux (Makkonen 1988). Drip points are now a
  Poisson scatter (mean 0.4 m), each with a lognormal flow wetting about 3 cm either side, and wet and dry spells
  (smooth noise, a knot every 8 h of melt): rivulets wander as the pack changes. 60% of sites between them stay
  dry, the rest get a thin film and grow stubs whose wall freezes it all. Dense week: 30 long, 25 stubs, 21 bare
  of 97 sites, in clusters. Each taper scatters within 3/4 +- 0.08 (Chen and Morris 2013).
- **Core in raster**: the clear ice is a cylindrical lens: a ray entering at b bends to asin(b/(nR)) and passes
  the axis at b/n, so the core shows n = 1.31 times its width (corrected 20 Nov: an earlier formula gave
  n/(n-1) = 4.2 and drew the core as a wide opaque band, so icicles read as grey metal spikes against a dark
  wall). Raster's transmission reads almost straight through the middle, so the core's vertex() widens it to
  that apparent width outside the path tracer and shadow maps (the hair strands' IN_PATH_TRACER pattern).
- **Roots attached** (21 Nov): an edge placed under the eave left roots floating with their caps showing from
  above. Each root's column now runs straight up to the first surface above it within `attach_reach` (0.5 m;
  meshes under `attach_path`, the parent's by default), 5 mm on into it: the film's ice along its drip path.
  `--ice-drop=M` lowers the bench's edge to check it; tests: 0.200 m to a slab 0.2 m up, none past the reach.
- **Raster transmission on solids**: the read takes the mip of the refracted footprint (UV derivatives), so thin
  curved ice no longer speckles or dashes; panes and open water identical (materials bench).
- `--hero`: from under the roof 1.7 m back looking out (snow to 64 m); `--hero --wall` head on against boards.

## Follow-up 38: a broken crust as plates (20 Nov 2026)

Research: a crust on soft snow is a plate on an elastic foundation; under a point load it cracks radially first,
then round at about its characteristic length, and the wedges between the cracks tip into the hole on their
outer edge (Hetenyi 1946; the same sequence as floating ice breaking through under a load, Kerr 1976). The
pieces are slabs as thick as the crust with sharp fracture faces; a postholed melt-freeze crust shows them as
angular plates leaning into each hole, steps between neighbours that tipped differently.

- The shared deform field already tips each of its seven sectors in as a ramp, but its 6 cm texels round the
  ramps off (follow-up 34's open item: wider funnels, not plates). `SnowSurface3D` now lays each break's plates
  as geometry over it: per sector (the field's own seed, so each lies on its ramp), a flat slab from the hole's
  edge to the sector's crack reach with a jag in its outer crack, as thick as the crust, tipped by the ramp's
  drop (read half way out and extrapolated, not from the footprint's own hole), the hinge edge lifted by up to
  0.6 of the crust's thickness, 3 mm cracks between plates. Glazed top (roughness 0.45), the fracture faces a
  little greyer and bluer. Laid a third of a second after the break (the body has sunk); new snow over the
  rim buries them; up to 256 holes kept. `crust_plates` (on), `get_crust_plate_count`.
- A body dragged through breaks at the hole's edge as it goes and keeps one set of plates (the trench's edges
  stay the field's ramps); separate steps each get theirs.
- `--test=crust` adds the plates' check; `--view=trail --prints-close --crust=1` shows them.

## Follow-up 39: surface hoar (round 5, 20 Nov 2026)

Research: hoar grows when the surface radiates below the air's frost point on a clear night with a light wind
(Hachikubo and Akitaya 1997; Colbeck 1988); the deposition is a bulk vapour flux,
E = rho_air C_E U 0.622 (RH e_w(T_air) - e_i(T_s)) / p, the surface's deficit set by net longwave against the
turbulent and pack heat flux (Andreas 1987 for C_E over snow). Wind over about 5 m/s at 10 m breaks the crystals.
Buried, it is the classic persistent weak layer, its shear strength rising with age and falling with crystal
size, (400 + 35 days) sqrt(5 / mm) Pa (Jamieson and Schweizer 2000).

- `SnowCover3D.hoar_enabled` (on): per cell, the surface's cooling deficit is (70 W/m2 (1 - 0.95 N^2) - absorbed
  shortwave) / (rho cp C_E U + 5 W/m2K), with C_E 0.0015 and U at 2 m = max(0.75 U10, 0.3); the crystals grow
  70 mm per kg/m2 deposited, to 30 mm. Wind of 5 m/s breaks them (e-folding 0.5 h); rain, melt, air above 0.5 C
  or drifting snow clears them.
- New snow buries them as a layer (the newest, unless an older shallow one is larger). It settles with the pack,
  melt erodes the burial, dropped after 1000 h. `get_hoar_at`, `get_buried_hoar_at`, `_depth_at`, `_age_at`,
  static `hoar_deposition` and `buried_hoar_strength`.
- Avalanches read the buried layer as a weak layer in release and in the skier trigger (no collapse propagation
  yet).
- The look: the catch map's alpha is 1 + mm / 10. `snow_glint` and `snow_grain_facet` take the hoar (one function
  in BaseMaterial3D's snow pass, `SnowSurface3D` and the path tracer): the cell grows to the crystal (0.6 of its
  size), the share of mirror faces from 0.25 to 0.35, and a share (full above 4 mm) of the facets are plates set every way,
  their normals spread evenly up to 60 degrees off the snow's: fewer, larger, brighter glints from any sun. Trodden snow and the
  ground under a roof have none.
- `--test=hoar` (11 checks: clear calm night grows, wind, cloud, dry air and day do not, burial, weak layer, skier
  load, strength by age); `--view=sparkle --hoar=MM` for the look.

## Follow-up 40: rime (round 5, 20 Nov 2026)

Research: in-cloud icing on structures is Makkonen's (2000) model, dM/dt = a1 a2 a3 w v A: the collision
efficiency a1 from the droplets' inertia (Stokes number K = rho_w d^2 v / (9 mu D)) on a cylinder of diameter D,
fitted by Finstad et al. (1988) and used in ISO 12494 and the wind-turbine icing models; a2 and a3 are 1 in dry
growth (rime, well below 0 C at modest liquid water). Small collectors catch most (a wire or a twig far more
than a post; under K 0.17 the droplets follow the air round it), so rime gathers on wires, twigs and the edges of
boards, and the growing deposit widens its collector and slows (the time-dependent form). Its density is
Macklin's (1962) 110 R^0.76 kg/m3, R = -d v / (2 Ts): soft feathery rime in light cold wind, hard rime toward
900 in strong wind near freezing. It grows into the wind as feathers and vanes (the classic mountain-top rime
photographs, e.g. Fikke et al. 2007, COST 727).

- `SnowRime3D`: targets are the meshes under `target_paths` and in the `rime` group. Each mesh's points are
  welded; a point's collector is the part's smallest width, made up to 0.1 of it at sharp edges and corners, plus
  twice the rime already there. Per hour below 0 C the windward points (normal into the wind) gain
  `accretion_rate` times the facing; the way it grew is kept per point. Above 0 C it melts at 4 mm/h per C.
- The look: a crust a third of the deposit thick (to 3 mm) over the rimed faces, and feathers rooted on it,
  pointing the way the rime grew (with a fifth of the surface normal), each a flat vane with a ridge, as long as
  the deposit with a spread, seeded per triangle so they stay put as they lengthen. Spacing 1.2 cm on thick
  parts, closer on thin ones; `max_feathers` (40,000) thins and widens them past that. White matte ice with a
  little backlight. Real geometry, so the path tracer shows the same.
- Weather: `advance()` reads the air temperature, wind and the weather's fog water (`fog_water` overrides it),
  with each update of the active `SnowCover3D`.
- `--test=rime` (7 checks: efficiency by size, density soft to hard, rate, none above 0 or without fog, windward
  growth into the wind, feathers drawn, thaw); `--view=rime [--close]` a fence and a bare shrub after a day in
  0.25 g/m3 fog at 8 m/s and -6 C.
- Next: tree crowns (rime on needles and twigs, through the trees thread's crown meshes or a tint they read),
  glaze (wet growth near 0 C with high liquid water: clear ice, not feathers), and rime shedding in sun.

- **Look, round 2** (21 Nov, from the scene's close-up: hard faceted triangles, bare twigs, smooth posts):
  - Feathers are vane cards cut by an alpha-tested texture to a feather's outline with 7 barbs a side (geometry for
    the vane, texture for barbs too fine to model, as grass and fur cards do), drawn from both sides. Normals bend
    from the surface's at the root to the growth direction at the tip; vertex shade 0.62 at the root to 1 at the
    tip, 0.86-1 per feather, crust 0.7: tufts with depth, not facets. Backlight 0.55. Budget 60000; parts under
    2 cm (twigs, wires) keep every feather, the rest share what is left.
  - Bare twigs were a collector bug: the width came from the world box, so a slanted twig counted as 30 cm
    wide (efficiency near 0). Now the smaller extent across the part's principal axes (test: 20 mm pole at 45
    degrees reads 20.0 mm).
  - Wide faces: a roughness collector (1 cm, 35% of its efficiency; Makkonen 1984, rime on structures grows
    from roughness elements) sets the least a windward face catches. Faces parallel to the wind still take
    none, as on real posts: a post's side seen across the wind stays bare but for its windward edge.

## Follow-up 41: spring snow, suncups first (round 5, 21 Nov 2026; research before building)

- **What suncups are.** Hollows from 2 cm up to half a metre across, in a cellular pattern with sharp ridges, on
  melting snow in bright, dry, calm weather (Betterton 2000, arXiv physics/0007099; Post and LaChapelle). Their
  height obeys an ablation equation, dh/dt = -P/L + D grad^2 h: a hollow takes the light its walls reflect
  into it (a nonlocal term with a line-of-sight test) and so deepens, while diffusion (sun through the top
  centimetre of grains, about 1 cm extinction) smooths short waves. Linear growth rate
  omega = (a I / (pi L)) q - D q^2: one fastest wavelength. Mitchell and Tiedje (2010, JGR 115 F04039) evolve
  a nonlinear form from random starts into quasi-periodic, chaotically drifting cups, fully developed in about
  5 days of full sun in the field.
- **What stops them.** Cloud, wind and warm air heat the surface evenly and smooth the cups out (Matthes;
  Rhodes, Armstrong and Warren 1987); rain and new snow fill or flatten them. Thin dirt darkens and speeds
  ablation, thicker than about 3 mm insulates (Driedger, via Betterton); dirt gathers on the ridges, which
  then ablate slower, so dirty snow cups deeper (Workman: dirt on ridges 30-45 cm above the hollows). Thick
  dirt blocks cups entirely (Mount St Helens ash on Mount Olympus).
- **Penitentes**, the same instability by sublimation in high, very dry sun, 1-6 m: out of scope for the
  game's climates; noted for mortal-plane.
- **Build plan.** The cover's cells are 2 m, too coarse to evolve cups, so: (1) a per-cell cup amplitude on
  the cover that grows with clear-sky melt hours toward the measured size (to about 0.3 m of relief in 5
  sunny days, faster with ridge dirt, slower with thick dirt), and decays under cloud with melt, wind, rain
  and new snow (filled by the new depth); (2) a periodic suncup tile made once by evolving the nonlinear
  ablation equation from noise on a 64 x 64 grid (the published method, so the cells, ridges and spacing are
  physical rather than a Voronoi stand-in), read at two rotations and scales and modulated by a slow noise so
  no repeat shows; (3) the surface's vertex height and normals take amplitude x tile (real geometry, as Paulo
  prefers), with the ridge dirt drawn darker on the crests. Then rotten snow (postholing deeper in a wet
  isothermal pack) and slush over frozen ground.
- **Built (21 Nov).** `SnowCover3D` splits each cell's melt into the direct sun's share and the air's; relief
  grows logistically from 1 cm of roughness, 0.045 per mm of sun melt (in steps of at most 2 mm), to 0.3 m
  (x1.5 with dust 1e3-1e5 mg/m2, none past 1-4 kg/m2 of dirt, at most half the pack); e-folds per 40 mm of air
  melt and per 10 mm of rain; new snow fills it by its depth. Tests: 0.23 m after five sunny days (25 mm sun,
  5 mm air a day), 0.03 m after three warm cloudy days, filled by 12 cm of new snow, 0.32 m dusty, 0 under a
  thick layer, at most half a 20 cm pack. `SnowSurface3D` draws relief x a 96 x 96 tile grown once by the
  damped Kuramoto-Sivashinsky equation (g 0.1, t 60, about 8 cups a tile, 3.2 m: cups about 0.4 m), laid by
  hex-tiling (Mikkelsen 2022) so no repeat shows; geometry in vertex(), the same height in the fragment's
  normal. Raster and PT agree (`--view=trail --suncups=0.2`). Next: ridge dirt drawn darker, rotten snow,
  slush.
- **Rotten snow, built (21 Nov).** Wet snow loses its bonds (Yamanoi and Endo 2002), so the Bekker moduli scale
  by `SnowSurface3D.rotten_share` = 1 - 0.85 x wetness, and the dent cap packs toward 700 kg/m3 with up to 0.8
  of the snow thrown aside (a bearing failure rather than a compaction). The GPU field reads the wetness from
  the cover's part 1 .z (ground 518018ad). Test: 0.06 m in the cold, 0.19 m in the same pack at wetness 0.5.
- **Slush, built (21 Nov).** Frozen ground stops infiltration (Kane and Stein 1983), so over concrete frost
  (`WorldGround.frost_at`: frost over 5 cm, no thawed top) the water the pack cannot hold stays in its base as
  a saturated layer and leaves by Darcy flow down the slope (slushflows: Onesti 1985, Hestnes 1998): K from
  Shimizu's (1970) wet-snow permeability, about 8 mm/s, over a 50 m slope as a linear reservoir, so it
  e-folds in about 5 h at 20 degrees and two weeks on the flat (a slush pond). Water past the pores runs off.
  `get_slush_at`; the snow texture's wet channel carries 1 + the slush share of the depth, and the surface
  goes grey-blue (albedo x 0.5-0.6, Warren 1982) and glossy as the water table nears the top. Without a
  ground provider nothing is sealed. Next: lateral routing of slush between cells (it gathers in hollows).
  Round 2: the slush is pore water in its own field (`cell_slush`, mm), outside the SWE, so the pack gets no
  deeper when rain ponds in it; it freezes into the base in the cold (denser, not deeper); a print pressed
  below the water table fills with a dark, smooth pool. Raster and PT agree (`--view=trail --slush=150`).

## Follow-up 44: melt water from the whole terrain (round 6, 22 Nov 2026)

- The cover already handed its runoff (rain through the pack and melt, mm/h) to the weather map as the ground
  water WeatherRunoff3D routes, but only the near square's: outside it the weather fell back to rain, so a warm
  spell melting the mountain sent nothing downhill. Spring melt and rain on snow are the main flood water in
  mountains (Marks et al. 1998, Hydrological Processes 12).
- With a world tier the source is now the world's cells (4 km at 16 m) with the near square averaged into the
  cells it covers (by covered share at the square's edge), one source so nothing counts twice.
  `get_runoff_at` outside the square reads the world tier.
- Checks (`--test=climate`): a warm clear spell's melt outside the square is the weather's ground water there
  (0.74 mm/h both); rain on a square cleared bare over the world's 527 mm pack gives 4.00 mm/h, the square's,
  not the two added.
- Not here: rivers (PhysXRiver3D) take their discharge from the terrain climate, not from this field; a
  seasonal melt pulse in them would be the rivers' change.

## Follow-up 45: wind slab (round 6, 22 Nov 2026)

- Drift deposits were already laid down at 300 kg/m3; what was missing: snow falling in wind, the packing of
  snow the wind works over in place, and a record of the slab for the avalanche stability.
- Snowfall in wind: Crocus's new-snow density 109 + 6 T + 26 U^0.5 kg/m3 (Vionnet et al. 2012, GMD 5, after
  Pahaut 1976), or Hedstrom and Pomeroy's where higher (calm). 151 kg/m3 at -8 C and 12 m/s against 70 calm.
- Wind packing: Crocus's driftability SI = -2.868 exp(-0.085 U) + 1 + Mo (Guyomarc'h and Merindol 1998) with
  mobility from density and freshness (albedo stands for dendricity); where SI > 0 at the cell's own wind
  (MicroMet weights and lee) the top 10 cm settle toward 350 kg/m3 with time constant 48 h / (0.632 SI). Sommer
  et al. 2018 (The Cryosphere 12): packing only with drift. The pack is one layer, so the top starts from the
  pack's density.
- `cell_wind_slab` (m): deposits, snowfall while SI > 0, the packed top; settles with the pack, melts from the
  top, erosion takes it once the softer snow over it is gone. `get_wind_slab_at`, `wind_packing_enabled`.
- Checks (`--test=windslab`): SI's thresholds (fresh 100 kg/m3 drifts from about 4 m/s, old 300 kg/m3 from
  about 11); 12 h at 14 m/s packs 0.1 m of slab, calm none; a windy snowfall is twice as dense and is slab.

## Follow-up 46: sastrugi (round 6, 22 Nov 2026)

- Sastrugi are erosional bedforms of a cohesive surface (Filhol and Sturm 2015, JGR Earth Surface 120): drifting
  snow scours a hard (wind-packed or sintered) surface whose hardness varies, leaving the hard patches as ridges
  along the wind with steep prows into it, 5-50 cm tall, 1-5 m long.
- Cover: `cell_sas_x/z` hold relief (m) times the carving wind's direction. In `_blow`, a cell whose surface is
  hard at the start (wind slab with no soft cover, from follow-up 45's new `cell_slab_cover`, or 250 kg/m3 and
  more) and that does not gain snow takes 2% of the snow scoured off it (the saltation's outflow, whatever came
  in) as relief along the wind; deposits, new snow and melt fill it by their depth; at most 0.4 m and half the
  pack. Our rate: 5-6 cm an hour at 24 m/s, about nothing under 12.
- Surface: `SnowSurface3D.sastrugi_tile()` erodes a hardness field (periodic value noise, five times finer across
  the wind than along) at (1.2 - 1.6 H)^2, faster on faces into the wind and 15% in Werner's (1995) 15 degree
  shadow, then squares the height (flat floors, raised ridges). The shader lays it with the suncups' hex-tiling,
  turned to the cell's wind with +-9 degrees of jitter, and adds it to the height like the suncups (real
  geometry, raster and PT alike).
- Checks (`--test=sastrugi`): the step's carving, fill, cap and turning; on the cornice ridge in a 24 m/s
  westerly the windward snow, packed by the wind, carves 0.19 m along +x in 6 h; none on the lee drift (336 mm)
  and none without wind packing (soft snow). Bench `--view=trail --sastrugi=M[,deg]`, `add_sastrugi`.
- Media: /mnt/project-files/fork/snow/sastrugi/ (before/after raster, after PT).

## Follow-up 47: depth hoar and wind loading on weak layers (round 6, 22 Nov 2026)

- Depth hoar: over a temperature gradient above about 10 C/m (Akitaya 1974; Marbouty 1980; Colbeck 1983;
  SNOWPACK's kinetic growth, Lehning et al. 2002) the base turns to large facets over one to three weeks. The
  pack is one layer, so the cover keeps the base's share (`cell_depth_hoar`, 0..1): the gradient is the air (at
  most 0 C) over a base near 0 C across the depth; the share grows at (G - 10) / 10 per two weeks, stays under
  weaker gradients, goes in about a day once the pack holds liquid water (over 3%).
- Strength: between sintered rounded snow (4 x 14.5 (rho / rho_ice)^1.73 kPa) and facets (18.5 (rho /
  rho_ice)^2.11 kPa, Jamieson and Johnston 2001) by share, at a layer density of 200 kg/m3 (744 Pa at share 1).
  `SnowAvalanche3D` tests the whole pack as the slab over it in natural release and in the trigger index: full-
  depth releases on a loaded shallow-season pack.
- Wind loading: drift deposits now add to buried surface hoar's burial (and erosion takes it), so wind slab
  over buried hoar loads it as new snow does.
- Checks (`--test=depthhoar`): two weeks at 20 C/m make a full share, 8 C/m none, wetting ends it; ten days at
  -15 C turn 0.29 m to depth hoar, 1.79 m not; on the 38 degree face 1.25 m of old snow plus 30 cm of storm holds
  over rounded snow (S 1.11) and releases on depth hoar (S 0.86).
- Seen on the way, not changed: the new-snow slab's load uses the pack's bulk density, so on a dense old pack it
  overstates the new snow's weight (a 50 cm storm reads S 0.79 over old snow where its own 100 kg/m3 gives 1.1).
  Needs the recent snow's own mass tracked; noted for a later round.

## Follow-up 48: a drift the wind takes back (round 7, 22 Nov 2026)

Bench: `--test=drift` with `SnowCover3D.add_drift(position, radius, depth, density)` (a cone of wind-packed snow; also
for ploughed banks and piles in scenes). A 1 m drift on 20 cm of loose snow in a 14 m/s gale at -8 C keeps its crest
(120 to 118 cm in 6 h); the same drift on bare ground is gone from its centre in 6 h and creeps downwind.

Why it holds: the transport is saturated. The wind arrives carrying a full load off the open snow upwind, so each cell
passes on what it gets and a cell loses snow only where the wind speeds up (dS/dt = -div Q). The wind's weights see the
ground, not the snow, so over a drift nothing changes and the drift stays. On bare ground nothing comes in, so it goes.

What was tried (diff /mnt/project-files/fork/snow/drift-relief-wip-3.diff, not pushed):
- MicroMet's slope and curvature weights on the snow's own relief (as SnowTran-3D does with the snow surface, Liston
  et al. 2007). The drift does move, but flux rising up the windward face and falling in the lee is anti-diffusive at
  drift scale: the windward face feeds the lee, which traps the incoming load, so the drift steepens into a spike and
  grows (1 m to 1.5-1.9 m). This is the dune instability.
- A slip face (dry blown snow avalanching past 38 degrees, each substep) caps the slope, but the drift still grows
  (1.2 m to 1.8 m on loose snow).

What it needs: the minimal dune model (Kroy, Sauermann and Herrmann 2002; Andreotti et al. 2002), used for snow
bedforms by Kochanski et al. 2019 and Filhol and Sturm 2015:
- Wind shear over the relief from Jackson and Hunt's linear theory, non-local (an FFT of the snow surface on the
  cover grid): the shear peaks upwind of the crest, not on the face.
- A saturation length (about 1-5 m for snow saltation): the flux lags the shear, so the windward face erodes up to the
  crest and the lee fills, and drifts smaller than about 10-20 saturation lengths are flattened, not steepened.
- Slip faces at the angle of repose, and flow separation behind a crest (the lee eddy the obstacle code already has).
Drifts then reach Tabler's equilibrium profile on loose snow, migrate as snow dunes and barchans, and are taken back
where the supply runs out or the wind turns. Cost: one FFT pair per cover update on the near square (64-256 cells).

Reuse, not a second copy: `TerrainDuneSea` (modules/terrain_deform/terrain_dunes.h, the desert dunes) already has
Kroy's shear, tau = 1 + A (H[h'] + B h'), as a direct-sum Hilbert transform along the wind (reach 32 cells) on a slope
smoothed by 1-2-1 passes (the saturation length), with slides that move only the excess over the repose. Its
begin_shear/shear_rows are members on a wrapping grid: split them into a static helper (heights in, shear out,
wrap optional) that the dune sea and the snow cover both call, and drive the snow's transport weight with it. The
ground's sand (GroundSurface3D) has Nishimori and Ouchi ripples and a saturation-length flux for dune grass, at
centimetre and metre scale, not this runaway.

Built (round 7): `SnowCover3D.drift_relief` (on). `DuneShear` (terrain_dunes.h) is now a static helper with heights in,
shear out and wrapping optional; `TerrainDuneSea` calls it (bit-identical: a standalone run of the old and new header
hashes the same, and the terrain_gen checks print the same). The snow takes its relief (depth in cells) along the
wind, 1-2-1 smoothing passes = 2 (l_sat / cell)^2 with a 2 m saturation length (Pomeroy and Gray 1990 put snow
saltation's development length at metres), A 3 and B 0.5 as the dune sea, reach 32 cells, clamped edges; the wind
over the cell goes as sqrt(tau). A slip face at 38 degrees on the snow's own relief (snow on steep ground stays
SnowSlide's) after each blow. The shear is kept 5 minutes of weather, until the wind turns 5 degrees or the square moves.
- `--test=drift`: a 10 m, 1 m high drift on 20 cm of loose snow in 14 m/s becomes a snow dune: in 6 h its crest is
  130 cm 4 m downwind and its old place is scoured; without relief it stays at 118 cm. A 4 m drift flattens (60 to
  43 cm). On bare ground the drift blows away. The links wall case still passes.
- A per-substep slip face changed nothing (the smoothed shear is stable), so it runs once a blow.
- Not yet: flow separation past a brink (the shear only falls to its clamp of 0); flux lag beyond the smoothing.

## Follow-up 49: a layered snowpack (round 7, 22 Nov 2026)

Research: Crocus (Vionnet et al. 2012, GMD) keeps up to 50 layers, a new one per snowfall, merges thin similar ones, and
settles each by a viscosity eta = f1 f2 eta0 (rho / c_eta) exp(a_eta (Tfus - T) + b_eta rho) (eta0 7.62237e6 kg/m/s,
a_eta 0.1 /K, b_eta 0.023 m3/kg, c_eta 250 kg/m3); SNOWPACK (Bartelt and Lehning 2002) keeps finite elements with
grain type. On a 256 x 256 square a game can afford a handful a cell.

Built: `SnowCover3D.layers_enabled` (on), `get_layers_at`, static `layer_compaction`.
- Up to 6 layers a cell, bottom first: snow water, density, age, kind (new, wind, old). A snowfall is a layer (wind
  when it drifts as it lands); a fall within 6 h of the top one of its kind joins it; past 6 the two thinnest adjacent
  layers under the top merge. Wind deposits are wind layers; the wind's packing turns the packed top (split off the
  layer under it) to wind slab at the packed density. New turns old after a week (the avalanche's bonding time).
  add_snow at 200 kg/m3 or more is old snow; add_drift is wind slab.
- Each update each row's layers settle under the weight above (Crocus, temperature from the air at the top to 0 C at
  the base), then are matched to the bulk: what the bulk lost comes off the top (melt, erosion, slides, releases),
  what it gained past them goes into the top, and the bulk's depth change goes to the layers by their pore space, so
  fresh snow takes it and dense old snow little. The bulk (CLASS settling and all the processes) stays authoritative.
- Uses: the avalanche weighs each slab (new snow, over a crust, over buried hoar, the trigger index) by the layers'
  density over its depth, which fixes the round 6 quirk; the wind packs the top by its own density, so new snow on a
  dense old pack drifts and packs.
- `--test=layers`: a calm 40 mm storm on an old pack is [old 51 cm 296] [new 44 cm 91]; the layers add up to the bulk;
  a gale packs the top 10 cm into a wind layer; three days at +8 C take the top and leave the old layer's 150 mm; 50 cm
  of new snow on a dense old pack on 38 degrees reads S 1.31 by its own weight against 0.66 by the bulk density.
- Cost: 16.3 against 13.8 ms an update on a 256 x 256 square (worker thread).
- Not yet: a melt-freeze crust, buried hoar and depth hoar as layers of their own (their fields still carry them);
  liquid water per layer; a cut-through view.

## Follow-up 50: snow sliding off roofs (round 7, 22 Nov 2026)

Research: Eurocode 1 (EN 1991-1-3) and ISO 4355 give the load a pitched roof keeps as a share mu1 of the ground's,
0.8 up to 30 degrees, falling linearly to none at 60. Taylor (1985) and O'Rourke's roof surveys find slippery roofs
(metal, glass) shed at lower pitches than rough ones, so the bed friction goes with the roofing. Heat lost through a
roof melts the bed once it beats the snow's insulation, q > k (-T) / d (Tobiasson 1988, ice dams), which is why warm
roofs shed and cold ones hold. A sliding slab leaves the eave at v = sqrt(2 g L (sin a - tan phi cos a)) and falls
ballistically, so the debris lands 1-3 m out, further off steep or slippery roofs.

Built (SnowBodies3D): a node in group `snow_roof` is a roof face.
- It keeps `roof_shape(pitch) / 0.8` of the snowfall and of the ground's first-sight seeding, none past 60 degrees,
  and holds up to `roof_max_depth` (1.5 m).
- It slides by `slab_slides` with the roofing's friction, `roof_friction`: metal and glass 10 degrees, tile and
  slate 20, otherwise 25 (meta `snow_roof_material`); wetness takes 10 off.
- Meta `snow_roof_heat` (W/m2): when it beats the pack's insulation the bed melts and wets (wet at least 0.6), which
  breaks the bond and lowers the friction.
- `roof_piles` (on): the slab is thrown from the eave at the slide speed and lands as a bank of wind-packed cones
  (add_drift, 1.5 m radius, spaced 0.75 m) along the eave, each cone's share and throw varied a little, mass kept.
  `get_roof_slides`, `get_roof_landed`.
- `--test=roofs`: Eurocode share; 80 cm slides off 35 degree metal and holds on shingles; after a storm metal 0.58 m,
  shingles 1.09 m, warm shingles 0.26 m, 65 degrees none; 5 slides, 6589 kg landed, conserved; bank 2.03 m against
  0.84 m of level snow.
- `--view=roofs`: a shed with metal on one side and shingles on the other, shot the moment after the metal side lets
  go (14th hour of a 4 mm/h snowfall at -6 C): metal bare, shingles 0.59 m, a 1.5 t bank past the metal eave.
- Not yet: the slab as a falling body (PhysX) and its powder puff; ice dams and icicles at the eave from the same
  heat (follow-up 13 has icicles from a heated roof already); snow guards; gutters.

## Follow-up 51: water in the layers (round 8, 22 Nov 2026)

Research: SNOWPACK moves water by a bucket scheme or the Richards equation (Wever et al. 2014); Crocus by buckets
with a holding capacity of about 5% of pore volume. Lab and field work show water stops at fine-over-coarse
interfaces until the fine layer's suction is overcome (capillary barrier) and ponds in the few centimetres above
(Waldner, Schneebeli et al. 2004; Avanzi et al. 2016: 5-15% by volume); ice layers pond more and pass water by flow
fingers. Wet slab avalanches fail where it ponds (Mitterer et al. 2011, the LWC index; Wever et al. 2016).

Built (SnowCover3D, layers on by default):
- Layer kinds crust, hoar (buried surface hoar, its crystal size from its mass) and depth hoar join new, wind and
  old; PACK_LAYERS 8. The top crust is the top's crust-thick part at 450 kg/m3 and stays a layer when buried (the
  bulk keeps only the top crust); snowed-on surface hoar is split off the surface as a hoar layer; the base is depth
  hoar while the bulk's share is over 0.5. Crusts and hoar are never merged away as traces; when full, the two
  thinnest other adjacent layers merge. Wet new or wind snow turns old (wet rounding); wet or six-week-old buried
  hoar is gone.
- Each layer holds its share of the bulk's liquid (`liquid` in `get_layers_at`): water comes in at the top, drains
  from the bottom in the warm and refreezes from the top in the cold, and percolates down within the update. Each
  layer holds liquid_capacity of its ice, plus `layer_barrier` (8% by volume over a capillary barrier, 15% over a
  crust) in its lowest 3 cm. The bulk's water balance is unchanged: the layers say where its water is.
- Avalanches: with layers, `_check_release` weighs every buried crust and buried hoar layer (not only the newest)
  and adds the interface a ponding layer sits on as a wet weak layer; the wet factor reads the wettest layer's index.
- Fix on the way: a trace of rain (the phase's tail, micrograms in deep frost) resets the bulk's buried crust; the
  layers keep it now, so avalanches see it. The bulk field is left as it was (world-tier cornice and nesting
  benches depend on it).
- `--test=percolation`: rain then frost makes a crust layer on top; a 40 mm storm buries it and it stays; 12 mm of
  rain at +1 C ponds on it (index 1.87, 4.4 mm in the layer over it); the layers add up to the bulk; two snowed-on
  hoar nights are two hoar layers (12.3 mm each) where the field keeps one; 30 cm of new snow on a bonded pack on
  38 degrees releases after 13 h at +2 C with layers and not within a day by the pack's mean wetness.
- Not yet: ponding that holds back the pack's drainage (it would change the weather's runoff contract); preferential
  flow fingers; a crust's own strength as a bridge.

## Follow-up 52: roof slabs that fall as bodies; ice dams (round 8, 22 Nov 2026)

Research: roof avalanches leave the eave as a slab that breaks into blocks on the eave and again on landing; the debris
is a lumpy bank 1-3 m out (the same block fracture as slab avalanche debris; Bartelt and Lehning's slab blocks). Ice
dams: CRREL's work on heated roofs (Tobiasson 1988; Buska and Tobiasson 2001) finds them where the building's heat
melts the bed and the water refreezes over the cold eave, mostly with the air below about -4 C; they hold the snow
and feed the icicles.

Built (SnowBodies3D):
- `roof_slab_bodies` (on): the slab leaves the eave row by row (a row a piece's length up the roof leaves that much
  later), as rigid pieces about 1.2 m across of the slab's depth, irregular (vertices pushed by a hash of where they
  lie and of the piece), with snow's friction (0.8 dry); a piece whose speed halves from over 3 m/s breaks in four,
  a dry one throwing up a puff (its own SnowPowderCloud3D, where Flow is); each comes to rest as an add_drift cone
  where it stops, mass kept. Off: follow-up 50's thrown bank. `get_falling_chunks`, `get_chunks_spawned`.
- `ice_dams` (on): the warm roof's bed melt (heat loss over the snow's insulation) from the whole run reaches the eave
  and freezes there, all of it from -4 C; the dam is a ridge twice as wide as high (`ice_dam_height`), drawn along
  the eave as hazy ice with a height that wanders, melting back at ice's 0.3 mm an hour per degree. It holds the
  snow while it stands a quarter of the snow's depth (a rule of thumb for the keyed toe, not a published figure).
- Fix: `set_body_depth` capped roof faces at `max_depth` (0.4 m), not `roof_max_depth`.
- `--test=roofs`: 80 cm off a 35 degree metal roof leaves as 20 pieces, 24 after breaks, all down in 8.4 s, 4984 of
  4984 kg in the cover, a 0.52 m bank; a 30 W/m2 metal roof in 36 h of snow at -8 C grows a 24 cm dam and holds
  0.77 m, 0.15 m without dams. `--view=roofs --fall=S` (0.45 in the air, 10 landed), physics paused for the frame.
- Not yet: Blast fracture of the pieces; a dam view; the dam's water feeding SnowIcicles3D directly (it already
  grows from the roof's heat_loss).
- Review fixes (22 Nov, docs review of the frames): the pieces read as one slab (cut at 0.6-0.75 of their cell,
  shifted, turned, each with its own speed and spin, rows staggered by up to 0.25 s; corners knocked off); the bank
  showed sharp spikes, from two causes: each piece pressed a print with a pushed-up rim as it lay (pieces now carry
  the `world_contact_exclude` meta), and the cone heaps were only a cell or two across on the 0.5 m grid. Heaps now
  go through `SnowCover3D.add_mound`: a smooth (1 - (d/r)^2)^2 dome at least 2.5 cells in radius, kept off cells
  under anything more than a metre above the piece (the bare strip under the eave) and tapered toward them over two
  cells, so the bank no longer cuts across the wall's corner; the whole volume lands. The block stays in sight on its
  heap, still and no longer solid, sinking out over 90 s (at most 128). Pieces sit on collision layer 20 so the
  ground's downward rays (layer 1) never take one for the ground. `--test=roofs` checks a heap keeps its mass and is
  wider than the grid.

## Follow-up 53: the layers in cut walls (round 8, 22 Nov 2026)

Research: a snow pit wall is how the pack's layers are read (Fierz et al. 2009, the international classification;
Greene et al., SWAG "Snow, Weather and Avalanches"): new snow bright and soft, wind slab chalky and dense, rounded old
snow greyer and finer, a melt-freeze crust a hard glazed line, buried surface hoar a thin dark seam (it shows as a
shadowed line when the wall is brushed), depth hoar coarse, loose and dull at the base, wet layers darker. Games
show cut snow as one material; the pack here already knows its layers (follow-ups 49 and 51), so the walls of ruts,
prints and trenches can show them.

Built:
- SnowCover3D strata map (`get_strata_texture`): per cell, the layers merged into bands (same kind and wetness;
  wet = liquid over 0.3 of what the layer holds), scaled to the pack's depth, up to eight bands from the surface
  down, two to a float channel (12 bits each: 4 for the kind, 8 for the bottom's depth in cm). Built with the depth
  map from the live layers each update.
- SnowSurface3D: a steep face (weighted in from about 50 to 75 degrees off flat) at depth d under the
  untouched surface (ground + the cover's depth - the fragment's height, its edge wandering a few mm) takes its
  band's tint and roughness: new 1.0/0.75, wind 0.93/0.85, old (0.84, 0.87, 0.92)/0.8, crust glazed blue
  (0.76, 0.83, 0.92)/0.2, hoar a dark seam (0.55)/0.9, depth hoar warm grey (0.72, 0.71, 0.68)/0.95, wet bands
  x(0.75, 0.8, 0.86) and glossy 0.3. A crust or hoar seam is drawn at least 1.5 cm thick. Raster and path traced
  alike (the same shader).
- Docs review (22 Nov): only one line and one band read, and the wall looked like plaster. Now each band also has
  its grain (value noise at the band's grain size, two octaves, bumping the normal: wind slab 2.5 mm and smooth, new
  4 mm, old 6 mm, depth hoar 12 mm and deep, a crust glazed nearly flat), faded as grains shrink under a pixel; a
  crust stands out as a ledge (lit top, shadow line under it), a hoar seam is a recess (0.55, shadowed at its top);
  stronger tints (old (0.8, 0.84, 0.9), hoar 0.4, depth hoar (0.66, 0.65, 0.62), crust (0.68, 0.79, 0.93) at
  roughness 0.1); the cut's tool marks as streaks down the wall. The view's pack has two crusts.
- Second review: the coarse base read as stucco and soft dark streaks ran down the end wall. Depth hoar's grains are
  now faceted (each 6 mm grain cell its own tilt) with a per-grain brightness speckle (crystal faces to the light or
  the shade), so it reads as loose sugary grains; tool marks are fainter. The streaks were the deformation field's
  rim: pressed rims slump tile by tile and leave a ripple along the wall top that grazing light picks out (the view
  now digs without a rim, as a shovelled trench has none). Not yet: smooth the rim slump across tile seams.
  Grain under a pixel: raster fades the grains' relief and speckle as the pixel's footprint nears their size (it
  aliased into salt-and-pepper before); the path tracer keeps them and averages them over its jittered samples, so the
  two agree at any resolution (both smooth at 960 px across this view, grain at 1920).
- `--test=layers` checks the map: new over buried hoar over old, down to the pack's depth. `--view=strata`: a
  2.4 m trench pressed nearly to the ground (compacted_density 900) and two ruts in an 80 cm pack (depth hoar, new,
  crust, buried hoar, new), seen from inside the trench.
- Not yet: the walls of the cover's own cuts (avalanche crowns, cornice breaks), which are cell steps, not the print
  field; grain texture per band (sparkle of depth hoar facets).

## Follow-up 54: rut and trench walls that give way (round 9, 22 Nov 2026)

Research: a vertical cut in a cohesive material stands up to its critical height, Hc = 2.67 c / (rho g) with a
tension crack behind the face (Terzaghi 1943, Theoretical Soil Mechanics; the 3.85 of the uncracked case less the
crack), and a taller one fails along a plane near 45 + phi / 2 from the horizontal (Rankine; about 62 degrees for
snow's 35 degree friction angle). Snow's cohesion grows with density as a power law, about 14.5 (rho / 917)^1.73 kPa
for the shear strength of a layer (Jamieson and Johnston 2001, Annals of Glaciology 32), and with age as the bonds
sinter (new snow gains strength several fold over a day or two at moderate cold); a little liquid water binds
(capillary bonds, pendular regime) while more wets the bonds away (funicular regime, Colbeck 1982, Reviews of
Geophysics 20); depth hoar and buried surface hoar have almost none (Jamieson 1995; Schweizer et al. 2003). Broken
blocks lie at the angle of repose of debris (about 38 degrees) and take up about a quarter more volume than the wall
did (bulking). In play, SnowRunner's deep-snow trenches cave in at their edges as a vehicle works along them.

Built:
- SnowCover3D `cut_cohesion(kind, density, age, saturation, crystal)` (static): the layer's cohesion in Pa as
  above, sintering (0.3 + 0.7 min(age / 24 h, 1)) (1 + min(age, 480 h) / 48 h), wet x1.5 moist down to x0.1 soaked,
  depth hoar 0.1 of its strength share, buried hoar 0.15, a crust x2. `get_cut_critical_height(pos)` walks the
  cell's layers from the top: the first layer (counting the load of those above) whose bottom lies deeper than the
  critical height of the snow to that depth is where the wall fails; returns (Hc, the failing depth), or 1e6 when the
  pack stands to the ground.
- SnowSurface3D `wall_collapse` (default on): five times a second, the print field's active tiles are scanned for
  face tops (a drop steeper than 70 degrees ahead, none behind), each walked down to its foot. A face taller than its
  cell's Hc (0.6 of it while a body works the tile: the load beside the edge and the shaking bring a driven rut's
  walls in) gives way at a rate rising with how far over it stands, as a block 0.25-0.7 m along the face: the wedge
  in front of a plane between 52 and 64 degrees (each block its own, rough at 6 cm) comes off, down to 0.8-1.0 of the
  failing depth (a wall over depth hoar falls to the hoar's bottom, undercut). The debris lands as a toe from the old
  floor out at the angle of repose, bulked by a quarter, lumpy at 14 cm, never higher than its profile on the old
  floor (so toes never stack into new walls); a debris-topped face (mostly rim) is left to the field's loose-snow
  slump. Part of each block (4 % from cold powder up to 30 % from sintered old snow) lies on the toe as lumps through
  SnowClumps3D's `drop`, the stronger the snow the larger. On the GPU field the changed tiles go up through the tile
  cache (`seed_tile`, `pull_cached`; the snow surface now passes the cache uploads to its step).
- `--test=walls`: cohesion ordering; new powder's Hc 20-40 cm; a 53 cm trench in 60 cm of new powder caves in with a
  toe holding 1.25x the volume off the walls; a trench in old settled snow stands; a wall over depth hoar falls to the
  hoar; a 20 cm rut in new powder stands until a body works along it. `--view=walls`: a trench through 44 cm of new
  snow on 31 cm of old: the new snow sheds blocks to the old, which stands (`--walls=0`: before; `--gpu`: the GPU
  field, collapsing as it runs).
- Docs review (22 Nov): the view moved to two deep tyre ruts across a level snowfield with hills on the horizon,
  the sun along them. Fixes it found: a wall whose top lay just over a tile seam was never scanned (tiles beside an
  active one now count); in a rut too narrow for two toes the second wall's debris now piles on the first toe up to
  its scarp's base (it stood before); a toe that cannot hold a block scales the whole wedge back (it left a column at
  the face); a sliver with a drop on both sides falls at any height (spikes); a face counts as a wall only when its
  mean steepness is over 65.5 degrees, so a fallen scarp (now 52-64 degrees) does not keep failing back into the
  field; lumps never land above the rut's top edge (SnowClumps3D.drop's ceiling).
- Second review (22 Nov): the collapse did not read and raster and path traced disagreed on a wall. The mismatch was
  time, not light: the field kept settling during the shot's hold, and raster's TAA hold runs more frames than the
  path tracer's wait, so the two showed different rut depths; the view now holds the surface still for the shot. A
  hard line where the plain met the hills was the snow normals' step switching from the field's fine one to the
  coarse one at the deformation field's edge; it now eases over the field's outer tenth. The lumps' size now
  comes from the cohesion of the layers that come off (`cut_critical_height`'s block cohesion: a slab over depth hoar
  falls as slab blocks), not from the failing layer's, and the fracture faces are rough at 6 and 18 cm. The view is
  one rut through a wind slab on a base gone to depth hoar, lit from the side.
- Not yet: blocks as falling rigid pieces (as the roof slabs, follow-up 52) for the largest failures; the cover's own
  cell steps (avalanche crowns) crumbling.

## Follow-up 55: slab blocks that fall as bodies (round 10 start, 23 Nov 2026)

Research: a cut or crown in cohesive slab breaks along fractures into angular blocks, flat-faced with sharp edges
(slab avalanche debris and cornice falls: blocks first, rounding into granules only as they tumble far; Bartelt and
McArdell 2009), while new powder disintegrates. The docs review read the wall debris as pebbles.

Built:
- SnowBodies3D `throw_block(transform, size, velocity, spin, density, deposit)`: an angular piece (an irregular
  hexahedron: the box's eight corners each moved up to a fifth of its size, flat-shaded) on the roof slab pieces' path
  (follow-up 52): a PhysX rigid body on collision layer 20, it falls, settles, freezes and sinks out of sight; with
  `deposit` false its snow is not added to the cover again (a wall's block is already in the toe).
- Cut walls (follow-up 54): where the snow that comes off is cohesive (block lump scale 1.8 and up, sintered or
  wind-packed slab), about half the fallen blocks keep one or two pieces whole: up to 0.45 m along the face, a third
  to a half of the block's depth high, half its wedge deep, tipped out over the edge with a small push and spin.
  At most 256 pieces in the air at once. The SnowClumps3D lumps stay at most fist-sized.
- Fix: resting pieces only sank while something else was falling.
- Avalanche crowns: as the crown wall is built, every 0.8-1.4 m where the wall faces down the fall line and stands
  over 15 cm, one piece of the slab's depth (0.4-0.9 m wide, 0.3-0.6 m deep) lies just below it and slides off down
  the bed with the flow's direction. At most 60 per slide. Their snow is already in the flow (no deposit).
  They land on whatever collides there: the ground, and the snow surface's collision within its `collision_extent`
  of the follow target (the bench's `--view=avalanche --triggered --crown` moves it to the crown). Out past that
  they sink to the ground under the snow, out of sight.
- `--test=walls`: a slab over depth hoar throws blocks, new powder none. `--test=avalanche`: the crown throws blocks.

## Next round (proposed 19 Nov 2026, round 3): powder cloud, triggers, sharper flow

**Triggers from people and vehicles.**
- Foehn (1987) gives the extra shear a surface load adds at the weak layer's depth: delta_tau = 2 P cos(psi) sin^2(theta) sin(theta + psi) / (pi r). Here P is the line load, theta is the angle below the surface and r is the distance. Its peak sits roughly straight under the load, so it is about 2P / (pi h) for a slab of depth h.
- The skier stability index SK = tau_strength / (tau_gravity + delta_tau). Our natural S already has every term except delta_tau.
- The loads are a skier's 0.78 kN/m (Foehn), a boot on foot about twice that on a smaller footprint, and a snowmobile about 2-3 times a skier. Thumlert and Jamieson (2014) measured the snowmobile.
- Monti et al. (2016) correct for a layered slab, where a stiff layer spreads the load. Our crust flag can scale that.
- The load comes from SnowSurface3D's prints, which already know mass and footprint, through WorldContracts world_contact. Every body that presses snow can then trigger.
- A cornice failure (follow-up 7, which already reports position and mass) adds its impact as a large P where it lands.
- Remote triggers: a whumpf spreads along a buried crust (follow-up 4) for tens of metres, so a person on the flat can release the slope above. Gauthier and Jamieson (2008) give the propagation saw test. Spread the release flood fill from the trigger cell only along the crust.
- Checks: a skier crossing 60 cm at 38 degrees releases it, the same with no one on it holds, and a sled releases it where a boot does not.

**Sharper, faster flow.**
- Donor-cell transport smears the front, so the run-out is right but the speed is about half of measured values (15 m/s against 20-30).
- RAMMS (Christen et al. 2010) solves the same Voellmy equations with a second-order finite-volume scheme. The fix is to swap the transport step for one too:
  - MUSCL reconstruction with a minmod limiter, then an HLL Riemann flux (Toro 2001, for shallow water) and a Heun time step.
  - The wave speed |u| + sqrt(g h cos theta) sets the CFL step.
- Hydrostatic reconstruction (Audusse et al. 2004) keeps a wet/dry front stable on a slope and still snow still.
- Friction stays a separate implicit step, as now, so mass conservation does not change.
- Check: front speed within 20% of the 1D Voellmy terminal speed sqrt(xi h (sin theta - mu cos theta)), with the run-out unchanged within one cell.

**Powder cloud.**
- Big dry-snow avalanches lift a suspension cloud at the front at 3-10 kg/m3, against air's 1.2. It runs faster than the dense core and can go up to 60 m/s.
- Data comes from Vallee de la Sionne (Sovilla et al. 2015, front eruption currents). The dynamics follow Turnbull and McElwaine (2008); RAMMS::EXTENDED models the cloud as a second layer fed from the dense core's front (Bartelt et al. 2016).
- In PhysX Flow:
  - A heavy-gas emitter (negative `smoke_buoyancy`) sits on front cells where the speed is over 10 m/s and the snow is dry and fresh. Its rate is the entrained mass times a share of about 0.1-0.2.
  - Its initial velocity is the flow's velocity plus an updraft.
  - Flow's wind and terrain floor carry the cloud, so it runs up the opposite slope and settles. What settles adds a thin powder layer back to the cover.
- The cloud's dynamic pressure, about 0.5 rho u^2 (several kPa at 40 m/s), pushes bodies and trees past the dense deposit's edge. That is the blast zone.
- On the raster path it uses Flow's own draw. Under PT it is a participating medium if Flow's volume reaches PT. Otherwise it goes on the PC queue for a check.
- Check: the cloud outruns the core's front and leaves a thin powder layer beyond the debris. No cloud forms from a wet slide.

**Also worth offering.** An avalanche into a lake displaces water (an impulse wave from the WorldWater contract). The impact on trees: the trees thread's thrown-tree path breaks those above a dynamic pressure of about 5-10 kPa.

## Next round (proposed 19 Nov 2026, round 4): forests, wet spring avalanches, avalanches into water

Soot from smoke haze was already built in world-links (21b20d1a, `SnowCover3D._refresh_lap`): dry deposition at 0.2 cm/s from the smoke contract, a tenth black carbon. Nothing to add here.

**Forests and avalanches.**
- *Anchoring:* dense forest stops release. Crown cover over about 50%, or a stem density that breaks the slab, keeps the snow from failing (Teich et al. 2012, Bebi et al. 2009). Read canopy from the trees' cover (`cell_canopy` already exists) and raise the stability index under closed canopy.
- *Braking:* forest takes mass and momentum out of a running avalanche (detrainment). Feistl et al. (2014) fit it as an extra friction term that grows with stem density. Small and medium avalanches stop inside the forest; big ones run through.
- *Damage:* trees break when the flow's pressure beats the stem's bending strength. A dense core at 1/2 rho u^2 gives tens of kPa; Bartelt and Stoeckli (2001) found that 30-50 cm trunks break under about 10-20 kPa over their height. The powder cloud's 1-5 kPa snaps crowns and smaller trees. Send the broken trees to the trees thread's thrown-tree path, and leave a cleared lane that later avalanches run faster down (the classic avalanche path through a forest).

**Wet spring avalanches.**
- Wet snow avalanches run slower and denser, with much higher friction (mu 0.3-0.5) and cohesion. They make levees and deep runnels rather than a fan (Bartelt et al. 2012, wet-snow cohesion; Naaim et al. 2013).
- The trigger is water, not load. The liquid water in the pack (already tracked) weakens the base, and glide cracks open on smooth ground before full-depth glide avalanches (Ancey and Bain 2015; Jones 2004).
- Build: flow parameters that follow the slab's liquid share, a glide-crack rate on warm days, and a check that a wet slide stops short of a dry one, in levees.

**Avalanches into water.**
- An avalanche entering a lake or the sea pushes an impulse wave. Heller and Hager (2010) give its height from the slide's mass, speed and thickness and the water depth, from lab and field data (granular slides into reservoirs).
- This needs Valence's water (WorldWater) to take an impulse. The snow side reports where, how much and how fast through a contract call; the water side makes the wave. It also lays debris on lake ice or breaks it.

## Next round (proposed 20 Nov 2026, round 5): frost, spring snow, spindrift, packed roads

Research notes for Paulo's pick; nothing below is built yet.

**Rime and hoar frost.**
- *Surface hoar:* on clear, calm nights the snow surface radiates below the air's frost point and vapour
  deposits as feathery plates and cups, mm to cm long, that flash in low sun (Hachikubo and Akitaya 1997;
  Colbeck 1988). Growth needs light wind (1-3 m/s) and a humid layer; strong wind breaks it. Buried by the next
  snow it is the classic persistent weak layer: it would lower the avalanche stability index for weeks (Jamieson
  and Schweizer 2000), a real link from night weather to avalanches.
- *Rime:* supercooled fog droplets freeze on contact into feathers that grow into the wind on trees, fences,
  posts and wires (Makkonen 2000, models for rime, glaze, icicles and wet snow on structures: accretion rate =
  collision efficiency x liquid water content x wind speed x area). Needs the haze/fog contract's liquid water,
  WeatherMap3D wind, and a surface list (trees' crowns, props) as icicles take an edge.
- Build: a hoar layer flag on the cover (grown on clear calm nights, broken by wind or sun, buried as a weak
  layer), its glints from the existing grain facets at larger size; rime as geometry feathers on props (a
  shell grown along the wind's direction) and a rime tint on tree crowns via the trees thread.

**Spring snow surface.**
- Suncups: in clear sunny weather ablation hollows 20-50 cm across grow into a cellular pattern with sharp
  ridges (Betterton 2001, Phys. Rev. E 63; Rhodes, Armstrong and Warren 1987: dirt in the snow sets whether
  hollows or dirt cones form). At altitude in dry air they sharpen into penitentes. Dust and soot (already on the
  cover) gather on the ridges or in the hollows by their amount.
- Rotten spring snow: a wet isothermal pack bears little (the crust model's wet case), so walkers posthole
  deep; slush where melt water saturates the base over frozen ground.
- Build: an ablation-hollow height field on the cover in sunny melt (a cellular noise grown by melt hours,
  wavelength from the published 20-50 cm), the dirt split by amount, deeper prints in rotten snow.

**Spindrift plumes off ridges.**
- Snow blown off a ridge or cornice lip streams out in a plume hundreds of metres long in strong wind, a staple
  of mountain vistas. Stage 3's blowing-snow particles cover the near field only. Per the PhysX rule, the plume
  is a Flow gas: emitters on lee edges where the cover's transport flux is high (the cornice code already finds
  them), negative buoyancy for the snow's weight, Flow's wind and terrain floor carrying it.
- Check: no plume under the threshold wind or from wet snow; plume length grows with wind.
- **Built (21 Nov), follow-up 42.** As SnowDrift3D (Schneiderbauer and Prokop 2011) treats suspended snow, a
  scalar carried by the wind with a settling speed: `SnowCover3D` finds lee crest cells (as the cornices: not
  overtopped upwind, the ground falling 30 degrees or more downwind) with loose dry snow, takes the drift flux
  (`transport_flux`, Pomeroy and Male) at the crest's sped-up wind (Taylor and Lee 1984) times the cell width
  as the source, bins them along the ridge (8 m, doubled to fit `max_spindrift`) and feeds an internal
  `SnowPowderCloud3D` (Flow gas, 2 m cells, slight negative buoyancy for the settling, emitted at 60% of the
  10 m wind plus a 0.1 u lift off the lip). The gas fades by sublimation, 0.01 + 0.08 (1 - RH) per second
  (Schmidt 1972). Tests: none at 4 m/s (threshold), 200 kg/s off a 256 m ridge at 18 m/s, 930 at 26 m/s,
  none at +2 C. `--view=spindrift`. Open: in a gale Flow's block allocation fills (speed-threshold
  allocation with no minimum smoke: the wind alone allocates), so the plume is cut off; asked of the PhysX
  owner (summaryAllocate.speedThresholdMinSmoke).

**Packed-snow roads.**
- Traffic packs snow into a hard layer that polishes to ice where tyres spin or brake (Wallman and Astrom 2001,
  road friction: packed snow mu 0.2-0.3, ice 0.05-0.15, wet ice lower). Ruts form where wheels run; slush where
  traffic churns wet snow; tyres throw slush spray.
- Build: per-cell packing and polish from the shared press bodies (wheels already press the snow), a grip value
  in the world contract for world-links' tyre slip, ice glaze look (low roughness, clearer), slush spray via
  PhysX particles.
- **Built (21 Nov), follow-up 43.** Each physics frame `SnowCover3D` reads the `world_contact` bodies on the
  snow: a pass packs the share of the cell it sweeps (car tyre 75 kPa fully, a boot about half), and a slipping
  wheel (RigidBody3D wheel: contact velocity v + w x r; VehicleWheel3D: 1 - skidinfo) puts a quarter of its
  friction power, 0.25 W v_slip, into melt that refreezes as glaze below 0 C. Glaze melts at 0.3 mm/h/K and
  under rain. `get_grip_at`: loose 0.3, packed 0.25, polished ice 0.05 (wet or near 0 C) to 0.15 (-15 C), new
  snow over it 0.3 again. Drawn through the cups texture's G (packed) and B (glaze): packed smoother and a
  little grey, glaze dark and mirror-smooth. Tests: 10 s of a stuck wheel spinning at 10 m/s polishes 0.12 mm
  and drops grip from 0.26 to 0.20. Open: world-links' tyre slip reading it needs a `contract_snow_grip_at`
  in world_contracts.h (asked via the coordinator); slush spray from tyres.

## Next round (proposed 22 Nov 2026, round 6): melt water downhill, wind-shaped snow, weak layers

Research notes for Paulo's pick; nothing below is built yet.

**Melt water into the runoff and rivers.**
- The cover already tracks melt and rain draining out of the pack (`cell_runoff`), but nothing downhill
  reads it: WeatherRunoff3D fills puddles and streams from rain alone. Spring melt is the main flood source in
  mountains (rain-on-snow events, Marks et al. 1998, Hydrological Processes 12; McCabe, Clark and Hay 2007).
- Build: a published melt runoff field (mm/h, the snow contract) that the runoff and rivers add to rain.
  The reader side is the weather code, so the request goes to its owner. Checks: a warm spell raises the
  stream at the valley mouth hours later; frozen ground (the slush seal) sends it overland.

**Wind-shaped snow: wind slab, wind crust, sastrugi.**
- Wind packs the snow it moves into dense, fine-grained slab (200-400 kg/m3; Li and Pomeroy 1997; Sommer et al.
  2018, The Cryosphere 12, wind packing in the lab). On exposed ground the surface erodes into sastrugi: sharp
  ridges 5-50 cm tall along the wind (Filhol and Sturm 2015, JGR Earth Surface 120, snow bedforms; Kochanski,
  Anderson and Tucker 2019 for barchans and ripples).
- Build: the drift deposit takes a denser density by wind speed (wind slab, also a slab source for the
  avalanche stability); a sastrugi height field in the deep-snow surface on scoured cells, ridges aligned
  with the last strong wind and sized by its speed (real geometry, as the suncups).

**Weak layers and natural avalanches.**
- Buried surface hoar is kept (follow-up 39). Depth hoar is missing: a shallow pack over a strong temperature
  gradient (over 10 C/m) turns to large weak cups at the base (Colbeck 1983; Akitaya 1974; SNOWPACK's
  gradient metamorphism, Lehning et al. 2002). Together with the wind slab above it gives the classic
  persistent slab problem.
- Build: a basal depth-hoar share grown by gradient hours; the stability index reads the weakest buried
  layer (hoar or depth hoar) under a slab; natural releases on slopes where it falls below 1 after loading
  (Schweizer, Jamieson and Schneebeli 2003, Reviews of Geophysics 41).

## Next round (proposed 22 Nov 2026, round 7): drifts that move, layered snowpack, snow off roofs

1. Snow dunes and drift take-back: the minimal dune model on the cover grid (follow-up 48): Jackson-Hunt shear by
   FFT, a saturation length, slip faces and lee separation. Drifts migrate, form snow dunes and barchans on open
   flats, and the wind takes them back when the supply stops or the wind turns.
2. A layered snowpack: a few layers per cell (as Crocus and SNOWPACK keep), each with density, grain type and age, in
   place of one bulk layer. Fixes the recent-snow load using the pack's bulk density, and lets crusts, wind slabs,
   buried hoar and depth hoar stack in the right order for avalanches and the cut-through view.
3. Snow sliding off roofs: snow on buildings and props (SnowBodies3D) slides off sloped roofs in a thaw or when
   loaded past the roof's friction (roof snow loads, Taylor 1985; ISO 4355), landing in a pile below (add_drift).


## Next round (proposed 22 Nov 2026, round 8): water in the layers, roof slabs that fall, layers you can see

1. Crusts, buried surface hoar and depth hoar as layers of their own, with liquid water per layer: water percolates
   down (Colbeck 1972 gravity flow; Wever et al. 2014 Richards equation in SNOWPACK, simplified to a bucket per layer)
   and ponds above fine-over-coarse boundaries (capillary barrier, Waldner et al. 2004), which is where wet slab
   avalanches fail. The avalanche then reads the weak layer by its kind and wetness instead of the separate fields.
2. Roof slabs that fall as bodies: the sliding slab becomes a PhysX rigid body (or Blast chunks) that breaks on the
   eave and lands, with a Flow powder puff, so the bank forms where the pieces land; ice dams at the eave from the
   same heat (Tobiasson 1988), feeding the icicles (follow-up 13).
3. Layers you can see: ruts, shovel cuts and avalanche crowns show the stack on their walls, new snow bright and
   fine, wind slab chalky, old snow coarse and grey, crusts as thin glazed lines (the crown face of a slab avalanche,
   SnowRunner's trench walls), from the layer stack per cell through the surface's displacement.
