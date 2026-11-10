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
5. Appearance: glints, SSS, wet melting snow.

## Sources

Barre-Brisebois, Deformable Snow Rendering in Batman: Arkham Origins, GDC 2014. Michels,
Sikachev et al., Labs R&D: Rendering Techniques in Rise of the Tomb Raider, SIGGRAPH 2015.
Surricchio, Reinventing the Wheel for Snow Rendering, GDC 2023 (God of War Ragnarok).
Fearing, Computer Modelling of Fallen Snow, SIGGRAPH 2000. Festenberg and Gumhold,
Diffusion-Based Snow Cover Generation, CGF 30(6) 2011. Jennings and Molotch, HESS 23, 2019;
Jennings et al., Nature Communications 9, 2018. Hedstrom and Pomeroy, Hydrological Processes
12, 1998. Verseghy, CLASS, Int. J. Climatology 11, 1991. Hock, J. Hydrology 282, 2003.
Bernhardt and Schulz, SnowSlide, GRL 37, 2010. Winstral, Elder and Davis, J. Hydrometeorology
3, 2002. Liston and Sturm, J. Glaciology 44, 1998. Li and Pomeroy, J. Applied Meteorology 36,
1997. Stomakhin et al., A Material Point Method for Snow Simulation, SIGGRAPH 2013. Bekker,
Introduction to Terrain-Vehicle Systems, 1969. Wiscombe and Warren, J. Atmos. Sci. 37, 1980.
Locatelli and Hobbs, J. Geophys. Res. 79, 1974. Deliot and Belcour, EGSR 2023.

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
