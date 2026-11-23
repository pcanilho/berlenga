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
  A 1500 kg 4x4 at half throttle for 4 s: 20.5 m firm, 8.0 m damp sand (2 cm ruts), 0.5 m dry sand at
  200 kPa (12 cm), 8.3 m aired down to 60 kPa (7 cm); floored on dry sand the wheels spin (slip 0.93) and
  dig to 15 cm.
- **Wheel bulldozing only past D/6 (16 Nov).** Bulldozing was applied at every depth, on top of compaction,
  which the plan above never meant ("once sinkage passes a fraction of the radius"): a rear-drive 800 kg
  buggy on 55 kPa tyres met 0.38 of its weight in dry-sand resistance before slip and crawled at 0.15 m/s at
  any torque. Bekker's compaction theory holds to sinkage D/6; within it the field moves the displaced sand
  to the rims (SCM has no bulldozing force at all) and bulldozing is marginal at small sinkage. Wheels now
  bulldoze only the depth past D/6 (`wheel_bulldozing_resistance`); bodies ploughing keep the full term.
  Buggy, full throttle on dry sand: 40 kPa 12 m/s after 9.5 s, 55 kPa about 0.6 m/s, 200 kPa bogged. A 300 kg stone block shoved at 3 m/s stops in 1.15 m against 1.41 m.
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

### Sand glints and grain (built 16 Nov 2026)

`sand_glints` (on by default). Up close, loose dry sand shows its grains: a 0.25 mm grain
grid counted per pixel footprint as the snow's sparkle is (Zirr and Kaplanyan 2016; Deliot and Belcour 2023,
one facet per cell): grain colours vary (sum of two uniforms, std 0.41) and average out as one over the square
root of the grains a cell holds, blended across levels so it never steps; 12% of grains are clean faces
(quartz, mica) GGX-tilted (alpha 0.5) that flash as a coat lobe at quartz F0 0.045, roughness 0.45. The share
is published per lattice cell in the wet map's B channel (sand, not litter, deformable); wetness puts the
glints out. The path tracer applies the same functions on camera rays (pixel footprint) and others (ray cone).
The standing film on saturated sand now also hides the sand's relief: the normal map's depth falls by 85% and
roughness goes to 0.2 at 60% (Lagarde's accumulated water), raster and path traced.
A second, coarser layer (1.2 mm, beach shell grit) joined the 0.25 mm one so grains read from 30 to 50 cm
(65c4727c); bench `--view=grains`.

### Runoff channels (built 16 Nov 2026)

`runoff_channels` (on by default). The rills a beach grows at low tide and the gullies
rain cuts in soft ground. Research: beach rills are seepage erosion below the water table's exit point, which a
falling tide leaves behind (Turner 1993; Horn 2002 on beach groundwater); channelisation of an erodible bed by
sheet flow is a flow-concentration instability (Smith and Bretherton 1972; Schorghofer et al. 2004, rhomboid
and rill patterns); landscape models route that flow with multiple flow directions (Freeman 1991; Quinn 1991)
and move sediment transport-limited with an Exner balance (Tucker and Hancock 2010).

- Water table: per lattice cell, its depth under the surface. A sea or lake (water deeper than 10 cm) sets it at
  its own level (a head above the sand); once exposed it falls about 1e-5 m/s (Boussinesq drainage, k 1e-4 m/s,
  2 m aquifer, porosity 0.3, 6 m face), so the lower beach keeps seeping for hours after the tide. The 15 cm
  capillary fringe keeps the sand above a high table soaked (the wet band of a real beach at low tide).
- Sources: seepage k sin(beta) where the table is within 2 cm of the surface; rain where saturated, else its
  excess over the ground's k (1e-6 m/s soil to 1e-4 m/s sand).
- Routing: a grid of twice the field's texel (12.5 cm) over the field, cells in falling height order, each
  passing water and sand to every lower neighbour weighted by slope^1.1. Standing water and hollows end the flow.
- Flow: Manning sheet flow, n 0.025, h = (n q / sqrt(S))^0.6; shear rho g h S.
- Sand: Meyer-Peter and Mueller bed load at that shear (`bed_load_shear`) is the capacity; transport-limited
  Exner, porosity 0.4, to the field in 1 mm steps. The field's slumping keeps rill walls at repose.

Bench `--test=runoff` (beach 1 in 12, tide from 0.6 to 0.1 m, time x120): seepage sheets of 0.3 mm at 5 cm/s,
rills cut 9 mm (99th percentile) against a 2 mm median in 84 minutes, about 100 l laid at the foot of the face
above the waterline, net volume within 1%, 10 ms a step on the CPU (twice a second), 0.5 ms with no water.
`--view=rills` (--runoff=0 for the A/B). Not modelled yet: headward sapping at rill heads, the rills' braided
rhomboid fans, a finer near field (rills are 2 grid cells wide at 12.5 cm), water drawn in the rills.

### Sharp rills on the near field (research, 17 Nov 2026)

Docs declined the rills pair (soft stripes); the mud thread's near field (8 m at 1.56 cm) now draws detail fine
enough for real rills, but routing runoff on it with the steady-state scheme failed three ways (patches kept outside
the repo): multiple flow direction with a slope-grown exponent (Qin et al. 2007, MFD-md) gives ruled stripes along
the grid's eight directions on an even face; adding microtopography noise gives pockmarks (water stops in every
hollow and drops its sand); filling hollows first (priority flood, Barnes et al. 2014) gives runaway mounds (the
filled flats keep taking sand). MFD-md alone does make `--test=runoff` robust (median cut 0.09 of the 99th
percentile, was 0.25 on the threshold).

What the literature does instead:
- RillGrow (Favis-Mortlock 1998, 2000; Favis-Mortlock et al., Earth Surf. Process. Landforms): rills are
  self-organising. Water moves cell to cell over a microtopography DEM by the WATER SURFACE (ground plus depth), so
  ponds fill and spill instead of being skipped or filled ahead of time; detachment from stream power above a
  threshold, deposition where transport capacity drops. Rill networks emerge from the random roughness and the
  feedback of flow onto its own cuts, with no grid-direction bias because flow follows the water surface and the
  roughness, not one steady pass.
