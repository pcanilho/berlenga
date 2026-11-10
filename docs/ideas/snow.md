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
