# Sand: from prints to physics

Research note, 16 Nov 2026. Written before any code. The ask: sand that deforms and behaves like
real sand, the way snow does, and whether PhysX helps.

## What we already have

- **Prints.** `GroundSurface3D` (docs/ideas/ground-deform.md) gives dry and damp sand footprints
  and ruts on a camera-following field (32 m at 6.25 cm), mass conserving, with Bekker sinkage
  (Wong's dry sand values) and Sumner, O'Brien and Hodgins (1999) slumping to the angle of repose:
  a pit in dry sand slumps to 33 degree walls in 3 s, one in damp sand keeps 61. Wind slowly fills
  prints (`sand_drift_rate`). It is CPU only: 2 ms per physics step while dry sand settles under a
  walker. Bodies stand on the undisturbed collider; the print forms under them.
- **Snow got further.** `SnowSurface3D` has the same field on the GPU (`GroundDeformGPU` since stage 0,
  1.52 ms to 0.04 ms of main thread), a bow wave in front of ploughing bodies, crusts, and a two-way link with
  the world tier. Sand has none of these yet.
- **PhysX module.** `PhysXGranular3D`: a GPU MPM volume (Drucker-Prager, friction angle,
  cohesion, density) that piles, holds a slope and gets ploughed, drawn as a sphere per particle.
  It is a boxed domain, not tied to the ground. PhysX's own PBD particles (CUDA) cannot hold a
  pile (the node's own comment), so for sand the answer to "does PhysX help" is: its MPM path does,
  for grains in motion near the camera; its PBD path does not. Its vehicles
  (`PhysXVehicle3D`, tank, motorcycle) take tire forces per wheel and today see sand as a friction
  value only.
- **Terrain.** `TerrainGenerator3D` Beach and Coast guides; no dunes.
- **Water at the shore** (Valence's files, not edited here): shore run-up and contact wetness on
  `WeatherMap3D`.

So prints exist. What is missing for "real sand":

1. Grains that move as grains: kicked sand, a wheel spinning out a spray, a dug hole's wall
   pouring in, a dune's slipface avalanching in streaks.
2. Bodies feeling the sand: a truck bogging down, wheels digging in as they spin, a person slowed
   and sliding on a dune face.
3. Wind: ripples, sand streaming over crests, prints filled and ripples re-formed, dunes.
4. Water and sand together: the swash rounding prints, wet sand holding a cut wall, runoff
   carving channels.
5. Scale and look: GPU field like snow, sand glints, ripples as real geometry.

## How others do it

### Simulation

- **Height-field granular (the ground state).** Sumner et al. 1999 (built); Onoue and Nishita
  2003/2005 (only where it changed; built as the settle flag). Holz, Beer and Kuhlen 2009, "Soil
  Deformation Models for Real-Time Simulation: A Hybrid Approach" (VRIPHYS): height field
  everywhere, converted to particles only where a tool cuts in, and back once they come to rest.
  This hybrid is the shipped-scale answer to "grains that fly" and is what we would build.
- **Material point method for sand.** Klar et al. 2016, "Drucker-Prager Elastoplasticity for
  Sand Animation" (SIGGRAPH); Hu et al. 2018, "A Moving Least Squares Material Point Method"
  (MLS-MPM, SIGGRAPH), the GPU-friendly form our `PhysXGranular3D` already uses; Tampubolon et al.
  2017, "Multi-species simulation of porous sand and water mixtures" (SIGGRAPH) for wet sand;
  Yue et al. 2018, "Hybrid Grains" (SIGGRAPH Asia), continuum where dense and discrete where
  sparse. Recent: Li et al. 2026, "Adaptive GPU Kinetic Solver for Fluid-Granular Flows"
  (arXiv 2603.14982): MPM sand two-way coupled to lattice Boltzmann air for blown sand and dust;
  0.12 s per step at 1 M particles, offline, but its sparse tiles that follow the particles are
  the right memory layout for a local domain.
- **Depth-integrated sand and water.** Su, Zhang, Pan, Aanjaneya, Gao and Wu 2023, "Real-time
  Height-field Simulation of Sand and Water Mixtures" (SIGGRAPH Asia): four height fields (terrain,
  sand, water above, water inside the sand), Drucker-Prager on a 2x2 deformation gradient, cohesion
  from saturation (dry, pendular at 20%, funicular at 25%, capillary at 33%). 0.5 to 3.5 ms per
  step at 512 to 1024 squared on an RTX 4080. Built on Zhu et al. 2021's shallow sand equations.
  This is the only real-time method found where water carves sand and wet sand holds a wall from
  physics rather than a parameter. No rigid-body coupling in the paper.
- **Terramechanics (vehicles and bodies on soft soil).** Bekker 1969 pressure-sinkage (built);
  Janosi and Hanamoto 1961 shear: traction rises with wheel slip toward the soil's
  Mohr-Coulomb limit, `tau = (c + sigma tan phi)(1 - exp(-j/K))`; Wong and Reece 1967 wheel
  stresses (normal and shear around the wheel's contact arc, giving drawbar pull, compaction
  resistance and sinkage from slip, "slip sinkage"); bulldozing resistance in front of a deep wheel
  (Wong, "Theory of Ground Vehicles"). Project Chrono's SCM (Soil Contact Model; Tasora et al.,
  Chrono::Vehicle) is the reference real-time implementation: a deflecting height grid with a
  refinement patch following the vehicle, Bekker sinkage with the contact width from the flooded
  patch (b ~ 2A/L), Janosi shear, plastic flow to the rim; "close to real time". Chrono's CRM
  (Unjhawala et al. 2025, arXiv 2507.05643) is SPH continuum soil, far above a game budget, but
  its "active domains" around moving bodies are the same idea as our local MPM box.
- **Wind on sand.** Bagnold 1941, "The Physics of Blown Sand and Desert Dunes": threshold
  friction velocity (about 0.2 to 0.25 m/s for 0.25 mm quartz, roughly 5 m/s at 10 m) and the
  cube-law mass flux. Ripples: Anderson 1987 ("A theoretical model for aeolian impact ripples",
  Sedimentology), Nishimori and Ouchi 1993 (cellular saltation plus reptation; ripples
  emerge at 5 to 15 cm wavelength, 0.5 to 1 cm high, crests across the wind). Dunes: Werner 1995
  (Geology, slab cellular automaton), Paris, Peytavie, Guerin, Argudo and Galin 2019,
  "Desertscapes Simulation" (Pacific Graphics, CGF 38(7)): barchans, transverse, linear and star
  dunes from wind regime and sand supply, with vegetation and bedrock; Nilles, Guenther and
  Mueller 2024, "Real-Time Desertscapes Simulation with CUDA" (GRAPP): about 1 ms per step at
  1024 squared on an RTX 4080, floating heights, deterministic saltation, avalanching in 13 to 125
  passes. Dunes are terrain generation; ripples are the near field's job.

### Shipped games and engines

- **Spintires / MudRunner / SnowRunner** (Saber; Zagrebelnyy's breakdown, Gamasutra 2017): the
  benchmark for soft ground under vehicles. Per-16 m block height data, a 128 x 128 mud texture
  only for mud blocks, only blocks near the truck active, CPU vehicle-soil physics that Havok could
  not do out of the box. Feel comes from sinkage, slip and resistance, not from particles; spray is
  cosmetic.
- **Journey** (Edwards, "Sand Rendering in Journey", GDC 2013): the look reference. Diffuse with
  extra contrast, per-grain glitter, a sharp "ocean" specular on dune faces, detail normal maps for
  ripples, sand that streams in wind as sheets of particles. All shading, no simulation.
- **Dune: Awakening** (Funcom, UE5, 2025): dunes as Landscape with Nanite and Lumen,
  sandstorms as volumetrics, the sandworm's wake as a scripted deformation. No published sand
  simulation.
- **Unreal Engine.** No granular solver in Chaos; interactive ground is Runtime Virtual Texture
  plus Virtual Heightfield Mesh or Nanite tessellation, with Niagara for sprays. Marketplace
  "TerraMech" (Oct 2026): 16 m tiles of 12.5 cm cells only where vehicles drive, sinkage with
  spring-back, slip grip, compaction resistance, bulldozing: SCM in a plugin, which confirms the
  game-scale recipe.
- **HDRP / Unity.** Terrain heightmaps editable at run time (`TerrainData.SetHeightsDelayLOD`),
  tessellation displacement in HDRP Lit; sand prints are render-texture stamps. No granular physics.
- **CryEngine.** Tessellated terrain with displacement maps and particle "kick-up" effects;
  no deformation solver (Crysis' sand is shading).
- **Frostbite.** Terrain is a streamed procedural virtual texture with displacement
  (Widmark, "Terrain in Battlefield 3", GDC 2012); destruction deforms with decals and craters
  baked into the height map. No granular flow.
- **Godot addons.** Viewport-texture trail shaders ("Car Tracks on Snow or Sand", godotshaders)
  and footstep-sound plugins. No physics. Nothing to adopt.
- **itch.io and indie projects.** The sand on itch is almost all falling-sand cellular automata:
  2D (Sandustry, Pixel Physics Sandbox) and a few 3D voxel ones on the GPU (Archea; ZigSand3D;
  ARTIFACT, a 3D falling-sand world built in a jam). Each voxel moves by local rules (fall, slide
  diagonally, swap with water). Fun, but a fixed voxel grid, no friction angle or moisture, and
  blocky at our scale: not a fit for open-world ground. The useful idea from them is the
  "sleeping chunk" bookkeeping (only chunks with motion step), which our settle flag already has.
  Slime Rancher 2's snow (Game Developer deep dive, 2022) pushes resting particles aside around the
  player instead of simulating them all: the same pattern as stage 2's local domain.
- **Theses and older work worth a look.** Longmore, Marais and Kuttel 2013, "Towards realistic
  and interactive sand simulation: a GPU-based framework" (Powder Technology 235): GPU DEM with
  grains as rigid clumps of spheres, so grains interlock and hold a pile that round spheres cannot;
  the reason stage 2 draws clumps, and a check on our MPM pile angles. Cimbura's CTU thesis 2022
  and Taylor and Keyser 2023 on GPU dune fields are the steps before Nilles 2024.

Conclusion: nobody ships all of it. The best engines stop at stamps; the best game (MudRunner)
does terramechanics with cosmetic spray; the best papers give real-time pieces (Holz hybrid,
SCM, Su 2023, Nilles 2024). Our field already conserves mass and slumps, which the shipped
stamps do not, so each piece below builds on it.

## What we would build (by stage)

### Stage 0: the ground field on the GPU (performance, open-world scale)

`SnowDeformGPU` for `GroundSurface3D`, as follow-up 13 of the snow note listed as next: presses,
Jacobi slumping, the fresh (turned ground) channel and slow mud creep in compute, async read back
for collision and queries, `gpu_simulation` on with the CPU path as fallback. Expected: the 2 ms
settling step to under 0.1 ms of main thread, and a field large enough (64 m or more) for vehicles
at speed. Same checks, bit-close on both paths. Pure performance; every later stage leans on it.

**Built (16 Nov 2026).** The snow's GPU field moved to the ground module and now serves both:
`modules/ground/ground_deform.glsl` and `GroundDeformGPU` (render thread), plus `GroundDeformGPUHost` (the
main-thread side both nodes share: the stamps, the cache slots, the tile moves and the read back; the
snow's copy of that code went). `params.kind.x` picks the owner's columns:
- Ground columns come from the material lattice (rim share, repose slope, slumping rate, deepest dent,
  darkening, gloss, sand share, deformable; 32 bytes a point, nearest point as on the CPU), with the CPU's
  edge margin.
- The fourth channel of each texel, unused by snow, holds the turned-ground freshness. The cache and the
  read back carry it (two words a texel for the ground, one for snow).
- Slumping takes the giving column's rim, then its dent down to the deepest, at the material's rate.
  Slow material (mud) moves on every tenth of a second by the time saved up, which matches the CPU's tile
  wait. A texel that still moves keeps the slumping it owes, as the CPU's tile marks do; the read back
  carries what is owed, so the node keeps the slumping passes running while any texel owes some.
- Ageing (`_age`: drying, wind filling dry sand, rain softening soil) is one pass a second.
- The blit writes the turned-ground albedo and roughness factors, so the material and the path tracer's
  rt_params read the GPU texture directly (`ground_deform_map` is a `Texture2DRD`).
- `GroundSurface3D.gpu_simulation` (on), `is_gpu_active()`, `sync_deformation()`; queries read the copy
  read back at 15 Hz. The CPU path stays the fallback (Compatibility, headless).

Results (cloud, lavapipe): all 14 ground checks pass on both paths with the same numbers (dry sand pit
62 to 33 degrees in 3 s, damp sand keeps 61, mud 63 after 10 s and 25 after 15 minutes, volume kept to
0.001 l, turned ground dries). All 105 snow checks pass on both paths after the move, with the snow
numbers recorded before (crust test: rim 0.0156 of 0.3285 m3). Trail view in dry sand: main-thread
physics step 2.07 ms on the CPU path, 0.01 ms on the GPU path; moved volume 0.156741 against 0.156739 m3;
the path-traced frames of both paths differ by 0.0002 of 255 on average (max 7, a few pixels). GPU time
is on the PC queue.

### Stage 1: bodies feel the sand (terramechanics, SCM style)

- **Per wheel**, a sand contact read from the field under the contact patch: Bekker sinkage from
  load and wheel width; compaction resistance (the work of pressing the rut); bulldozing
  resistance once sinkage passes a fraction of the radius; traction from Janosi-Hanamoto shear
  with the wheel's slip, so a spinning wheel digs (slip sinkage, Lyasko 2010 / Ding et al. 2011)
  and a truck bogs down. Fed to PhysX vehicles through their tire force component (longitudinal
  and lateral force caps and a resistance torque), not as a friction number. The wheel's rut is
  pressed into the field (the same `press` path), with the dug sand thrown to the rear rim
  when it slips.
- **Characters**: sinkage slows the stride (energy cost of walking on sand is 1.6 to 2.5 times
  that on firm ground; Lejeune et al. 1998, J. Exp. Biol.); on a face steeper than the repose
  angle the footing slides down with the grains (the print slumps under it). Exposed as
  `get_footing_at()` (effort and a slide vector); the game decides how a controller uses them.
- **Rigid bodies** (crates, rocks dropped on a beach) sink by Bekker and get a resistance force
  while moving through the field (the snow bow wave's ploughing term, sand values).
- Collision stays the undisturbed collider plus sinkage, as now, so nothing new to trace.

**Built (16 Nov 2026).** `modules/ground/ground_terramechanics.*` holds the soil maths (Bekker static
sinkage with Wong's elastic-tyre mode, Lyasko slip sinkage, compaction and bulldozing for the new depth only,
Janosi-Hanamoto traction); `GroundSurface3D.get_soil()` reads the material lattice. Each cell now carries
shear values (c, phi, K, gamma; Wong table 2.2) and its own pressure-sinkage for tyres: Bekker's values depend
on the plate size they were measured with, and damp sand under a tyre bears as packed LETE sand (n 0.79,
kc 102, kphi 5301), while the boot values (and so the prints' look) stay as they were.
- **Vehicles** (`PhysXVehicle3D.soft_soil`, on; `PhysXVehicleWheel3D.tire_pressure`, 200 kPa): two
  components in the PxVehicle2 sequence. After the road query, each wheel's road plane drops by its sinkage
  (settling over 0.1 s; slip sinkage builds over a contact length of travel, so a wheel spinning in place
  digs in over seconds), the rut is pressed along the travel, and the tyre's friction is set to the soil's
  shear limit and its longitudinal stiffness to the Janosi-Hanamoto initial slope. In each substep the
  motion resistance acts at the axle against the travel (no wheel torque: a towed wheel's soil reaction
  passes through its axle). Not built as a force clamp: PhysX's wheel integration rings at walking pace
  (its own note on `PxVehicleTireSlipParams`), so on soft soil the active slip floor is 3 m/s, the
  stiffness is kept within what a substep integrates stably, and slip is averaged over the substeps.
  Diagnostics: `get_wheel_sinkage/slip/soil_resistance/soil_traction`, `is_wheel_on_soft_soil`.
- **Walkers**: `get_footing_at(position, mass, area)` returns effort (Lejeune 1998 fit to print depth) and
  a slide acceleration on faces past repose less 3 degrees (Pouliquen 1999); the game's controller uses it.
- **Rigid bodies** in `deform_groups` plough (`ploughing`, on): compaction and bulldozing at their width.
- `--test=soil` (17 checks, GPU and CPU paths): rigid wheel 7.7 cm in dry sand (Bekker 7.7), Rc 874 N
  (Bekker 874), 50 kPa tyre 4.5 cm, slip sinkage x2.0 at 50%, Janosi share 0.37 at 10% and 0.93 at 80%,
  second pass in the rut 0 N, walking effort 2.11 dry and 1.30 damp, 3.9 m/s2 slide on a 50 degree face.
  A 1500 kg 4x4 at half throttle for 4 s: 20.5 m firm, 5.3 m damp sand (2 cm ruts), 0.3 m dry sand at
  200 kPa (12 cm), 1.2 m aired down to 60 kPa (6.4 cm); floored on dry sand the wheels spin (slip 0.94) and
  dig to 13 cm. A 300 kg stone block shoved at 3 m/s stops in 1.15 m against 1.41 m.
- Known: motion resistance follows Bekker (damp sand 0.13 to 0.23 of the load, dry 0.5 at road pressure),
  inside Wong's 0.15 to 0.3 for sand except loose dry sand at road pressure, where cars do bog. Floored on
  damp sand the wheels dig in too, which real packed sand likely resists better. Light crates barely sink,
  so they barely plough.
- `--view=ruts` renders a 4x4's S-curve through the ground; the PT frame matches raster detail. Found while
  building it and fixed: with `RenderingServer.render_loop_enabled` off the device advances no frames, so
  no async read back lands, and the field queued one each step until the device's download staging blocks
  overran (a crash, ground and snow alike). Now one read back of each kind is in flight at a time; a check
  in `--test` runs the loop off for 2 s.

### Stage 2: grains that move (hybrid height field and MPM, Holz 2009)

- Where a body cuts faster than the slump can follow (a spinning wheel, a kick, a shovel, a pit
  edge failing), the field gives up a column of sand to a local `PhysXGranular3D` domain that
  follows the action: mass removed from the field equals mass spawned, at the field's moisture
  (dry: friction 33, cohesion 0; damp: cohesion from Su 2023's saturation curve).
- The MPM steps (MLS-MPM, Drucker-Prager, as already built); particles at rest for a while, or
  leaving the domain on the ground, deposit back into the field (mass to the column they land on,
  the field then slumps). Sand stays conserved end to end.
- Drawing: real geometry, as Paulo prefers. Near the camera each particle is a small clump mesh
  (several grains), not a sphere; the spray's fine fraction as a thin sheet of point grains.
  Both are instances the path tracer traces. Dust (silt under 0.06 mm) as Flow density, traced
  in the PT volume path the Flow work built.
- Budget: one to four domains, a fixed particle budget (for example 50,000), oldest
  deposited first. Off by default until its GPU cost is measured on the PC.

**Built** (`PhysXGroundGrains3D`, extends `PhysXGranular3D`; nothing throws sand without one in the scene):

- The field gives sand up and takes it back: `GroundSurface3D.dig` (rim first, then dent) and `deposit`
  (hollow first, then rim), on the CPU and as a GPU stamp. The depth is scaled by the disc's antialiased
  cover summed over the cells, so the volume moved is the disc's exactly, even for a disc smaller than a
  cell (a landed grain).
- Sprays: on soft soil a `PhysXVehicle3D` wheel turning more than 0.5 m/s faster than it travels reports
  a spray (`take_sprays`): from the rear of its contact, at up to 8 m/s of slip, 0.7 of it backwards and
  0.45 up (about 30 degrees; sand slides on the rubber). The grains node digs `tread_depth` (8 mm) per metre
  of slip (to 10 m/s) over the sand the tread reaches (contact plus 10 cm, the loose rim first, so a
  spinning wheel throws its bow wave and berm back instead of heaping them round itself), and throws it,
  spread along the step's path as particle emitters do. `kick(position, radius, depth, velocity)` for
  scripts (a shovel, a foot, a blast).
- Grains: one MPM particle per 3 cl (an 8 x 4 x 10 m box at 6.25 cm cells; it follows `follow_target` and
  reaches 3 m the way the sprays head). Thrown grains fly ballistic: the MPM's settling damping (0.992 a
  substep, there for piles that creep) fades out above 1 m/s for this node (`granular_free_flight`); it
  steps on the physics tick, so the landing checks keep pace with the bodies throwing them. A grain lands
  once it has been seen leaving its emit point and has risen a grain or flown 0.25 s, at the field's
  surface; landings are merged per 6.25 cm cell and deposited as one disc each, and the slot given back
  (`MPMFluidSolver::kill`, dead slots skipped in every pass). One leaving the box flies on ballistically to
  where it comes down.
- Drawing: thrown sand dilates to a few percent solids and its fine grains make a roost nearly opaque,
  which no count of real-size grains can draw. Each particle is a loose cloud of 56 specks of 2.5 to 6 mm
  over about 12 cm (jittered octahedra, real geometry the path tracer traces), each instance turned and
  sized by its slot and tumbling as it travels (in the MPM render pass).
- `--test=grains` (10 checks, GPU and CPU field): a kick digs its disc (3.53 l) and the field drops by it;
  115 grains fly; dug = in the air + landed to 0.0000 l every step; everything lands; landed 3.51 of 3.53 l
  (the rest is under one grain) and the field net -0.025 l; it lands at 1.25 m against 1.22 m for a
  drag-free throw. A 1500 kg 4x4 floored on dry sand throws 54 l in 2 s, behind it, volume kept.
- `--view=roost` (`--cam=`): the 4x4 floored from rest on dry sand, the frame mid-spray.
- Found while building it: grains were thrown from below the rut floor (the contact is already at the
  sunken road plane) and counted as landed at once; with the MPM stepped per frame they lagged the physics
  and heaped up behind the wheels; grains leaving the box were dropped at its edge (walls of sand).
- Known: the PhysX wheels spin up to 40+ m/s on loose sand at full throttle (no rev limit in the bench car);
  the throw caps at 8 m/s, the volume at 10 m/s of slip. The MPM is a continuum: a dense sheet behaves as
  one, which is what a roost is near the tyre; further out it is a ballistic cloud. Dust (silt) as Flow
  density is not built.

### Stage 3: wind on sand (ripples, streamers, filling)

- **Ripples** in the near field as a separate ripple height layer on the GPU field at a finer
  spacing (2 cm over the middle 8 m), grown by Nishimori-Ouchi saltation and reptation from the
  weather map's wind when it is over Bagnold's threshold; prints fill and ripples re-form
  across them; a crossing ripple field rotates with a wind shift. Real geometry: the patch's
  middle gains a finer ring so the path tracer sees the crests (ripples are 0.5 to 1 cm; the
  6.25 cm grid cannot hold them). Off when wet (cohesion stops saltation).
- **Streamers**: saltating sand as a low sheet of grains over the surface above threshold,
  thickest over crests, from the same flux (GPU particles like the snow fall's, traced).
- **Dunes** are a terrain-generation job (Paris 2019 / Nilles 2024 on the terrain generator's
  heights), passed to the terrain thread through the coordinator, not built here.

**Built: ripples (16 Nov 2026).** `modules/ground/ground_ripples.*`, driven by `GroundSurface3D`
(`wind_ripples`, off by default until its cost is measured on the PC):

- Nishimori and Ouchi's 1993 lattice: each step lifts a saltation load from every cell and drops it a hop
  downwind, the hop growing with the cell's height over the mean (L = L0 + b hs tanh(h / hs); L0 8 cm, b 3,
  hs 3 mm), so crests throw further and the pattern selects its own wavelength; reptation creeps a share of
  each landing on (scaled by the step's flux), then the slopes relax. 1.25 cm cells over `ripple_extent`
  (4 m, 320 squared, by default; 8 m is four times the vertices). At 8 m/s it settles near 11 cm and 6 to 7 mm crest to trough, crests across the wind,
  within the 5 to 15 cm and 0.5 to 1 cm of field ripples.
- Flux from the weather map's 10 m wind (`wind_override` for scenes): a log-law friction speed (z0 0.1 mm),
  Bagnold's threshold 0.2 m/s, Lettau and Lettau's flux. Nothing moves under the threshold. Real time is
  slow (a few steps in 10 s), so a field grown on a worker (500 steps, about 2 s) fills cells entering the
  window, regrown when the wind turns past 35 degrees; `ripple_time_scale` speeds the rest.
- Only dry, bare sand moves (`Cell.blown`: sand times dryness, damp grains hold from 5% to 20% moisture,
  Belly 1964; litter stops it). Damp sand keeps the ripples it has: they show wherever there is sand. The same share now sets how fast wind fills prints on the GPU and CPU paths
  (it read a roughness factor before, so damp sand drifted as dry sand did).
- A print, a rut or landed sand wipes the ripples on it (the field's surface changing by over 1 mm), and the
  wind grows them back over it; the layer's mean holds, so no sand is made.
- Shading: the ripple map (`ground_ripple_map`, height and sand share) is a global the ground patch reads in
  its vertex stage; the patch is meshed over the window at the ripple cell, so the crests are real geometry and
  the path tracer, which reruns the vertex stage, sees them. They fade out over the window's outer 15%.
- Limits: the profile is near symmetric (field ripples have a gentle stoss and a steeper lee); Anderson's
  1987 impact erosion would skew it but killed the instability here. A slight upwind drift of the pattern.
- Far ripples: past the window the grown field (periodic, so it tiles) shades the rest of the patch as a
  slope on the normal where there is sand, fading in as the window's geometry fades out. The raster reads
  a mipmapped copy with anisotropic filtering (far ripples average out instead of shimmering); the path
  tracer reads the ripple map itself (rt param 68), which carries the sand share over the patch and its
  rects under the window's rows. They cast no shadows of their own, unlike the window's geometry.

**Built: streamers (16 Nov 2026).** `GroundStreamers3D` (nothing blows without one in the scene):

- The saltating grains as GPU particles in a square round the camera, traced like the snow's. The amount
  follows the ripples' flux: the sand in the air over a square metre is Q over the grains' speed (5 g at
  12 m/s), and each particle stands for as many 0.25 mm grains as that needs (a few hundred), drawn with
  their summed cross-section (a sub-pixel streak keeps its light, as the snow's do).
- Hops: exponential heights with a 4 cm mean, about the same at any wind (Namikas 2003; Kok et al. 2012),
  at 80% of the threshold's log-law wind at the hop's top (2.4 m/s), so a hop runs about 11 times its height.
- Streamers (Baas and Sherman 2005): ribbons about 20 cm wide and 2 to 3 m long, carried downwind at the
  20 cm wind and turning over in a couple of seconds; 15% of the ground at the threshold, a continuous sheet
  at twice the threshold friction speed (Stout and Zobeck 1997 for the intermittency). The particles fly
  only inside them, at the density that keeps the mean flux.
- Over damp sand, litter, soil or rock nothing flies (the same `blown` share).

### Stage 4: sand and water (Su et al. 2023)

The sand layer and water inside it from Su 2023 on the near field: swash smoothing prints,
seepage, wet walls holding, runoff carving channels in a beach. Needs the shore water from
Valence's water and the contact wetness; built only with that thread, through the coordinator.

**Plan (16 Nov 2026).** The water stays the water module's (its surfaces, rivers and live water); the
sand reads it and answers on its own field, so the coupling is one way, water to sand, which is what
shows at a beach (sand changing under water; the water barely notices a footprint). From Su 2023 we take
the sand side: water held in the sand and the cohesion it gives; from coastal and river engineering the
sediment moved by flowing water.

- **Pore water** per field texel: saturation S of the top layer (porosity 0.4). Filled by water over
  the sand (a surface's `sample_depth` above zero, a river's or live water's depth, the swash film from
  `sample_swash`) and rain (`get_wetness_at`); drains down by gravity (Darcy, fine sand k about
  1e-4 m/s, so a wet layer drains in minutes) and spreads sideways in the sand by diffusion (seepage),
  dries by evaporation at the weather's rate. The texel's moisture is S, not the surface's
  single `moisture`, where water has been.
- **Cohesion from S** (Su 2023's water stages: dry; pendular to 20%; funicular to 25%; capillary to 33%;
  past it saturated): repose and slumping rate follow it, so damp sand holds a cut wall, dry and
  saturated sand do not; saturated sand under a boot squelches (pore pressure: Bekker's kphi falls,
  sinkage rises, the print's walls slump back).
- **Sediment moved by water** (the Exner equation on the field's dent and rim layers): bed load from
  the water's speed over the sand, Meyer-Peter and Mueller 1948, q = 8 sqrt((s - 1) g d^3)
  (theta - theta_c)^1.5 with the Shields number theta from the flow's shear (a drag coefficient of
  about 0.003 on the depth-averaged speed) and theta_c 0.047; carried along the flow, so a print under
  the swash is rounded off and filled within a few waves, and runoff over a beach cuts small channels
  and fans where it slows. Turned sand under water shows darker (wet) and its glints are gone.
- Checks: a wet wall stands where a dry one slumps to 33 degrees; saturation drains from full to the
  pendular state in minutes; a print under a 0.5 m/s swash loses most of its depth in under a minute and
  no sand is made or lost (Exner keeps the volume); no sediment moves under the Shields threshold.

**Built (16 Nov 2026).** `GroundSurface3D.water_coupling` (on; 0.3 ms twice a second) and `water_sources`
(the water nodes it reads: a PhysXWaterSurface3D's `sample_swash` film and `sample_depth`, a river's or live
water's `sample_depth`, all their `sample_flow`; any node with those calls), plus the weather map's contact
wetness:

- Pore water per lattice cell (25 cm), sampled from the water on a 1 m grid: Green-Ampt infiltration under
  water (saturated in about 10 s), gravity drainage towards field capacity (15%, e-folding in 80 s), then
  evaporation (hours); never drier than the contact wetness says. Sideways seepage is left out: at fine
  sand's diffusivity (about 2e-5 m2/s) it moves nothing across a 25 cm cell in minutes.
- The cell's rules take the pore water as their moisture, so damp sand is firm and holds steep walls; past
  80 to 95% the water bridges are gone and the walls fall back to dry sand's 33 degrees (Su 2023's
  capillary and saturated states).
- Under water faster than the Shields threshold (0.25 m/s depth-averaged for 0.25 mm sand) the cell slumps
  towards a near-flat bed at the Exner relaxation rate from Meyer-Peter and Mueller bed load: a 3 cm print
  under 0.6 m/s is down to 7 mm in a minute; under 0.15 m/s it keeps its walls. The swash's run-up speed is
  taken as twice the shallow-water speed of its film. Slumping keeps the volume.
- Not built: channels cut by runoff (they need the bed load carried along the flow, an advection pass on
  the field, not only relaxation).
- The look follows the pore water (Lagarde 2013, "water drop 3b", porous surfaces): the ground publishes
  `ground_wet_map` (per lattice cell: wetness = smoothstep(0.03, 0.4, S); sheen = 0.35 smoothstep(0.95, 1, S) for
  the film standing on saturated sand; under water the water's own surface shows). The patch darkens albedo by
  mix(1, 0.2, porosity) and roughness by half that, porosity 0.6 when the material has no wetness of its
  own (else its own, taking the wetter of the two so nothing is wetted twice); the sheen pulls roughness to
  0.05. Raster and path tracer read the same map (rt param 69), so damp sand dries visibly back from the
  swash line as it drains to field capacity.

### Look (any stage)

- Sand glints: the snow glint lobe (`snow_glint()`) with quartz grains (0.25 mm, share lower
  than snow's) and wet sand killing them, like snow.
- Turned sand darker (already), plus Journey's sharper specular on wind-smoothed faces.

## Cost and checks

- Stage 0: same checks as the ground and snow GPU paths (volume to the gram, slump angles, CPU
  and GPU within a millimetre); main-thread time in the bench.
- Stage 1: a 4x4 on dry sand at full throttle bogs to under 2 m/s on a 15 degree dune and
  climbs it on damp sand; sinkage against Wong's tables for a given wheel load; a wheel at 50% slip
  digs in further than one at 10%; drawbar pull against slip follows the Janosi curve.
- Stage 2: mass out of the field equals particles spawned equals mass back; a dumped pile
  stands at the material's repose angle; GPU cost for 50,000 particles on the PC.
- Stage 3: ripple wavelength 5 to 15 cm at 8 m/s wind, crests normal to the wind, none under
  threshold or when wet; prints filled within the bench's expected time.
- Every stage: a path-traced frame before merge, raster against PT at 100% crop.

## For the PC (when it is back)

GPU timings for the ground field, the MPM domains and ripples; denoised PT shots of a truck
digging in, a kicked spray, a ripple field at low sun.

### Dune grass (sand trapping by plants)

Built for the vegetation module's dune grass. Plants take a share of the wind's stress: Raupach's (1993) drag
partition gives the friction speed left on the sand, u*s/u* = 1 / sqrt((1 - m sigma lambda)(1 + m beta lambda)),
m 0.5, sigma 1, beta 150 (grass, Wolfe and Nickling 1996), lambda the frontal area index from `wind_shelters`
(VegetationField3D answers `sample_lateral_cover`, clamped at 0.5). At marram's 0.3 the surface keeps 0.22 of
u*, under the threshold in any gale. The flux each cell can carry is the Lettau and Lettau flux at that u*
times its blown share; the flux carried relaxes to it over the saturation length (1 m, Andreotti et al. 2010),
swept downwind cell by cell, and where it falls the sand lands (Exner, dh/dt = -dQ/dx / 1600 kg/m3), written
to the field in 2 mm steps. A 12 m/s wind piles about 2 cm an hour on a tuft's windward edge: the nebkha.
Sheltered sand also loses its ripples' and streamers' share and keeps its prints. Reads for vegetation:
`get_sand_flux_at`, `get_deposition_rate_at`, `get_sand_deposit_at`. Bench `--test=shelter`. Not modelled:
the lee eddy's scour and the wake's reduced flux past the plants (Okin 2008); the sand dropped on damp sand
downwind of dry sand comes from the same rule.