- Virtual pipes (Mei, Decaudin and Hu 2007; St'ava et al. 2008; layered heightmaps, VMV 2024): explicit shallow
  water on the heightfield, outflow per pipe from the water-surface difference, erosion and deposition toward a
  sediment capacity C = Kc sin(a) |v|, sediment advected semi-Lagrangian. Real time on the GPU at 512^2.

Plan (needs a card before building, it replaces the runoff routing on the near field):
1. Near-field runoff as virtual-pipe shallow water on the near grid (256^2 at 3 cm, GPU compute next to the
   deform pass; the coarse grid stays steady-state MFD-md and feeds the edge), sources unchanged (seepage, rain).
2. Microtopography from the sand's own roughness (a few mm, fixed per place) so channels start irregularly.
3. Capacity erosion and deposition (Mei 2007) with the Shields threshold already in `bed_load_shear`; deposits
   where flow slackens, fans at the foot.
4. Checks: `--test=runoff` (channels, mass kept, fans), a new rill spacing check (tens of cm on a 1 in 12 face,
   field observations of beach rills), cost under 2 ms GPU; `--view=rills` raster and path-traced pair.

### Water-sim rills on the near field (built 17 Nov 2026)

Paulo picked the water-sim rills. On the GPU near field (8 m at 1.56 cm) the runoff is now the standing water's
own shallow water with sediment (`ground_deform.glsl`, modes WATER_ERODE and WATER_SED after each water substep);
the CPU steady-state routing stays for CPU-only runs.
- Water: local inertial shallow water (Bates, Horritt and Fewtrell 2010) with semi-implicit Manning friction
  (n 0.025) per pipe, replacing the damped pipe model while runoff is on; velocity from the face fluxes, held to
  Froude 2. Sources: the seepage face (Darcy k sin b below the falling table, Turner 1993), rain; the sea and lakes
  are sinks.
- Microtopography: fixed value noise (3 cm, 2 mm) on the water's bed only, so the flow starts irregularly
  (RillGrow, Favis-Mortlock 1998).
- Bed load: Meyer-Peter and Mueller at the Manning bed shear rho g n^2 u^2 / h^(1/3), turned downhill by 1.5 times
  the bed slope (Bagnold 1966; Koch and Flokstra 1981). Taken from the flow round each texel (1-2-1 weights):
  per texel, sand stranded wherever the film thinned and grew mounds with scour pits beside them.
- Bed: Exner in flux form with upwind faces (porosity 0.4), so the sand is kept exactly; sped up by the
  morphological factor (Roelvink 2006) `runoff_time_scale`. The transport is held to the bed's own wave
  (3 q / (0.6 h)) crossing half a texel a substep, past which the explicit update breaks into chains of pits; the
  factor is split over substeps of at most 40 (up to 32).
- The cut goes into the field's dent and rim, so it draws, collides and slumps to repose like any print.

Result (`--test=runoff`, time x120): a braided, diamond-shaped net of rills down the face, 4 mm at the 99th
percentile against a zero median, sand laid at the foot, net change under 5% of what moved. The net is the
rhomboid rill pattern of a draining beach face (Woodford 1935; Hoyt and Henry 1963), not the straight branching
seepage rills of a steeper face. The run's depth does not grow with the time scale, since the table also drains
on it. `--view=rills` now runs at x120 from a higher camera. Left: rill spacing check, water drawn in the rills.

Docs round 1 (declined: dotted rills, a parallel comb, no water, bench background). Fixes:
- Relief: the bed's fixed relief adds the swash's low ridges and hollows (15 and 50 cm, 3 and 4 mm) to the 3 cm
  grain roughness, so the seepage gathers into fewer rills that join and split downslope (RillGrow's
  microtopography at the scale beach rills space at).
- Floors: a separate, larger bed-slope weight along the flow (3 on top of the 1.5 across, as morphodynamic models
  keep streamwise and transverse factors apart) planes bumps off channel floors; 8 brought back cross ridges.
- Water: the ground drew water only past 0.3 mm, but the sheet is 0.2 mm (3 mm in the rills); a film now glistens
  from 0.02 mm (smooth surface, Lagarde 2013's accumulated water) and covers the grains past 0.3 mm as before.
- `--view=rills` looks down the face to the waterline. The site frame belongs to the scene's real beach.
Left: faint ridges across some floors (the explicit scheme's two-texel mode; de Almeida et al. 2012's flux
weighting would damp it but needs the neighbours' fluxes double-buffered). Fixed 18 Nov: a backlit close-up showed
it worse than ridges, a ladder of one-texel rungs along every rill (odd and even texels' discharges decoupled, the
bed cut to match), drawn as stepped islands in the glistening sheet. The depth pass now keeps each face's signed
discharge (the shift buffer, extended) and the flux pass starts each face from its own mixed with the two parallel
faces' either side, theta 0.8 (de Almeida, Bates, Freer and Souvignet 2012). Second difference of the depth along
the face down 0.90 to 0.57 of the first; the deepest cut 17.7 to 7.8 mm (the rungs had deepened it), the runoff
test passes. Two shading fixes found on the way, both renderers: the standing film is classified from the pore
saturation after filtering, not before (its edge was rounded squares a lattice step across), and standing water's
depth between texels is the interpolated level less the bed, dry texels given their wet neighbours' mean level, so
the waterline runs between texels instead of round whole dry ones.

Docs passed round 2, with a note: in raster the net partly reads as raised ridges with bright crests. Checked
on the cut: the rills' banks sit 0.1 mm below the face on average, not above it; under the low side sun each
groove's far wall is lit, which reads as a crest. One real cause fixed with it: the water ran over the fixed relief
and filled its hollows, but the relief was not drawn, so filled hollows showed as raised patches (30% of the face
0.5 to 7 mm up). The blit now draws the relief with the cut while runoff is on.

### Next sand candidates (research, 17 Nov 2026; need a card)

- Swash marks: the thin arcuate ridges of fine sand, foam and debris a swash leaves at its uprush limit, a few
  mm high and cm wide, one per wave, overlapping in scallops (Komar 1998, Beach Processes and Sedimentation;
  Reineck and Singh 1980). Built from the shore water's swash film: where the film stops and turns, lay a thin
  ridge (deposit from the film's last load) and a foam-tinted line that dries pale. Small, high payoff in shore
  frames.
- Rill heads and fans: beach rills start at a sharp head where seepage saps the face (headward sapping, Higgins
  1984; Dunne 1990) and end in small deltas at the waterline, lobes that spread and split. Needs the head's
  steepening (a cut wall over repose slumps into the flow) and the fan's lobe switching (deposits blocking
  their own channel). Medium; makes the network read as rills from far.
- Flowing water in the rills: draw the seepage as moving, with flow-mapped normals and sparkle advected by the
  shallow water's own velocity (Vlachos 2010, flow maps; the velocity is already on the GPU). Small to medium;
  the water now only glistens.
- Plain fix, no card: the faint two-texel ridges across some rill floors; de Almeida, Bates et al. 2012 weight
  each face's discharge with its neighbours' (theta 0.7 to 0.9), which needs the fluxes double-buffered.

### Swash marks (Paulo picked all three candidates, 17 Nov 2026)

Built first of the three. `swash_marks` (on): ten times a second the near field is cut into shore-normal transects
10 cm apart (the beach's downhill direction there); along each, the water's landward edge (swash film or sea over
1 mm, from the water sources' `sample_swash` and `sample_depth`) is found to 6 mm. When a swash that was climbing
falls back 5 cm from its highest reach, that reach is the uprush limit (Komar 1998): a ridge of sand is laid along
it, 4 mm high and about 2.5 cm wide (two near-field texels; a narrower one is lost between texels), joined halfway
to the neighbouring transects' marks of the same wave (within 25 cm) so each wave draws one line. The
next swash that reaches higher smooths it as it does prints; the highest lines stay. About 0.7 ms a step on the
CPU.
Docs round 1: one smooth mark read as a pencil stroke, and its long curve as a ring. The film is followed only past
1 mm and smoothly along the beach, but a swash's thin leading edge breaks into tongues: lobes convex landward meeting
in seaward-pointing cusps (Reineck and Singh 1980). Each wave now draws its own lobes, 0.5 to 1.2 m long and 4 to
10 cm beyond the followed reach. `--ebb=from,to` lowers the sea's `swash_run_up` over the run, as a falling tide, so
later waves leave their marks below the earlier ones and the beach keeps a ladder of lines. The lip is the swash's
finest load (mica, heavy minerals, bits of debris), so it is laid with the fresh tint and reads darker than the sand
round it until it dries (`drying_time`); deposits carry that tint only when asked (`GroundDeformField::amount_fresh`).
Docs round 2 (raster lips too faint) and 3 (lines crossing, beads): the shadow map cannot shade millimetre
relief, so the raster patch takes the curvature of the field's own heights a texel and a half round (the depth
buffer's unsharp mask, Luft, Colditz and Deussen 2006; raster only, as SSAO): crests lighten, feet darken; the path
tracer shadows them for real. The reach a mark is laid at is averaged over a metre either side (the sea's film is
solved on cells of tens of centimetres: its edge's finer wiggles are the grid's), so lines run along the shore with
gentle 0.8 to 1.3 m scallops; a new line flattens any older one on its transect it reached past, so lines never
cross; and each stretch between two marks is laid once, by the second to mark (a doubled join was a bead). Bench:
`--prints=0`, `--sea=name:value` (`shore_wave_curl:0` for path-traced shore frames until the lips have a PT look).
`--view=shore` prints the count; `SWASH_DUMP=file.f32` writes the ground's change over x -4..4, z -1..5 at 1 cm.
Docs rounds 4 and 5 (short hooked scraps far off, near lines too straight and doubled): marks are now laid per
wave, not per transect. A wave is the mean reach over the stretch rising and falling back 8 cm; its line is each
transect's highest reach that wave, tent-averaged 1.5 m either side, plus scallops 0.8 to 1.5 m long and 18 to 30 cm
deep (a sine raised to 0.6, so the cusps stay sharp). An older line within 10 cm landward of a new one, or seaward
of it, is flattened there (one reach draws one ridge), and what is left of a line under a metre long goes with it.
Validity is kept per stretch between transects, so flattening one never orphans a 10 cm dash beside it.
The PT frame of that round lost the lines: the lip's darker tint was the fresh tint, which dries away in minutes,
so by the frame only the raster's micro-occlusion drew them. The lip's dark grains are its material (heavy minerals
and mica, Komar 1998), not its moisture: it is now laid as a band of another sand (`amount_fresh` from 2: 2 plus its
share, kept, albedo down to 0.6, no sheen), which both renderers draw.
Docs round 6 (lines crossing, ghosts, square cusps, scraps): each lobe is now a parabolic arc meeting the next at
a cusp of about 40 degrees (a sine to a power under 1 met at right angles and read as a stencil); a line keeps
within 25 cm of its wave's mean reach (the sea's film drifted a metre along the stretch and drew diagonals); an
older line within 15 cm of a new one is flattened; runs under a metre are not laid. A line being washed out
(`amount_fresh` -2) takes its rim and levels the hollow beside it over 12 cm: saturated sand had slumped it wider
than it was laid, and a control run with `swash_marks` off showed the leftover shapes were the lines'. Lines also
wash out under any water fast enough to move sand (`bed_load()`), not only a film over 1 mm. A wave washes out older lines
only inside the line it will lay (the film's tongues past it cut gaps where nothing newer was laid), and kept lines
within reach of a flattening dig are laid again. A lobe past the near field's edge ends the line there instead of
leaving dashes; the shore bench uses a 12 m near field, since the run-up climbs about 3 m and the default field's
sharp middle ends 3 m out.

### Ripple variation: forks, turning crests, patchy height (18 Nov 2026)

The seabed under the surf showed the wind ripples as even stripes: every place read the same 4 m grown tile with one
crest direction, and the near window none of the far variation. Research:
- Werner and Kocurek (1999, Geology, "Bedform spacing from defect dynamics") and later field counts (Frontiers in
  Earth Science 2020, ripple defect density): what makes a real ripple field read as natural is its defects, crest
  terminations and Y-junctions (a crest forks in two), whose density falls as the field matures; spacing and
  direction drift with them.
- Roos and Blondeaux (2001, JFM) and the "Sand ripples under sea waves" series: wave ripples under the sea form
  long crests with bifurcations and brick patterns, not a perfect grating.
- Pattern-formation theory (Cross and Hohenberg 1993): a defect in a stripe pattern is a phase singularity, the
  phase winding by 2 pi round it; a pair of opposite sign inserts a crest segment between them (fork and rejoin).
- Engines break repetition with hex-tiling / stochastic texturing (Mikkelsen 2022, JCGT 11(3); Heitz and Neyret
  2018). For a directional pattern its blends between rotated tiles cross crests (ghost gratings), so it is not used:
  a phase-coherent warp plus explicit dislocations keeps every crest continuous.
Built: GroundRipples::vary() and ground_ripple_vary() (material.cpp, raygen): a three-octave world warp (4 m at 24 m,
1.2 m at 6 m, 6 cm at 1.7 m: crests turn by up to about 15 degrees and change spacing over tens of metres, bend over
a few), dislocation pairs in half the 2 m cells, along the crests, 30 to 90 cm apart, shifting the tile by
exactly one wavelength of its dominant wave vector (found by a DFT of the grown tile, 3 to 40 cm band, sent in the
ripple map's last row), a stretch along the crests' normal (30 cm at 2.5 m: crests crowd and open by up to a fifth, 9 to 14 cm apart; stronger broke the near crests into grain-like noise in the desert bench), and a height between 35 and 100% over 2 to 6 m. Integer hashes, so the CPU fill of the real
ripples' window and both renderers agree to the bit and meet crest to crest. Far ripples fade where their crests come closer than about three pixels (prefiltered to the pixel, so they do not alias into moire bands). Crest height (19 Nov): the height-dependent hop saturated at 3 mm, so grown ripples stood 4 mm peak to peak (index about 30); real ones stand 5 to 10 mm at 9 m/s (index 15 to 20; Sharp 1963, Andreotti 2006). hop_saturation is now 5 mm: 7.3 mm at 12 cm (index 16), checked by --test=wind. Next if wanted: true wave ripples
(symmetric, shore-parallel) where the bed is under the sea.

### Rill heads and fans (Paulo's pick, 19 Nov 2026)

The net cut by the runoff solver started heads anywhere on the face and ended in a thin fringe at the waterline.
Real seepage rills on a sand beach start at heads near the top of the seepage face, join downslope and end in
small fans where they spread onto the flatter low-tide terrace (Otvos 1999, rain- and seepage-made beach forms;
Schorghofer et al. 2004 for seepage channels in sand).
- Heads: seepage erosion (Dunne 1990; Howard and McLane 1988). A cut lies below the saturated table round it, so
  ground water gathers into it through its walls (Darcy k c / l) on top of the face's own k sin(beta), and the
  ground round it loses as much: the cut smoothed over 3 cm against its mean on a ring 25 cm out, at most 2.5
  times the face's own outflow. Taken texel by texel (a 4.5 cm ring) it broke the face into pits. The water also
  lifts the grains it leaves through (seepage force, i = q / k against a critical gradient of about 1 for sand),
  lowering the threshold by up to 30% where the seepage gathers. Both in `ground_deform.glsl`
  (runoff_seep_gather, bed_load_shear_f), GPU near field only.
- Fans: Exner already lays sand where the transport falls off, but a face falling straight into the water has
  nowhere to drop it but the waterline. The bench beach now has a low-tide terrace (1 in 12 face over 1 in 50,
  eased over 40 cm: a reflective face over a dissipative terrace, Wright and Short 1984); `--terrace=0` keeps
  the plain face. Real beaches get the terrace from the terrain.
- Plain bug found on the way: with runoff on the GPU, standing water evaporated at the sped-up (morphological)
  rate while its sources landed at their own, so a terrace's thin seepage dried faster than it seeped and the
  rills' water vanished on reaching it. Evaporation now runs at the water's own pace there.
- Moving water: already drawn (flow-mapped ripples on the sheet swept by the Manning velocity, Vlachos 2010; built
  with the swash marks), both renderers.

Docs round 1 (partial): the rills read as an even comb with no trunks, walls stretched like streaks, a busy
even pattern on the sand round them, rows of round pits in the rill floors. Fixes:
- Trunks: a real face is not a plane. The bench face now carries beach cusps, horns and bays about 5 cm deep over
  2 to 3 m (Komar 1998; Coco et al. 1999); the bays gather the small heads into a few trunks that converge and end
  in arcuate deposit fronts. 1 cm of relief barely bent the comb, and a five-octave bed roughness or a longer run
  did not either: on a plane the solver keeps its own fastest-growing width, about 10 cm.
- Capture: once channels cut below the table, the ground between them stops seeping and each channel takes the
  outflow of the strip it drains (drain spacing, Hooghoudt 1940; Kirchner 1993's drainage competition): seepage
  goes as how far the bed (smoothed over a 5 x 5 texel box) lies under its mean on rings 25 and 60 cm out, over
  1 cm, 0 to 4 times the face's own, conserved on the mean.
- Pits: the capture taken texel by texel, or with sparse taps, dug chains of pits at the tap spacing (the bed's
  second difference along the rills doubled); the dense box keeps it at the plain solver's level.
- Busy pattern: the bed's fixed roughness was drawn on the coarse main field (6 cm texels) too, where its 3 cm
  octave aliased into an even pattern over the whole beach. Each octave now fades out under about 1.5 texels a
  wavelength.

Docs round 2 (partial): still a comb lower down, banks like steps with pixel-stair edges, a busy pattern beside
the rills, cusp bays not visible from the front camera. Fixes:
- The steps were a wave about 3 texels (4.7 cm) long down every rill in the water depth and, weaker, in the bed:
  the explicit shallow water scheme's short waves on steep thin flow, under the scales it resolves (it showed in
  every run's depth spectrum along the flow and never across it). The water's gloss read it as a ladder of bright
  rungs. The depth is drawn under a 1-2-1 filter (a quarter of a 3-texel wave left, nine tenths of a rill), which
  also rounds the rills' edges over a texel as a film's are; the water reads as a full mirror only from about
  0.8 mm, so the channels' deeper water shows brightest and the thin sheet beside them stays matte.
- Cusps: the bays now run down the face (the relief stretched about three times downslope) and are 7 cm deep,
  so they gather the heads into trunks all the way down. Regular cusps (|sin|^0.7 horns and bays every 2.5 m, 6 cm
  deep) were tried and gathered the whole seepage into a sheet a bay wide: no rills. `--cam2` adds a low side-on frame along the beach.

### Footstep avalanches on slip faces (Paulo's pick, 19 Nov 2026)

Dry sand on a dune's slip face rests between its stopping and starting angles (about 31 and 33 degrees,
hysteresis: Daerr and Douady 1999). A step on it pushes the surface layer past the starting angle and a grain
flow runs down the face as a tongue that widens downslope and ends in a lobe at the toe (McDonald and Anderson
1996; Kleinhans 2005; Breton et al. 2008 for the scar and lobe shapes). A cellular slump rule with the two angles
only crept a fraction of a millimetre a step (it is diffusive): the flow is an event instead.
- Trigger: a press (CPU press, press_box, press_wheel; GPU main sink stamps) on dry sand whose repose is a dune
  sand's (tan 0.58 to 0.73), sand share at least a half, on ground between 2 degrees under repose and 1.15 times
  its tangent. One flow per 40 cm per 4 s, at most 16 at once.
- Path: down the fall line in 4 cm steps until the slope falls under 0.85 of the stopping angle. The front runs
  at about 1 m/s; the tongue's two edges wander on their own (3 to 10 degrees a side), so no two tongues match.
- Sand: a surface layer 1.5 cm deep at the top thinning to 4 mm is scoured from the tongue and laid as the lobe
  over the last fifth of the path and a little beyond; the volume is kept. `slip_avalanches` (on).
- Bench: `--test=avalanche` (reach, nothing beside it, volume kept, a shallow scar, none on the stoss slope);
  `--view=avalanche` a line of steps across a 32 degree face (`dune` height kind).

### Wind fills prints from upwind (Paulo's pick, 19 Nov 2026)

Wind does not fade a print evenly. Its upwind wall shelters the hollow, so the saltation's shear falls there and
the load the wind brings lands; the rim facing the wind takes more shear than the plain and wears. With the flux
lagging the shear by its saturation length, about a metre, longer than a print (Sauermann, Kroy and Herrmann
2001; Andreotti, Claudin and Douady 2002), the net effect on a print is its relief carried downwind at the bed's
celerity c = q / H (Exner): the upwind wall fills first and the hollow shallows as it creeps downwind.
- `_drift_prints()` (CPU) and `drift_give()` plus MODE_RELAX (GPU): the relief (rim less dent) advected,
  first-order upwind and conservative, through each texel's faces, gated by the lesser blown share of the two
  texels (damp or littered sand neither gives nor takes; shelter included). Its numerical diffusion, c l / 2,
  rounds the walls. c is 0.5 m times the old fade rate (about 1 m an hour at 10 m/s, 1.4 at 12; field flux at
  10 m/s, about 1e-5 m2/s, over a 2 cm print gives 2 m an hour); 40% of the old even fade stays for the
  softening the shear's sideways spread gives. `wind_fills_prints` (on).
- Bench: `--test=infill` (upwind half fills first, the hollow moves downwind, nothing under the threshold).

### Calm lee zones and stopped ripples (Paulo's pick, 19 Nov 2026)

Behind a boulder, wall, parked car or nebkha the wind near the ground falls under the sand's threshold: nothing
saltates, and the grains falling out of the slowing cloud bury the ripples under a smooth drape (the shadow
behind an obstacle, Hesp 1981; the wind shadow of dune models, Benes and Roa 2004). Further on, where the wind
recovers, ripples show again, low and nearly stopped.
- Built on world-links' lee (WorldWeather.obstacle_lee_at, scaling the sand's friction speed with the plants'
  drag partition): the ripples' shown share goes as the flux factor at a reference gale (none under 2% of the
  open flux, all from 40%), bilinear over the lattice so the shadow's edge is soft. The ripple field's own steps
  already slow there. Raster and path tracer alike (the ripple map's share channel and the far sand share).
  `lee_calm_ripples` (on). `--test=shelter` checks the cavity, the recovery and the open sand.

### Pebbles and gravel armour the sand (ground detail, 19 Nov 2026)

On a shingle or gravel beach the coarse grains take the wind's stress and shelter the sand between them (a
deflation lag; Bagnold 1941; Nickling and McKenna Neuman 1995): it neither blows nor grows ripples (only sparse
granule ripples, Sharp 1963). Ripples shown among pebbles read as fine stripes from above. GroundSurface3D reads
WorldGround.coarse_cover_at (the stone fields' cover) on its 1 m wind grid: the sand's supply and flux share fall
to none past 35% pebble and gravel cover (all under 10%). `coarse_armour` (on); checked in `--test=shelter`.

### Wet margins round still water (oasis pools, 19 Nov 2026)

Sand by a pool or lake stays wet above the water's level by capillary rise, about 15 cm in fine sand (Lu and Likos
2004), so a still pool's shore shows a band of dark wet sand (about 35% darker, Lekner and Dorf 1988) fading out
above it. The water's level (any water source or the water contract) is carried 2 m out from the cells it covers,
the nearest wet cell's; dry ground under 15 cm over it is kept near saturated at the top, falling to 0.4 at the
fringe's top. `water_fringe` (on); `--test=fringe`. The terrain material round the patch now shows the same pore
water in raster as the path tracer already did (it read the wet map under PT only).

### Salt crusts round still water in a dry climate (oasis pools, 19 Nov 2026)

Where the fringe's water evaporates at its top it leaves its salts there: efflorescence (Rodriguez-Navarro and
Doehne 1999), the pale crust round playas, sabkhas and desert pools (Nield et al. 2015; crust albedo about 0.5 to
0.7). Only in a dry climate: the `WorldWeather` season climate's aridity index (UNEP 1992: annual precipitation
over Thornthwaite's potential evapotranspiration, from the same `WeatherSeason` lines the weather uses) under
about 0.3, full under 0.1. The crust lies from about 8 cm over the water's level to some 35 cm, round the fringe's
top, and only by still water (a stream's banks are washed). It is a wet-map block (texel 6's tag 8.5, after the mud's fresh deposits at 7.5), drawn in
patches that join as it thickens, white and matte, by BaseMaterial3D, TerrainMaterial3D and the path tracer alike.
`salt_crusts` (on); `--test=fringe` checks it in a desert climate (`desert_climate.gd`) and not without one.
Docs round 2: a band of even width read as a foam line. The crust now follows the ground: its top wanders along the
shore (2.3 m value noise), reaches higher in hollows (the lattice's Laplacian over 50 cm) and lower on rises, and
fades out on banks over about 8 degrees. Up close it is polygonal plates some 10 cm across (salt polygons, Lasser et
al. 2023): whole plates crusted or bare, each a little whiter or greyer, with cracks that open and close along their
length; past a few millimetres a pixel the plates give way to their share. `--view=fringe` (and `--cam=` for close).

### Avalanche tongues and planed ripples (19 Nov 2026)

Any dig, however shallow, set the fresh-ground tint in full, so the tents' tails turned the whole face below a row
of prints into one dark, blocky rectangle. The tint now scales with the dig's depth, full from 3 mm (CPU field and
GPU shader alike).

Docs round 2 on the tongues (faint stains, no lobe; prints identical capsule dishes): the flow now scours 2.5 cm
thinning to 8 mm and piles everything it took into its last 25 cm and a rounded snout 28 cm on, a lobe about 3 cm
high (field lobes 2 to 5 cm, Breton et al. 2008), its thickness rising over the lobe's first quarter; the scar is
tinted at a quarter of a dig's (the same dry sand, shown by its relief). The bench's prints are boot soles
(`press_box(..., sole = true)`: heel, forefoot, tread), toed out and alternating across the walk. `--test=avalanche`
checks the lobe's height.

The tongue's scour and lobe are now tents 1.6 times the flow's half width: flat discs 4 cm apart left each disc's
edge as a terrace, a row of arcs down the tongue like contour lines. A change of the surface now planes the
ripples by its depth over a ripple's height (4 mm), read between the field's texels at each ripple cell: the old
all-or-nothing erase over whole texels left stepped, blocky edges round a tongue.

### No uniform ripple texture to the horizon (docs, 19 Nov 2026)

Dry sand showed the same busy small ripples everywhere to the horizon. The ripple amplitude now also fades in and
out over fields some 16 m across (two value-noise octaves, 16 m and 5 m), leaving smooth sheets between rippled
fields, as on real beaches and sand sheets, besides the 20 to 100% height variation within a field. The same lines
in material.cpp, the path tracer and GroundRipples' CPU copy.

### Path traced dents follow the field

The path tracer recaptures a mesh's vertex() output when a material parameter changes, not when a texture it
samples is rewritten in place, so a fresh rut could show at an older, shallower depth there. The patch material
carries `ground_deform_serial`, bumped at most once a frame whenever the field's texture was rewritten.
Its vertex() must also read the serial: the path tracer counts material parameters among a capture's dependencies
only when vertex() reads one, so the declared but unread serial recaptured nothing (ruts under PT stayed shallow,
covering pressed stones).

### No glints on damp sand

Glints (`sand_glints`) were killed by pore water only, not by the cell's own moisture, so on a damp beach every print
that bared sand between stones sparkled with white specks. Water held between the grains rounds their facets into
menisci: the grain share the wet map publishes now fades out from moisture 0.05 to 0.2.

### Drifting on sand (Paulo, 16 Nov 2026)

Asked: a fast slide through a sharp turn should throw sand, and dry sand should raise dust that has to settle.
- **Sideways slip throws and shoves sand.** Spray only came from spinning wheels (longitudinal slip). Now the
  contact's sliding velocity over the ground, longitudinal slip plus side slip, drives the throw direction
  and speed, the usual way game tyre emitters are driven (rate and direction from the contact's slip velocity),
  and as the physics has it: a sliding tread drags the sand
  with it. The same velocity drives the press's bow wave, so a sliding wheel heaps a berm on the outside
  of the turn, and the print is stamped along the travel at the width the tyre sweeps across it
  (`width |cos a| + contact length |sin a|`), so a drift leaves a wide scuffed track.
- **Side bulldozing.** A wheel sliding sideways in loose soil pushes a wedge of soil with its sidewall; on
  loose soil that is a large share of the lateral force (Ishigami, Miwa, Nagatani, Yoshida 2007,
  "Terramechanics-based model for steering maneuver of planetary exploration rovers on loose soil", J. Field
  Robotics: Hegedus bulldozing on the side face). Added against the side slip, Bekker's bulldozing on a face
  one contact length long and the sinkage deep, eased in over 0.3 m/s: sand scrubs a drift down.
- **Dust.** The silt and fine sand in dry sand (tens of microns) leaves the tread as a plume, rides the wind and
  settles at its Stokes speed, w = (rho_p - rho_a) g d^2 / (18 mu): 0.13 m/s at 40 um. Emission follows the
  unpaved-road form, linear in vehicle speed (Gillies et al. 2005, Atmospheric Environment 39), times the
  share of the ground the wind could move (dry, bare sand; damp sand gives none), plus the sand thrown by
  slip. Transport is the standard Eulerian dispersion model: advection by the wind and the plume's own decaying
  jet, eddy diffusion, gravitational settling, deposition at the ground. A small grid follows the vehicle
  (`GroundDust3D`, CPU, about 40k cells) and is drawn as a FogVolume density texture, so raster volumetric
  fog and the path tracer's camera segments both see it. NVIDIA Flow (the fork's smoke) was the other
  candidate; it builds on Linux too (lavapipe in the cloud), so it becomes the near-field dust (below).
- **Built (16 Nov 2026).** Vehicle: contact slide velocity drives bow, throw and dust; prints laid along the
  travel; side bulldozing (`get_wheel_soil_side_force`). `GroundSurface3D.dust_emission` (0.02, tuned for the
  look: a sliding 1500 kg 4x4 lifts a few kg a second, far above unpaved-road PM10 data, because the plume is
  modelled as 15 um silt alone, which settles at 1.8 cm/s and lingers for a minute). `GroundDust3D` (48 x 16 x
  48 cells at 0.5 m, trailing its target). `--test=drift` (8 checks): Stokes 0.0180 m/s, a still puff sinks at
  it, wind carries a puff, 60 um dust settles out, sideways throw 3.1 m/s, sidewall 475 N, plume to 2.5 m on
  dry sand, none on damp. Raster and path-traced frames match (the path tracer sees it on camera rays only).
- **Round 2 (17 Nov 2026).** Billows: the fog shader erodes the grid's density with tiling 3D Worley fBm
  (Schneider 2015, Horizon Zero Dawn clouds; `billow` 0.9, `billow_size` 2 m), drifting with the wind. Grid 64 x
  20 x 64 at 0.5 m (32 x 10 x 32 m), `dust_emission` 0.06, a stronger upward jet with ground speed. Sideways
  throw now leaves as grains (side bulldozed volume), and `PhysXGroundGrains3D` no longer turns on MPM auto
  colliders, so grains land on the field instead of resting on its static collider.
- **Flow near field (17 Nov 2026).** Paulo wants the near dust in PhysX Flow: ground shadows by density, the car
  body as a solid, scene wind, every wheel. `PhysXGroundDust3D` claims the dust lifted within `near_range` (40 m)
  of its target (`GroundSurface3D.claim_dust` / `take_near_dust`; the rest still goes to `GroundDust3D`) and feeds
  its own PhysXFlow3D through a pool of sphere emitters, one per dusty wheel, coupled hard (a wheel crosses its
  sphere in a tenth of a second), swelling (`expansion`, emitter divergence) and gusting (`turbulence`, the
  wake's eddies) so the plume rolls. Heavy dust via `smoke_buoyancy` (`dust_heaviness`), the floor from the
  deformed sand (`GroundSurface3D.get_height` for Flow's terrain floor), ground wind as an air-only box emitter at
  0.75 u10. Flow now takes the shapes of bodies whose shapes live on their own PhysX actor (vehicles) from their
  CollisionShape3D children, so the dust curls round the car. Raster and path-traced frames match in shape (the
  path tracer's dust is browner and grainy without a denoiser). Defaults (17 Nov, after frames): `dust_heaviness` 0.3, `expansion` 3, `turbulence` 4: the plume billows to about car height. Raster dust is greyer than the path tracer's.

Docs round 3 on the rills (18 Nov: glyph-like channels with square hooks and cross bars, no moving water, no fans).
The hooks came from the bed's fixed roughness, value noise whose blobs have edges along the grid's axes: the
seepage followed them and cut them in. It is now gradient noise (Perlin 2002), each octave turned by its own
angle, and the rills curve and braid at shallow angles. Tried and dropped on the way: a diffusive bank flux in
the solver (it worked against the fixed relief and went unstable) and B-spline drawing of the relief (no visible
change: the shapes were several texels wide, not texel facets).

## Characters print with their feet (world walker contract)

GroundDeformField::press_bodies (shared by sand, mud and snow) never presses a walker's body shape
(WorldContracts::describe_body(...).walker, every CharacterBody3D unless meta `world_contact_walker` says otherwise).
A walker with meta `world_contact_feet` has feet that are contact bodies of their own and is skipped; any other walker
presses WorldWalkerSteps soles, 0.27 x 0.10 m boot outlines (the press_box sole shape), a step 0.41 of stature and the
feet 18 cm apart. Ground bench `--test=steps`; the snow bench's crust checks drag a boot (`world_contact_walker` false).

## Fine relief under path tracing, and wet tracks in their prints

The field's millimetre relief (the bed's runoff roughness, a swash mark's lip) is finer than the patch's vertices, so
raster draws it as an unsharp mask on the field's heights (Luft, Colditz and Deussen 2006). The path tracer now applies
the same term from the same map (raygen rt_ground_relief): before, grazing sand read flat traced beside raster (oasis
bench, about a third of the fine detail). The term fades out over the field's outer fifth, so the field's square has no
edge on the ground past it.

WeatherMap3D's contact wetness (wet tracks, 12.5 cm texels) is shaped on printed ground by the field's own dents
(ground_contact_shape(), raygen the same): the water lies in the sole's and tread's outline, and the contact map sets
only how wet it is.

## Far ripples over the dune field (GroundSurface3D `far_ripples`, on)

Paulo's pick (20 Nov): wind ripples past the 32 m field. The grown field's periodic slope tile (the near ripples'
own, wind-aligned, varied by ground_ripple_vary so no repeat shows) is drawn as a normal over all sand terrain past the
patch: BaseMaterial3D's terrain branch takes the ground's share (all sand for a sand ground_type with no type_map,
meta[14]); TerrainMaterial3D takes its own sand share (tm_sand_b). It eases in over the sand block's outer 4 m, so the
block has no edge. None on slip faces past about 30 degrees (Bagnold 1941: the face avalanches and buries them). Crests
are prefiltered to the pixel as before (faded under three pixels a crest); crests finer than that still shade the pixel
as a patchy tone, up to 15% darker by the field's amplitude (LEAN mapping's point, Olano and Baker 2010: unresolved
slopes become the pixel's statistics). The path tracer the same in raygen, with the cone's width as the footprint
(rt_cone_width).

## Ripple grain sorting (GroundSurface3D ripple_sorting, on by default)

Wind sorts a ripple's grains: the coarse ones creep up onto the crests, the small dense ones (magnetite, ilmenite,
garnet) settle in the troughs as a lag, so a ripple field shows dark lines in its troughs and paler, coarser crests
(Bagnold 1941; Sharp 1963; Anderson and Bunas 1993). Drawn as an albedo tint keyed to the ripple's height in
standard deviations of the grown field's (map meta m3.w carries that spread, 0 when off; the far texture's B and the
map rows n+S..n+S+n-1 its normalised height). The trough lag is patchy over some 7 m, as the heavy-mineral share is.
Where the crests are finer than a pixel the ground takes the bands' mean tone, scaled by the field's height there
(the LEAN point again). Same lines in BaseMaterial3D, TerrainMaterial3D and the path tracer
(ground_ripple_sort_tint).

## Dune-scale tone (TerrainMaterial3D `sand_dune_tone`, GroundSurface3D `dune_tone`, on by default)

Three colour changes a dune field shows from afar, after the grain-scale speckle and the ripple sorting:

- Rubefaction: iron-oxide coats redden grains that have lain still for millennia (Walker 1979; Pye and Tsoar 2009),
  so older, stabilised sand is redder than freshly worked sand. Drawn as a patchy low-frequency field (420 m and
  130 m value noise), strongest where the sand is thin and stable.
- Interdune flats: deflation strips the fine sand and leaves a coarser, darker lag (Sharp 1963; Lancaster 1995). Keyed
  to the generator's interdune floors (TerrainGenerator3D.get_sand_interdune_at(), published as
  `terrain_sand_interdune_map`): sand depth against the deepest within about 50 m, so floors count even where they
  still carry metres of sand (a first version keyed to absolute depth under 3 m found almost none in a real sea).
- Slip faces: grain flows of fine, well-sorted sand at the angle of repose, paler than the stoss (Bagnold 1941). Keyed
  to slope (about 29 to 33 degrees), so it shows on any sand.

One function (ground_dune_tone in material.cpp and the raygen, tm_dune_tone in TerrainMaterial3D, same hash and
noise) so the patch meets the terrain without a seam. The patch reads the generator's depth and its own sand share
from the ripple map's sand block (G, B; meta texel 4 x = on), so the path tracer needs no new parameter.
TerrainMaterial3D runs as a custom hit shader under path tracing, so it needs no copy.

## Granule ripples on the interdune lag (TerrainMaterial3D `sand_granule_ripples`, GroundSurface3D `granule_ripples`, on)

Where deflation leaves a coarse lag (the dune tone's dark interdune flats), the grains too heavy to saltate creep
under impacts into granule ripples (megaripples): 0.3 m to several metres apart, a few centimetres high (ripple index
10 to 20), coarse grains armouring the crests and windward slopes over troughs of fines, crest lines less regular
than impact ripples (Sharp 1963; Ellwood, Evans and Wilson 1975; Yizhaq et al. 2012; Lammel et al. 2018; arXiv
2208.00888). Drawn as a normal and tint, h = 0.04 amp c with c a raised cosine of a phase that carries: a 1.1 m
mean spacing, a warp along the wind whose slope moves the local spacing between about 0.5 and 2 m, and crest bending
over metres. Crests run tens of wavelengths and taper out at their ends. The slope comes from the phase's own
gradient (differences). Crest colour follows the granules: paler for quartz (the default here), darker for lithic or
basaltic granules. Gated by the dry, wind-blown share (none under water or on wet sand), eased to the ground's own
past the patch's block so nothing stops at its edge. A first version (straight, even, dark painted stripes with hard
ends) was rejected by docs, 20 Nov. The second (r2, with paired edge dislocations as forks and h = 0.05 amp c^1.5) was declined
21 Nov: each dislocation pair's closed contours read as a small oval rimmed pit (a dark speck at its core up close),
and the steep lee (12 degrees) went hard-edged and dark under a 12 degree sun. The forks are gone, the profile is a
rounded cosine under 7 degrees, and the lag and trough tints are warmer (lag 0.84, 0.74, 0.62: the reddish brown of a
desert lag, not grey).

## Next sand candidates (research, 21 Nov 2026; on a card)

- **Rain pits and rain crust.** Drops on dry sand leave raindrop impressions: shallow craters a few mm across
  with low raised rims, denser and overlapping as the shower goes on, until a downpour smooths them away
  (Reineck and Singh 1980). As the wetted skin dries it seals into a thin structural crust, its fines washed
  into the pores (Valentin and Bresson 1992), which holds the pits and keeps the wind off until saltation or a
  foot breaks it. Built on what exists: the rain rate from WeatherMap3D get_rain_at (the one weather source,
  no private copy), the field's wetness and drying for the crust's age, `rain_smoothing` for the downpour end
  and the silt skin's break and re-crust logic for its breakup. Drawn as a stochastic pit layer in the near
  field (hashed drop positions per cell and time, so the count grows with the rain's total) and a crust share
  that darkens and mattes the sand and stops ripples and streamers where it holds. Medium.
- **Dust devils over hot sand.** Convective vortices form on clear, calm afternoons over a strongly heated
  surface (lapse rate far over adiabatic near the ground), a few metres to tens of metres across, wandering
  with the light wind for minutes (Sinclair 1969; Balme and Greeley 2006). Built with PhysX Flow (the house
  rule): a rotating, rising emitter lifting dust where the surface is loose and dry, spawned from WeatherMap3D
  get_surface_temperature_at, get_sun_light and get_wind_at, with GroundStreamers3D taking the grains at its
  foot and a faint swirl left on the ripples. Medium to large; costs Flow time while one lives.
- **Adhesion warts at damp edges.** Dry grains blown onto a damp surface stick where they land, building small
  irregular warts and ridges that grow upwind (adhesion structures, Kocurek and Fielder 1982). For oasis margins
  and drying swash: where the wind's sand flux crosses the wet margin, add a rough, upwind-facing wart relief and
  a paler dry-grain tint. Small; reads only up close.

## Rain pits and rain crust (GroundSurface3D `rain_pits`, on; built 22 Nov 2026)

The card's recommended pick, built while the card waits. Each drop digs a crater about 2.5 times its own size, some
0.12 of its width deep, ringed by a low rim of the sand it throws out (Katsuragi 2010, PRL; Zhao, van der Meer and
Lohse 2015, PNAS, "Granular impact cratering by liquid drops"). A hundredth of a millimetre of rain lands some 5000
drops of 1.5 mm on a square metre, so the craters cover dry sand by about 0.05 mm of rain (cover = 1 - exp(-mm /
0.03)), and each rain lays a new pattern (a seed) over the last. The wetted skin dries into a structural crust,
its fines washed into the pores (Valentin and Bresson 1992): from half a millimetre of rain, whole by 3 mm, over
half an hour of drying, never while the surface is wet. Saltation over loose sand wipes the pits in minutes and
wears the crust away over hours (both scale with u*/u*t - 1, as the ripples' flux does, and with
ripple_time_scale); the crust keeps the wind off what it covers: the ripples' mask, the prints' wind filling and
get_blown_at() (so GroundStreamers3D) all take (1 - crust).

- State: patch-wide (cover, crust, mm since rework, seed), from WorldWeather rainfall_at (less 0.8 x canopy
  closure) and surface_wetness_at at the patch's centre, in _rain_marks() each ripple update.
- Drawing: ground_rain_pits() (material.cpp, the raygen's copy; TerrainMaterial3D calls the same function through
  the ground deform code): two chances per 1 cm cell, craters 3 to 7 mm across, h = -0.24 R (1 - u^2) inside and a
  0.072 R rim at u = 1.1, as a normal; finer than about three pixels they fade to a 5% darker skin. Meta texel 6 of
  the ripple map (cover, crust, seed), so it needs wind_ripples and dune_tone's sand rows; weighted by the sand
  share; none in prints (relief over 3 to 8 mm), which cannot tell a print's age from the rain's.
- Checks: ground --test=rain (a shower pits the sand, more as it goes on; no crust while wet; still air keeps the
  pits; saltation wipes them; off reads nothing). --view=ripples --rain-pits=mm draws them; raster and path traced
  agree at 100% crop.
- Not yet: the crust's own look (sealed skin, fewer grain glints) and crust plates breaking under feet; drops
  under trees as larger drip craters.

## Adhesion warts (GroundSurface3D `adhesion_warts`, on; built 22 Nov 2026)

The card's next option, built while the card stays open. Saltating grains that land on a damp surface stick there,
held by the pore water's menisci, and build up into adhesion structures: irregular warts on a damp flat, adhesion
ripples where the wind is steady, growing upwind as dry sand keeps landing (Hunter 1973, "Pseudo-cross-lamination
formed by climbing adhesion ripples"; Kocurek and Fielder 1982, "Adhesion structures"). Typical of wet interdunes,
sabkhas, beaches above the swash and oasis margins.

- State: patch-wide growth 0..1 that saltation builds (u*/u*t - 1, an hour at twice the threshold to full, sped by
  ripple_time_scale) and rain flattens (e-folding over 1 mm); the wind that built them. In _adhesion() next to the
  rain marks, published in the ripple map's meta texel 7 (wind x, z, growth).
- Where: per pixel, where the wet map's pore water is over 0.15 to 0.3 (ground_wet.x; the pool's own water hides
  them where it stands), on the sand share. They go as the sand dries (dry warts blow away with the loose sand).
- Drawing: ground_adhesion_warts() (material.cpp, raygen copy; TerrainMaterial3D calls the shared one): the tops
  of two octaves of warped value noise, turned against each other, over a threshold that falls with growth, longer
  across the wind, patchy over decimetres; 2 to 5 mm high; slope by differences; the lumps' tops count as dry (pore
  water x 0.2), so they read pale on the dark damp sand. Unresolved (finer than about three pixels) they fade to the
  mean dry share.
- First scout: hashed ellipses read as scattered dots, and a gate on the wet map's film channel left a hard line
  along the saturated strip; replaced by the noise field and the pore-water gate only.
- Checks: ground --test=rain (saltation grows them, rain flattens them, off reads nothing); --view=fringe --adhesion
  --no-nmap --taa --cam=0,0.6,2.3,0,0,1.5 --sun=-14,90 (with --prop=adhesion_warts:false for the before).
- Not yet: true upwind-climbing adhesion ripples under a steady wind; warts on rain-damp sand (the weather's surface
  wetness rather than the ground's own pore water).

## Dust devils (PhysXDustDevils3D, on; built 22 Nov 2026)

The card's last option, built while the card stays open, with PhysX Flow as asked. Dust devils are convective
vortices over strongly heated, dry ground on sunny, light-wind afternoons: a few metres across, spinning at 5 to
15 m/s round a rising core, drifting with the wind for minutes, lifting loose dust into a tan column tens of metres
high (Sinclair 1969, "Vortex development and dust devils"; Balme and Greeley 2006, "Dust devils on Earth and Mars").
Their flow is a Burgers-Rott vortex: radial inflow in a shallow layer at the foot turning up into an axial updraft,
solid rotation inside the core radius and the swirl falling off outside it (Rankine-like).

- Weather gate (get_favour_at, 0..1): the surface's heating over the air from WeatherMap3D (surface minus air
  temperature, none under `min_heating` 8 K, full 12 K over it), the 10 m mean wind (mean_wind_at: not the devils'
  own swirl) (gone by 6 to 11 m/s: shear tears the
  column), the surface wetness (0.1 to 0.3), sun visibility, and none at all in rain over 0.05 mm/h. Read through
  WorldContracts (the one weather source, no private copy).
- Spawning: a Poisson process at `frequency` devils per km2 per hour at full favour over the disc of `spawn_range`
  round the follow target, where WorldGround blown_sand_at says the dust is loose, at least 30 m apart, at most
  `max_devils` (2). Radius 1.5 to 6 m (most small), swirl 6 to 15 m/s, either sense, 1 to 6 minutes. spawn() places
  one by hand.
- Motion: with 0.7 of the 10 m wind plus a slow wander; spin-up over 15 s, decay over the last 25 s; once the
  favour goes (rain, wind, cloud), it dies within 25 s.
- Flow column (Burgers-Rott forcing with air-only emitters in one PhysXFlow3D, 0.25 m cells): a foot ring of 8
  spheres on the core radius blowing tangentially with a weak inflow and an updraft, warm (the superheated surface
  layer), lifting dust in proportion to the loose sand; three spinning rings at 1.5, 3.5 and 6 radii, flaring,
  keep the wall spinning up the column; a warm box core (a few kelvin: buoyancy and the shimmer, no glow, no fire
  light) lifts it. The dust fades slowly (0.06/s): the fines hang, so the column is as tall as the updraft takes
  them (a fast fade, 0.5/s, left a squat 10 m puff). Flow carries the rest: the column leans
  and drifts in the weather's wind (its pull weakened to 0.25/s so the column keeps its own updraft), the dust swirls
  up the wall and fans out on top.
- Shared wind (Paulo's connectivity rule): each devil is a vortex in WeatherMap3D (add_vortex / set_vortex /
  remove_vortex, docs/ideas/world-contracts.md), swirl as it spins up and down and an updraft of 0.3 of it (Tratt
  et al. 2003), so wind_at and the shaders' weather_wind carry it: leaves, grass, dust, smoke and other gases feel it.
- Flow runs one 1/60 s step per frame (maxStepsPerSimulate 1): below 60 fps it runs slower than the game. The bench
  renders with --fixed-fps 60 so a second of devil is a second of gas.
- Checks: docs/bench/dust_devils --test (headless: the gate, drift, spin-up, the vortex in the shared wind (swirl
  on the ring, updraft on the axis, none 8 R out, gone with the devil), the weather's spawns under the cap, rain
  stopping new ones and killing the old); the view renders one devil (--raster, --pair, --flow=prop:value).
- Look: cloud scouts (lavapipe, ~25 min for 20 s of devil) still showed a squat column with a dark, dense foot;
  the look and the PT pair go on the PC queue, where a scout takes seconds.
- Round 2 (22 Nov), from the PC runs (build ca985258): raster and PT matched and the foot was light tan, but every
  variant was a tent, widest at the ground and tapering to a spike, and read as spray. Rebuilt on the measured
  structure (Sinclair 1973; Kaimal and Businger 1970; Balme and Greeley 2006):
  - the surface inflow layer: foot emitters at half their radius above the ground, converging at half the swirl, so
    the dust is swept into the wall and up it instead of thrown out into a wide skirt;
  - the wall's updraft near the swirl (0.8 of it; Sinclair measured 5-15 m/s in 8-20 m/s walls) so the dust
    outruns its spread;
  - rings to 15 radii (1.5, 3.5, 6, 10, 15), flaring 5 % a radius: an inverted cone of a few degrees, heights ten to
    thirty diameters; V r kept up the flare;
  - lift in two arms turning at half the vortex's angular speed (smoke 0.25 to 1.75 of the mean round the ring), wound
    into spiral bands;
  - Flow's block budget: at 0.25 m cells the column and its trail asked for over 1,800 blocks in 8 s and the 1,024
    cap cut the column. Now 0.35 m cells and 2,048 blocks (about 900 used at 16 s), an air-only reserve box over
    the whole column (Flow loses gas that reaches unallocated blocks), and PhysXFlow3D `allocation_smoke` (new) so a
    freed block holds no visible dust;
  - smoke density 0.8 (1.6 blackened the foot once the dust stayed in the wall), scattering strength 1 (PT parity).
  Cloud scouts: a taller column widening upward; at 320-400 px lavapipe draws a pale haze cut in steps round it
  that the PC's 1080p frames never showed (it stays with heat shimmer, light and ambient scattering all off). PC
  runs judge the look.
- GroundStreamers3D takes the grains at the foot (22 Nov): its particle shader adds the shared wind's vortices
  (WeatherMap3D's own shader field, weather_vortices) to the patch's wind, so each grain runs with its local wind
  (round the devil, in toward its wall) and flies only where that wind's u* passes the threshold, in the share of the
  budget its flux makes; the budget is set for the strongest wind over the patch (a 9 x 9 read of
  get_vortex_wind_at). Ground --test=wind: a 12 m/s devil in a 0 m/s wind lifts sand (peak 11.1 m/s, 8.8 g/m/s).
- Devil tracks (22 Nov). Research: dust devils leave tracks where the foot strips the fines and flattens the ripples
  in a band about the devil's width (Reiss et al. 2011, Turpan; Balme and Greeley 2006); on Earth saltation fills
  them within tens of minutes, in a calm they stay. GroundSurface3D `add_scour(position, radius, amount)` keeps the
  share swept per 1 m world cell (each pass sweeps its share of what is left, fading over the disc's outer 30%), read
  bilinearly; the ripples' show share (near ripples and the 32 m sand patch, so the far ripples too) is multiplied by
  what is left. It heals only under saltation: rate = local flux / flux at 10 m/s / 600 s (the patch's own flux,
  past it the open flux), on ripple_time_scale; cells under 0.01 or a kilometre off are dropped. PhysXDustDevils3D
  scours 1.3 R round its foot at strength x dt per second. Ground --test=shelter checks it sweeps, refills under
  12 m/s, holds in 3 m/s.
- Track tone (22 Nov): the foot lifts the bright fine grains off the surface skin and leaves the coarser, darker sand
  under it (Reiss et al. 2011; Balme and Greeley 2006), so the band reads a little darker and warmer: albedo x
  (0.9, 0.87, 0.82) at full scour, the interdune lag's hue much weaker (only the skin goes). The share swept goes to
  the ripple map's tile rows, G, over the sand block's rect (rows n+S..n+S+S-1; meta texel 5 w on when any), read
  by material.cpp ground_sand_scour_at() / ground_scour_tone() and the same lines in raygen. Inside the dune tone's
  sand block (dune_tone on, the default). Raster scout: the band about 4% darker, its ripples gone.
- Not yet: the PT cost on the PC queue.

## Nebkha tails (GroundSurface3D `plant_wake`, on; 23 Nov 2026)

The last of the round-2 candidates. Shrubs and grass tufts trap blown sand into mounds with tails downwind (shadow
dunes, nebkhas: Hesp 1981; Tengberg 1995; Lancaster and Baas 1998). The mound under the crown was already built
(the drag partition, "Dune grass"); the tail was missing because the plants sheltered only the ground under them.
- Technique: Okin's (2008) lee model as Li et al. (2013) give it: behind a plant the surface's friction speed is
  u*s/u* = r0 + (1 - r0)(1 - exp(-x / (C h))), r0 0.3, C 6 (their fits 0.28 to 0.32 and 5.6 to 6.2), the nearest
  plant upwind setting it. Each 1 m cell of the shelter grid looks back along the wind three e-folding lengths;
  a plant is lambda over 0.02 to 0.3 (not a few stems). h is `plant_wake_height` (1 m), since the vegetation gives
  frontal area, not height.
- The sand reaches the shadow by spreading sideways: the flux carried downwind diffuses across the wind (0.2 m per
  metre run, as a difference across 0.5 m, a modelling choice so the clouds from either side of a 2 m plant meet within its wake; mass kept), only
  with a wake present, so open-sand transport is unchanged. Where the wake slows it the sand drops (the same Exner
  deposit), so the tail grows from both sides and tapers as the wind recovers.
- Ground --test=shelter: the tail's deposit after two scaled hours at 1, 4 and 11 m behind a 2 m tuft, and none
  5 m to the side.

## Next sand candidates, round 2 (research, 22 Nov 2026; on a card)

- **Haboob dust walls** (recommended). A thunderstorm's cold outflow (the gust front) spreads along the ground and
  lifts loose dust into a wall 1 to 3 km high and tens of km wide, moving at 10 to 25 m/s, its head a rolling,
  lobed density current (Sutton 1925; Idso et al. 1972; Miller et al. 2008, "Haboob dust storms of the southern
  Arabian Peninsula"; Knippertz and Todd 2012). Visibility drops under 1 km, the light turns orange to brown.
  Built on what exists: WeatherMap3D storms already blow a downburst outflow (Oseguera and Bowles 1988); the dust
  source is WorldGround blown_sand_at times the outflow's friction speed over threshold (GroundRipples
  saltation_flux, Shao's vertical dust flux ~ q x clay share); the far wall as a WeatherMap3D dust layer (like
  smoke_at: column load g/m2 riding the outflow front, washed out by the storm's rain) that the sky, haze and sun
  visibility read; near the camera, PhysX Flow billows at the front's head (lobes and clefts, Simpson 1997). Large.
- **Nebkhas round shrubs.** Shrubs slow the wind in their lee and trap blown sand into mounds with tails
  downwind (shadow dunes; Hesp 1981; Tengberg 1995; Lancaster and Baas 1998). Read the vegetation's shelter
  (WeatherMap3D shelter boxes / WorldCanopy) in the dune and ripple transport (GroundSurface3D shelter flux
  already relaxes to the drag partition), deposit the excess as a mound under the crown and a tail of length
  ~ 5 to 10 crown heights. Medium.
- **Devil tracks and grains.** Finish the devils: the swirl scours a faint curved track across the ripples (the
  lag left darker, Balme et al. 2003 for Mars, faint on Earth) through the near-field ripple map, and
  GroundStreamers3D grains streaming in at the foot. Small.

## Haboob dust walls (Paulo's pick, 22 Nov 2026): design

Technique: wind erosion dust emission over the weather map (Marticorena and Bergametti 1995: horizontal saltation
flux Q from the friction speed over its threshold, Kawamura/Owen; vertical dust flux F = alpha Q with
alpha = 10^(0.134 clay% - 6) /m), carried as a mineral dust aerosol layer beside the fires' smoke, with the
haboob's shape from the storm outflow as a density current (Benjamin 1968: front speed ~0.7 sqrt(g' h); a raised
nose and lobes and clefts along it, Simpson 1997).

- Source: every map update, per cell, u* from the cell's 10 m wind (which already carries the storms' downburst
  outflow) on the log law; over the dry-sand threshold (Bagnold, wetted by WorldGround moisture and the weather's
  surface wetness) on loose ground (WorldGround blown sand share / a default desert share), F in g/m2/s into the
  column. A general dust storm law: haboobs come from the storms' outflow, any strong dry wind raises dust.
- Layer: g/m2 per cell, advected by the wind near the ground (not aloft: the dust rides the outflow), a 1.5 km
  mixed depth in the outflow, gravitational settling (about 1.5 cm/s for 10 to 20 um, Stokes) and the shared
  washout (WorldWeather washout_rate, coarse). Optics: mass extinction 0.6 m2/g at 550 nm, Angstrom about 0.2,
  single scattering albedo 0.92 falling to the blue (iron oxides): the sun dims and turns orange to brown, the
  light goes brown-orange, visibility under 1 km in the wall (sigma = 3.9 / V).
- Readers: contract dust_at / dust_concentration_at (like smoke_at), the sun's transmittance (SkyClock), the haze
  and the sky (aerosol by colour), the path tracer's medium.
- The wall itself: a volume in the cloud march between the ground and the layer top, density from the dust layer
  with the density current's head (a nose up to 1.5 to 2 times the layer height at the front, lobes and clefts
  along it, billows behind), lit like the low cloud but brown-orange and forward-scattering.
- Near the camera: PhysX Flow billows at the front's foot (rolling, lifted dust curling over the nose), seeded
  where the front crosses the camera's field, as the near tyre dust (PhysXGroundDust3D).
- Ownership: the dust layer and its optics live in the weather module (Valence's); the wall in the cloud march
  (the clouds thread); the source law and Flow billows here.

### Built, stage 1: the dust layer (22 Nov 2026)

WeatherMap3D `dust_storms` (on), weather_dust.cpp, as designed above, with what the bench taught:
- The source reads the erodible surface's own roughness (5e-4 m), not the map's, and a 0.35 m/s threshold (natural
  desert surfaces, Gillette 1980): about 8.5 m/s at 10 m. `dust_clay` 8 %.
- The outflow is a density current with a front (the shelf cloud's ring: 1.45 core radii plus 0.02 per m/s, lobed):
  the dust's own wind drops the downburst profile's 1/r tail beyond it, and the transport is a forward,
  mass-conserving splat, so the dust piles at the front (a backward trace cannot pile up where the wind converges).
- Profile: densest at the ground, scale a third of the top (a uniform mix left 16 km visibility in the wall; now
  under 1 km at 4 g/m2, as observed in haboobs).
- docs/bench/haboob --test: source law (threshold, wetness, clay), a ring of 4 g/m2 round a 25 m/s outflow with its
  head marked outside the peak, the nose at 1.7 x the depth, the profile, the sun at 7 % through it, the contract,
  rain washing it out. `--map` prints the column.
- Next: the wall in the cloud march (clouds thread, against weather_dust_map).

### Built, stage 3: the near-field billows (22 Nov 2026)

PhysXHaboobFront3D (PhysX Flow), after the gravity-current head (Simpson, Gravity Currents, 1997): a raised nose that
rolls Kelvin-Helmholtz billows back over the current, split into lobes and clefts along the front. Twice a second it
finds the head share (`get_dust_head_at` above 0.4 with dust) nearest the camera within `near_range` (300 m), lays
`emitter_count` (10) points across the front's travel over 1.6 x that, each slid onto the head's crest, and moves
them with the front between scans. Each is a wide box emitter pushing dust forward at 1.1 x the front's speed and up
at half of it; Flow's own vorticity confinement rolls the shear into billows, so no shape is authored. Its Flow:
2 m cells, no buoyancy, slow fade, shimmer and fire light off. docs/bench/haboob --test: 10 points, all on the head,
all travelling outward (about 11 m/s). Look: PC queue (Flow raster needs the GPU at this size).

Round 2 (22 Nov), from the first raster scouts (a wall, but pale and cut flat in steps at the top):
- The steps were Flow's blocks: gas carried up past the allocated blocks is read as still air there and lost. Each
  front point now has an air-only reserve box (couple 0: allocates, pushes nothing) from the ground to 1.5 x
  `head_height` (100 m) and 140 m back, and a lid of still air above `head_height` (couple 1): the gravity current's
  head under ambient air at rest (Simpson and Britter 1979), so the shear there rolls the billows over and the dust
  stops near 75 m (probe) instead of rising into unallocated blocks.
- Pale: the puff was a hard, dense shell (0.15 per m per unit) lit with scattering strength 2 and a 4 m shadow reach,
  so every billow was lit alike. Now dust optics (albedo 0.92, forward lobe 0.6, Dubovik et al. 2002), strength 1 (PT
  parity), shadow reach half the head height, 12 steps; density 0.25 for the head's measured mix (smoke about 0.1
  there for 1 at the foot).
- The front points jumped tens of metres each scan (the map's ~190 m cells), smearing the head thin: now eased a
  quarter of the way to each find and moved by the found front's own drift between scans (a steady storm's ring
  stands still while its outflow blows through it).
- `dust_color` is the far wall's tint (similarity relations over weather_dust_albedo, as the sky's dust_tint):
  (1, 0.93, 0.62). `weather_dust_near` tells the sky's wall where the billows are.

Dust field round 2 (clouds' note: dust only in a 1.5 km ring, nothing behind):
- The transport jumped up to 8 cells a step (25 m/s x 60 s over 190 m cells) and crossed the front: now sub-stepped
  to a cell a step.
- The column rides the current's bulk speed (DUST_BULK 0.3 of the surface outflow; the 10 m wind still raises it):
  the following flow gains on the head at only 0.1-0.2 of its speed (Simpson and Britter 1979).
- The head passes on what flows in: its load returns at the inflow's rate over a head length (1 km), laid back over
  the wake up to 3 km behind (not past the rain core). Bench storm: head 8.9 g/m2, 4.1 at 500 m behind, 2.3 at 1 km.
- Emission check: 8.7 mg/m2/s at 30 m/s and 8 % clay, mid-range of measured strong-event fluxes (1-100); the
  Marticorena-Bergametti clay factor is uncertain by about ten, so `dust_emission_scale` 10 for a Phoenix-class event
  is within it.
