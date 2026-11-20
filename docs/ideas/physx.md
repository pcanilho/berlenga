# PhysX 5 in this fork

Status: research done, design proposed, nothing implemented yet.
Dates: first note 2026-11-06; research pass and design 2026-11-08 (see
"Research pass" and "Design" below). Scope chosen: physics backend,
destruction, cloth, and simulated smoke and fire that is path traced.

## Where it came from

The forks survey (#89) found a PhysX 5 physics server in Diginaat/godot
(branch `dev`). That module is not Diginaat's own: it comes from
uno1982/godot, branch `feature/physx5-module`, directory
`modules/godot_physx` (read at c6f3b65, 2026-11-05). Its example project
and per-feature tests are in uno1982/godot-physx-example.
NVIDIA-RTX/godot (`nvidia-pt-dlss`, 49a825dc) has no PhysX code, and
neither does `nv-port`.

## What the module provides

- A full `PhysicsServer3D` backend, selected per project like Jolt.
  `RigidBody3D`, `CharacterBody3D`, `Area3D`, joints and ray casts work
  unchanged.
- GPU rigid body dynamics (CUDA) with a CPU fallback.
- Particle fluids and granular material (sand, snow): CUDA, or an MPM
  solver on compute shaders that runs on any GPU.
- Cloth (CPU or CUDA), skinned cloth, GPU soft bodies.
- NVIDIA Blast destruction with an editor fracture tool (optional).
- NVIDIA Flow gas: smoke and fire (optional).
- Vehicles, FFT ocean with caustics, boats.
- 226 files. The PhysX SDK is not vendored: it is built out of tree with
  CMake and linked in. Without a configured SDK the module is skipped and
  the build is the stock one.

## Licences

- Module: MIT, Wild Ox Studios.
- PhysX 5, Blast, Flow: BSD-3-Clause (NVIDIA-Omniverse/PhysX). The GPU
  simulation source was opened in April 2025, so no binary-only parts are
  required.
- Compatible with this fork's MIT licence. Redistributed binaries carry
  the BSD-3 notices (COPYRIGHT.txt).

## Findings that matter here

1. GPU dynamics slow down when the GPU is busy. The module's own
   measurement (RTX 5070): a ball pile stepped in 8 to 9.5 ms on the GPU
   against 11 ms on the CPU with the GPU idle, but 21 to 23 ms against
   7.6 ms with the GPU about 90% busy. Every step waits for the GPU. With
   path tracing and Ray Reconstruction the GPU is always busy, so the CPU
   solver is the expected default; GPU dynamics would be measured per scene.
2. Flow gas is not path traced. `PhysXFlowRenderEffect` ray marches the gas
   in a compositor effect after transparent geometry, lit by the first
   directional light plus optional omni/spot scattering from shadow maps.
   Under path tracing it would sit on top of the traced image and stay
   outside the Ray Reconstruction inputs. To have gas that is lit by traced
   light, Flow's density grid would have to feed the path tracer's volume
   path (NVIDIA's traced fog) instead of Flow's renderer. This is the main
   piece of new engine work.
3. Platforms. Windows x64 and Linux x86_64, CPU and GPU, are built and
   tested. macOS, Android and web are not supported; Flow does not build on
   macOS. Linux ARM64 is not listed as tested. Our releases cover macOS and
   Linux ARM64, so those builds would ship without the module.
4. Jolt is already built in. The module's README says that on the CPU alone
   Jolt is the better choice. PhysX earns its place through the features
   Jolt does not have (fluids, granular, cloth, destruction, gas, vehicles,
   ocean), not through rigid bodies.
5. Known gaps listed by the module: long joint chains drift under constant
   force with the PGS solver (use TGS), no joint softness/bias/restitution,
   bounce between two bouncy bodies uses the larger value instead of the
   sum, area-to-area overlap is an O(n^2) poll per step.

## Open-world fit

Destruction, cloth, vehicles, ocean and granular terrain are what a large
open world uses most. Points to check before adopting it for that scale:
broad phase and scene query cost with many static bodies, streaming bodies
in and out by region, large-coordinate precision (the fork plans a relative
origin), and the per-step GPU sync from finding 1.

## Build and CI cost

- CI needs CMake and clang, plus the CUDA toolkit for GPU builds (12.8;
  end users only need the NVIDIA driver). The SDK build adds time to each
  Windows and Linux job; it should be cached by SDK commit.
- Linux binaries need the glibc of the build machine or newer.
- Shared libraries (`PhysXGpu_64`, Blast, Flow) ship next to the editor and
  go into the release zips with their notices.
- Flow in a fresh Linux cloud container: in the PhysX checkout's `flow/`
  directory run `bash build.sh`. Then build the engine with
  `flow_sdk=<that flow dir>`; SCsub copies
  `_build/linux-x86_64/release/libnvflow.so` and `libnvflowext.so` into
  `bin/`. If the libraries were built after the engine, copy them next to
  the executable by hand. The binary finds them through `$ORIGIN` in its
  RPATH, so no environment variable is needed.
- Without those libraries a Flow build still runs: it prints one error
  ("NVIDIA Flow unavailable") and every Flow node stays inert. Before
  17 Nov 2026 it crashed in `dlclose(NULL)` inside Flow's loader.
- PhysX is linked statically. Blast is linked dynamically, so a binary
  without the Blast `.so` files does not start (the system loader names the
  missing library). The GPU PhysX library (`libPhysXGpu_64.so`) is optional
  and loaded at run time.

## Options when the work starts

1. Module as is, CPU solver by default, no Blast or Flow. Smallest step:
   project-selectable PhysX on Windows and Linux.
2. Option 1 plus Blast and cloth for destruction and props.
3. Option 2 plus Flow, with Flow's density grid fed into the path tracer's
   volume path so smoke, steam and fire are lit by traced light and seen
   by Ray Reconstruction. Largest; needs its own research pass.

Static haze and steam do not need PhysX: the NVIDIA rewrite's volumetric
fog with ray traced shadows covers them. PhysX gas is for smoke and fire
that move and react to bodies.

## Attribution

Anything taken from uno1982/godot (Wild Ox Studios, MIT) is credited in the
README References. Copied code keeps its licence notice in COPYRIGHT.txt,
with NVIDIA's BSD-3 notices for PhysX, Blast and Flow.

## Research pass (2026-11-08)

### Reference: the Batman Arkham games (Unreal Engine 3, PhysX and APEX)

- Asylum (2009): tearing drapes and cobwebs, fog that leaves a wake
  behind moving characters, paper that moves when walked over, breakable
  tiles and walls (APEX Destruction; NVIDIA's GDC 2010 talk used it as the
  UE3 case study). The cape itself was the studio's own code.
- City (2011): newspapers and bank notes in the wind and in fights, pushable
  rubbish and masonry, glass shards that stay, rubber door flaps, smoke and
  sparks from damaged machinery, fog that reacts to running guards; APEX
  Clothing on Bruce Wayne (GDC 2012, Newkirk and Zoellner). PhysX High cost
  up to 13 fps on the hardware of the time.
- Origins (2013): APEX Turbulence for wind-blown snow and smoke that react
  to characters. Knight (2015): interactive fog and smoke, paper debris,
  floor destruction, rain beading off the cape.
- What made it work: many cheap, wind-reactive secondary elements (paper,
  fog, debris, rain on cloth), all moved by the same air, rather than a few
  large simulations. APEX Turbulence smoke was particles with particle
  shadow maps, not a traced volume.

### Unreal Engine 5

- Chaos Destruction: geometry collections fractured in the editor (uniform
  and clustered Voronoi, radial, planar, slice, brick, mesh cutters), one
  hierarchy level per fracture, upper clusters break first, neighbours
  joined by strain-limited links broken by impulses or fields, a cache
  system for replaying large collapses. Epic's docs give no cost numbers or
  open-world guidance; users report "remove on sleep" does not stop frame
  drops, so debris pooling and lifetime are the engine's own job.
- Chaos Cloth: a cloth section is made from part of a skeletal mesh without
  removing it from the mesh (works on one-piece meshes). Painted masks: max
  distance (0 = follows the skin), backstop distance and radius (keeps cloth
  out of the body), anim drive (springs back to the animated pose).
  Collision from the physics asset capsules. Panel Cloth (5.3+) adds XPBD
  constraints, geodesic long range attachments, and an aerodynamics model:
  per-particle drag and lift (defaults 0.035 to 1.0, air 1.225 kg/m^3),
  separate back-face values, a turbulence ratio blending linear and
  quadratic drag. ML Cloth is quasi-static (no swinging), for tight clothes.
- Heterogeneous volumes: the real-time path ray marches with a dense
  lighting cache; the path tracer re-voxelizes volumes into world and
  frustum grids under a memory cap (512 MB cinematic, 2 GB hard) and drops
  voxels when it runs out (blocky volumes). Sparse volume textures are a
  page table plus tiles, imported offline from VDB; nothing documented for
  GPU-simulated data.
- RTX Volumetrics (RTX Remix, Half-Life 2 RTX) is a ReSTIR-sampled froxel
  radiance cache (screen/16, 48 slices), composited into the colour before
  DLSS RR, and the RR control mask is scaled by fog transmittance.

### Papers

- XPBD (Macklin, Mueller, Chentanez 2016): stiffness independent of
  iteration count and time step, 1 to 3% over PBD, gives constraint forces
  (useful for tearing).
- Long range attachments (Kim, Chentanez, Mueller 2012): one-sided
  geodesic distance limits to the nearest pinned vertex stop stretching in
  3 to 5 iterations; 1,500-vertex garment 0.91 ms on one CPU core,
  14 characters (20K vertices) 0.8 ms on GPU.
- Hofmann, Hasselgren, Clarberg, Munkberg (NVIDIA), "Interactive Path
  Tracing and Reconstruction of Sparse Volumes": delta tracking for free
  flight, ratio tracking for shadow rays, one global majorant, Russian
  roulette below 0.1, NanoVDB with hierarchical DDA empty-space skipping,
  stochastic trilinear lookup (one fetch instead of eight, ~3x faster),
  emission from a temperature grid accumulated at every collision (no
  emitter NEE: +30 to 50% cost). 16 to 27 ms at 1080p 4 spp on an RTX 3090
  for explosions and plumes; cost rises steeply with albedo. One volume,
  no surfaces.
- Novak et al. 2014, residual ratio tracking: unbiased transmittance,
  better than delta tracking at equal cost; standard for shadow rays.
- Kutz et al. 2017, spectral and decomposition tracking: control density
  handled analytically plus a residual; 3.5x lower MSE than delta tracking.
- Miller et al. 2019, null-scattering path integral: pdfs for null-collision
  methods, so MIS between techniques becomes possible.
- Lin et al. 2021, Volumetric ReSTIR: reservoir reuse over volume paths
  including volumetric emission; 1 spp beats 4 to 13 spp of the baseline.
- No shipped game path traces simulated heterogeneous volumes yet; shipped
  practice is froxel caches (Remix) or particles.

### DLSS Ray Reconstruction and volumes

RR has no volume guide. Its optional inputs that matter: the transparency
overlay (premultiplied colour + alpha rendered separately, not baked into
the input) and colour-before-transparency (same format as the input colour).
Two patterns exist: feed a pre-filtered volume term into RR's input (Remix,
suits low-frequency fog), or keep the volume out of RR's main input
(overlay or composite after RR). Which one keeps detailed smoke sharp has
to be measured on hardware.

### Ray tracing updates for moving geometry (NVIDIA RTX best practices)

Rigid pieces and props: shared compacted BLAS per mesh, moved by TLAS
transform only. Cloth and skinned meshes: refit every frame, full rebuild
every N frames staggered across characters, skip off-screen and distant
ones, lower-poly RT proxy where possible. New fracture pieces: build their
BLAS once. Later for the open world: clustered acceleration structures and
partitioned TLAS. NVIDIA publishes no ms budget; ours is set by profiling.

## What the module does with each physics engine

Read from the module source (c6f3b65). Godot selects one physics engine
per project (`physics/3d/physics_engine`); two engines cannot own the same
bodies. With this module that setting offers Jolt or PhysX, and most of the
module's features do not depend on which one is selected:

| Feature | With Jolt | With PhysX |
|---|---|---|
| Rigid bodies, characters, joints, areas | Jolt | PhysX (CPU solver default; GPU solver optional) |
| `PhysXCloth3D` (own XPBD CPU solver, collides through space queries) | yes | yes |
| `PhysXSkinnedCloth3D` (compute shaders, any GPU) | yes | yes |
| `PhysXDestructible3D` (Blast; pieces are `PhysicsServer3D` bodies) | yes | yes |
| `PhysXFlow3D` smoke and fire (Flow on Godot's RenderingDevice) | yes | yes |
| Ocean, buoyancy, boats, wakes | yes | yes |
| MPM particle fluids and granular (compute) | yes | yes |
| PBD particle fluids, GPU deformables (CUDA) | no | yes |
| `PhysX` vehicles | no | yes |

So "Jolt or PhysX or both" becomes: pick the rigid-body engine per project;
cloth, destruction, smoke, water and MPM fluids work with either.

## Design

### Engine and build

- Module at `modules/godot_physx`, from uno1982/godot. Builds without the
  SDKs stay stock (module skipped). Windows x64 and Linux x86_64 release
  builds get PhysX (CPU SDK), Blast and Flow; macOS and Linux ARM64 build
  without it. CUDA (GPU solver) is not built in CI: the CPU solver is the
  default under path tracing (see findings), and Flow, skinned cloth and
  MPM do not need CUDA.
- CI builds the PhysX SDK with the module's `misc/build_physx.py`, cached
  by SDK commit; Blast's Linux build pulls ~3 GB of packman dependencies,
  cached the same way. Shared libraries (Blast, Flow) go into the zips with
  their BSD-3 notices.
- README gets a PhysX section: what it adds, the engine table above, how
  to select it, platforms.

### Path-traced smoke and fire

Flow already runs on Godot's RenderingDevice, and the module exposes the
grid's density atlas (temperature, smoke, light) and sparse block table as
RenderingDevice resources (`flow_backend.h`, `grid_render_rd`). The path
tracer can bind them directly: no NanoVDB conversion or copy each frame.

1. Geometry: one procedural AABB per active Flow block (32x16x16 cells) in a
   volume BLAS, rebuilt when the block set changes. Hardware traversal then
   finds entry and exit and skips empty space; every ray type (camera,
   bounce, reflection, shadow) can hit smoke, so smoke appears in
   reflections and shadows the world.
2. Majorants: per block, plus a 4^3 sub-brick max built by a compute pass
   after each sim step (Hofmann used one global majorant and names local
   ones as the improvement).
3. Estimators: delta tracking with stochastic trilinear lookup for camera
   and bounce rays; residual ratio tracking with Russian roulette below 0.1
   for shadow rays. Decomposition tracking (Kutz) once a per-block control
   density exists.
4. Emission: temperature through a blackbody table, collected at real and
   null collisions. For fire to light the scene, hot blocks join the light
   selection (the fork's light grid/RIS, later the light tree) as volume
   emitters with power = summed block emission, visibility by ratio
   tracking (Volumetric ReSTIR idea).
5. Phase function: Henyey-Greenstein per flow (forward scattering for steam).
6. DLSS RR: two candidates built behind one setting and compared on the
   RTX 4090 with 100% crops: (a) smoke in RR's main input with guides at the
   depth where transmittance reaches 0.5 (albedo = volume albedo, normal =
   density gradient, roughness 1, motion from the sim velocity); (b) smoke
   kept out of the main input as RR's transparency overlay with
   colour-before-transparency. The one that keeps wisps sharp without
   ghosting becomes the default.
7. Godot's froxel fog stays for wide haze; Flow is for local smoke, steam
   and fire that move and react to bodies.

### Cloth and props

- Characters: `PhysXSkinnedCloth3D` on one-piece meshes with painted max
  distance (vertex colour red): 0 on torso and shoulders, rising toward
  coat tails, hems and sleeves; backstop against the body; collisions
  against the skeleton's physical bones. Dense scanned meshes simulate a
  decimated proxy that skins the render mesh. Distance LOD: full rate near
  the camera, half rate further, plain skinning beyond.
- Long range attachments and XPBD with drag and lift per particle, as in
  Chaos Panel Cloth (check the module's solvers against these and add what
  is missing).
- Umbrella: canopy cloth tethered to rigid ribs on a spring-damped handle
  joint, wind torque on the canopy.
- Light props: rigid bodies with an air-drag force (area x relative wind
  squared), fast sleep, pooled, a per-scene cap on awake bodies; paper as
  small cloth patches. Breakable props (bottles) as small Blast assets.
- One wind field: every system (cloth, props, Flow, rain) samples the same
  field, and events such as a train's pressure wave add to it.
- Water: thin-water simulations in the project stay the water surface;
  PhysX splashes and floating props read their height rather than drawing a
  second surface.

### MPM fluid: two-layer water in raster (picked 17 Nov)

Problem (bucket pour, docs review 17 Nov): a stream falling in front of the
pool shows the bucket wall behind the pool, a faint brown band, where the path
tracer shows the pool's surface. The shot draws the isosurface mesh with the
clear-water material (Transmission, not thin-walled): raster transmission
reads the screen copy taken before the transparent pass, which holds opaque
geometry only, so the stream sees through the pool as if it were not there.
(A debug render ruled out the screen-space proxy: the shot does not use it.
The refraction mask added there in 0e459f27 stays: it fixes a real leak for
screen-space fluid.) The same limit hides a river behind a waterfall's sheet,
or one glass behind another, in every engine that refracts a single screen
copy (Unreal and HDRP document it for translucency behind translucency).

Technique: depth peeling (Everitt, "Interactive Order-Independent
Transparency", NVIDIA 2001), as screen-space liquid renderers do for layered
refraction (Imai, Kanamori, Mitani et al., "Real-Time Screen-Space Liquid
Rendering with Complex Refractions", CAVW 27, 2016). Two layers, in the
forward renderer:
1. Front depth: the screen-reading transparent surfaces (Transmission,
   refraction) drawn depth-only, front faces, into a scratch depth D1.
2. Second layer: the same surfaces drawn in colour into the scene colour,
   each fragment discarded unless it lies behind D1 (plus a small bias); their
   transmission reads the opaque screen copy as now.
3. The screen copy is taken again, so it now holds layer 2.
4. The normal transparent pass: the front layer reads the new copy and shows
   the pool behind the stream, lit and reflecting, as the path tracer does.
Only frames with such surfaces pay: one depth-only and one extra colour pass
over them, and one more screen copy. Back faces stay culled, so a closed
water mesh's second front-facing layer is the next body of water. PT is
unchanged.

Status: done. Project setting `rendering/transparency/refraction_second_layer`
(on). A two-sphere glass test found one flaw in step 3 as written: a surface
that is itself a second layer somewhere (the far sphere, where the near one
covers it) refracted its own image out of the new copy, a small ghost. So the
peeled copy goes into its own texture beside the opaque one, a third depth-only
pass (2b) keeps the second layer's depth, and every screen texture lookup is
wrapped by the shader compiler to pick per sample: the peeled copy only where
the second layer there lies clearly behind this surface and no other
screen-reading surface lies clearly in front of it, the opaque copy elsewhere
(as before). Cost when a screen-reading surface is visible: two depth-only
passes and one colour pass over those surfaces, one more screen copy with mips,
and two depth fetches per lookup. GPU bench on the PC queue.

Round 2 (docs: the band barely changed). Debug renders showed why: the
stream's refracted lookups mostly land beside it, where the pool is the front
layer, which the copy did not hold. Now the copy holds the front layer too
(drawn before the second layer, then the scene colour is restored), a lookup
takes it when it lies clearly behind the surface and is another sheet (a
depth walk from the fragment to the sample finds a jump or a gap, so a
sphere never sees its own far side), a 2x2 gather rejects layer edges, and
the material's depth lookups see the same layer so its refracted ray stops at
the pool. What stays brown in the bucket shot is where the stream's
refraction (the solid-thickness model, a sphere of transmission_thickness)
bends the ray up onto the wall above the waterline; a synthetic stream over
a pool shows the pool through the stream end to end.

### MPM fluid: spray, foam and bubbles

Technique: Ihmsen, Akinci, Akinci and Teschner 2012, "Unified spray, foam
and air bubbles for particle-based fluids" (The Visual Computer 28). It is
still the base of FleX, Houdini and Blender FLIP whitewater. The newer
production method, Wretborn, Flynn and Stomakhin 2022, "Guided bubbles and
wet foam for realistic whitewater simulation" (ACM TOG 41(4), Weta FX),
extends it offline (about 15 s to 90 s a frame). We take its cheap parts.

- Spawn (Ihmsen): trapped-air, wave-crest and kinetic-energy potentials,
  each clamped to [0, 1]. On MPM, the neighbours are the 27 grid nodes of
  the B-spline stencil, weighted by each node's liquid fraction. The crest
  curvature proxy is how far the particle sits from the local centre of
  liquid mass. New particles are placed in the cylinder that the fluid
  particle sweeps that frame.
- Trapped air on MPM (17 Nov 2026): each node is compared with the
  particle's own APIC velocity field, v + C (x_node - x), not with v alone.
  Against v alone a smooth falling stream read about 1 m/s across the
  stencil (its shear, stretch and necking), as high as the plunge, and
  filled the pour with white specks. Against the APIC field the stream
  reads under 0.05 m/s and the plunge 0.05 to 0.3 m/s (measured on the
  bucket pour); the clamps are 0.06 to 0.3.
- Sort: by the grid's liquid fraction (Ihmsen's neighbour count): spray
  below 0.12, bubbles above 0.7, foam in between.
- Spray: ballistic.
- Bubbles (Wretborn): carried by the water at the slip speed where
  buoyancy meets their Eq. 5 drag, mu r (6 pi + pi/2 rho r |dv| / mu) dv.
  A 1 mm bubble rises at about 0.16 m/s. Radii follow the inverse-cubic
  size distribution (Deike et al. 2016).
- Foam (Wretborn): advected with the surface and projected back onto it,
  the move capped at half a cell. Lifetimes are normally distributed
  (sd 0.4 of the mean).
- Drawing: foam and bubbles as lit white spheres, as in both papers. Spray
  as clear water drops (the fluid's clear-water material on small spheres):
  airborne drops read by their highlight and refraction, which is how
  Pixar shaded droplets in "Simulating Rivers in The Good Dinosaur"
  (Reisch et al., SIGGRAPH 2016 talk), with scattering volumes only for
  whitewater. White spray read as a curtain of beads.
- Not done (cost): two-way bubble coupling, SPH wet-foam forces, screen
  space or volumetric foam.
- GPU cost: two passes a frame (one over the fluid, one over a 16k ring);
  the PC queue measures it.

### Flow: fire light and embers (campfire)

Paulo picked a campfire showcase (17 Nov 2026): combustion, heat glow lighting
the ground, smoke resting on the slope.

- Fire: NVIDIA Flow's own combustion (fuel burns above ignition_temperature
  into heat and smoke), drawn by the heat ramp.
- Light in raster (`fire_light`, on): the gas's glow lights nothing in
  raster, so a shadowed point light stands in the flames, as engines light
  fire (Unreal's Niagara light renderer, Frostbite). It pulses at the flame's
  puffing frequency, f = 1.5 / sqrt(D) Hz for a base D metres across
  (Cetegen and Ahmed 1993, "Experiments on the periodic instability of
  buoyant plumes and pool fires", Combustion and Flame 93): about 2 Hz for a
  0.4 m campfire. The path tracer keeps it as the flame's proxy light for next
  event estimation: the traced glow lights a surface only when a bounce ray
  happens to hit it, so at a few samples a pixel the ground around the fire
  stayed dark (first PT check). With the proxy on, the glow is dropped on rays
  leaving a diffuse bounce or a smoke scatter, so the fire is not counted
  twice; camera and mirror rays still see it.
- Flame look (docs review: "a soft orange smoke column, no tongues or core").
  Two causes. The default heat ramp rose linearly, so all warm gas glowed
  orange; it is now soot as a blackbody (Nguyen, Fedkiw and Jensen 2002;
  Pegoraro and Parker 2006): colour from the blackbody's chromaticity,
  brightness from Planck's law in Wien's approximation at 580 nm, about
  twentyfold per 100 K, so the flame has a sharp edge and a bright core and the
  cooler gas reads as smoke. And the fire light, standing inside the smoke, lit
  it brighter than the flame's own glow (its near field is inverse square); it
  now scatters at `light_volumetric_fog_energy` 0.1, which the path tracer
  honours too (stored in the light's free word).
- Embers are velocity-aligned streaks (a camera shutter's motion blur), 0.4-1.5x
  sizes, in linear blackbody colours whose brightness falls as they cool.
- Showcase sticks char towards the fuel bed with glowing Voronoi cracks.
- Embers (`embers`, on): GPU particles from the fire's base, drag and a weak
  net gravity with turbulence, coloured along the blackbody colours of a
  cooling ember (about 1,300 K to 800 K), emissive billboards that PT draws.
  They do not read the gas velocity (that needs a Flow readback).
- Smoke on the slope: `smoke_buoyancy` below zero makes cooled smoke heavier
  than the air, and the terrain floor keeps it on the ground.

### Flow: terrain under the gas

Flow solids are boxes, spheres and capsules only, so heightmaps and trimeshes
used to let gas sink into the ground.

- Built (17 Nov 2026, `terrain_floor`, on): solid tiles on the ground under
  the gas, each tilted to the plane through its cell's four corner heights,
  1.15x wide so they overlap. Heights come from the terrain node or a
  static-only ray, are cached and re-read once a second, with at most 32
  tiles a side. Cost: a few KB and under 1 ms of CPU; the GPU time of up to
  1,024 box solids is on the PC queue.
- Limits: no overhangs or caves (it is a heightmap), and bumps smaller than
  a tile are smoothed away.
- Future improvement (Paulo, 17 Nov: document, do not build yet): the exact
  terrain as a narrow-band signed distance field, a NanoVDB emitter or an
  SDF texture the solid test reads.
  - Estimate for a 40 x 40 m dust volume: a +-3-cell shell is about 15 MB
    at 5 cm (4 MB at 10 cm); a dense grid would be about 500 MB.
  - Rebuilding the shell costs about 20-50 ms of CPU every few metres as the
    region moves, so it must run in the background.
  - GPU cost is close to the tiles.
  - Gains: 5-10 cm accuracy (rocks, ruts) and, built from meshes, caves and
    overhangs.
  - Worth it where gas must get under or into things.

### Spreading fire on surfaces (picked 17 Nov)

Goal: a fire that leaves the campfire and runs through grass and litter,
faster downwind and uphill, slowed by wet fuel, leaving char behind, with
the flames and smoke drawn by Flow (raster and path traced).

How others do it:
- Games: Far Cry 2 (Levesque, interview 2008) spreads cell to cell over
  10 m patches, faster downwind and uphill, slower when humid, and caps
  spread for cost. Zelda BotW's chemistry engine (GDC 2017) is a similar
  grid of states. Both are rules, not a rate of spread.
- Graphics papers: Pirk et al. 2017 ("Interactive Wood Combustion for
  Botanical Tree Models", SIGGRAPH Asia), Hadrich et al. 2021 ("Fire in
  Paradise", SIGGRAPH) and Kokosza et al. 2023 ("Scintilla", TOG) burn
  modules of plants coupled to an air grid: moisture must evaporate before
  ignition, mass is lost and char insulates. Interactive, but CUDA-heavy
  and per-plant.
- Wildfire science, run in real time for decades: a level-set fire front
  (Osher and Sethian 1988) moved at Rothermel's rate of spread (Rothermel
  1972, USDA INT-115), as WRF-SFIRE does (Mandel, Beezley and Kochanski
  2011, Geosci. Model Dev. 4). Fuel left after ignition decays as
  exp(-t / T_f), with T_f per fuel; the heat released drives the
  atmosphere above.

Technique: the WRF-SFIRE surface model on a 2D grid over the ground, with
Flow as its atmosphere.
1. Level set phi (negative = burnt or burning) on a grid (0.25 m cells,
   one or two hundred a side), first-order upwind (Osher-Sethian), updated
   only in a narrow band round the front, reinitialised now and then.
2. Rate of spread R = R0 * eta_M * (1 + phi_w + phi_s) along the front
   normal: phi_w from Rothermel's wind coefficients for the fuel's
   surface-to-volume ratio (C, B, E), with the wind along the normal (Flow's
   wind where readable, else a wind vector); phi_s = 5.275 beta^-0.3
   tan^2(slope) from the terrain; eta_M Rothermel's moisture damping, zero
   at the moisture of extinction.
3. Fuel per cell: load (kg/m2), moisture, R0, T_f and sigma from a fuel
   type (short grass, tall grass, litter, wood) or an image map. Burning
   fraction exp(-(t - t_ig) / T_f); char = 1 - fuel left.
4. Coupling: the heat released per cell (-dF/dt * load * heat content)
   becomes Flow emitters over the burning band (a small pool, burning cells
   merged into tiles), so flames rise, lean in the wind and light through
   the existing fire light. Hot gas from Flow (cpu_readback temperature)
   above the ignition temperature ignites cells under it, so a campfire's
   heat or a fallen burning branch can start the ground burning.
5. Look: a char and glow map (texture, global shader uniforms with its
   transform) that ground and vegetation shaders read: black char, glowing
   embers in the burning band, grass hidden where burnt. Path tracing sees
   the same materials and the Flow volumes.
Cost: CPU, narrow band only (a few thousand cells); the map upload is a
small texture. Wood and Blast pieces catching fire, and moisture from the
weather map, are later steps.

Status (17 Nov): `PhysXFireSpread3D` (modules/godot_physx/flow). Found while
building it:
- Unburnt cells starting at a flat far value do not get crossed by the
  front, they relax towards it (dphi/dt = -R (phi - phi_n) / h), about nine
  cells' time per cell. The unburnt side is kept a distance field with
  fast sweeping (Zhao 2005) after each substep, which only lowers phi.
- Projecting the wind onto each front normal (WRF-SFIRE's form) leaves the
  flanks almost still, a needle instead of a grass fire's ellipse. Wind and
  slope factors are summed as vectors into one effective wind (Rothermel;
  Finney 1998, FARSITE), Anderson 1983 gives the length-to-width ratio from
  it, and each normal moves at the ellipse's support function
  c cos t + sqrt(a^2 cos^2 t + b^2 sin^2 t) (Richards 1990, as CSIRO Spark
  does); the ellipse's polar radius is wrong for a level set and slowed the
  head about fourfold.
- Short grass, 6% moisture, 2 m/s wind: head 0.3 m/s, about 30 m2 after
  30 s, fireline intensity up to about 440 kW/m, as BehavePlus gives for
  fuel model 1.

### Flow gas pushes cloth, flags and debris, two-way (picked 17 Nov)

Goal: a fire's updraft, a blast or a gust carried by Flow lifts a flag,
flaps a tarp and rolls light debris, and those bodies push the gas back.

How others do it:
- NVIDIA APEX Turbulence (Batman Arkham City, Borderlands 2, Hawken and
  PlanetSide 2; GDC 2013 "Enhancing Hawken and PlanetSide 2 Through
  Turbulence and Destruction"): a GPU velocity grid whose samples drive APEX
  particles and clothing; moving rigid bodies are collision shapes in the
  grid. That is the real-time pattern: sample the grid's velocity at the
  body, apply aerodynamic force; bodies enter the grid as moving solids.
- Unreal: Chaos Cloth's aerodynamics (per-triangle drag and lift
  coefficients from the relative wind), Niagara Fluids sampling its grid for
  particles; the gas is not pushed by cloth.
- Offline: Guendelman, Selle, Losasso and Fedkiw 2005 ("Coupling water and
  smoke to thin deformable and rigid shells", SIGGRAPH) solve thin shells and
  smoke together; too costly here, but it names the two directions: the shell
  takes the fluid's pressure, the fluid sees the shell as a moving boundary.

Technique: grid sampling plus moving solids, as APEX Turbulence.
1. Flow velocity readback: Flow's NanoVDB export already reads back velocity
   (velocityEnabled, as it does temperature and smoke); add
   `PhysXFlow3D.sample_velocity(position)` beside sample_temperature.
2. Gas to cloth: PhysXCloth3D's existing drag and lift model (per-triangle,
   Keckeisen et al. 2004) takes the wind at each triangle from the Flow grid
   when a flow is set, falling back to its wind.
3. Gas to debris: rigid bodies in the gas get drag
   F = 0.5 rho Cd A |u - v| (u - v) at their centre, u the gas velocity, plus
   hot-gas buoyancy for light ones (paper, leaves); Blast pieces through the
   rocks' piece velocity, centre and mass getters.
4. Debris and cloth to gas: bodies already enter Flow as moving solids
   (collide_with_bodies, with body velocity). Cloth adds a few sphere solids
   along its particles carrying their velocity, so a flapping flag stirs the
   smoke.
Cost: a velocity readback (a few frames late, as the others) and a few
hundred samples a frame on the CPU.

Sources: https://gdcvault.com/play/1017851/ ;
https://docs.nvidia.com/gameworks/content/gameworkslibrary/physx/apexsdk/APEX_Turbulence/Turbulence_Module.html ;
http://physbam.stanford.edu/~fedkiw/papers/stanford2005-02.pdf ;
https://dev.epicgames.com/documentation/unreal-engine/niagara-fluids-in-unreal-engine

Status (17 Nov): built as above (fc47ebb3). Found while building it: a body
sampling the grid inside its own solid reads its own velocity back, so it
never moves; the gas is sampled on a six-point shell 1.5 cells outside the
shape. Area is the shape's surface over four (Cauchy's projected area), and
the impulse is capped at the speed difference so light bodies do not
overshoot the gas. A 0.5 m crate in a campfire updraft moved 0.44 m, a flag
streamed out; neither moved with coupling off.

### Next options (card, 17 Nov)

1. Burning wood and debris. Far Cry 2's fire (GDC 2008, Levesque) and
   Breath of the Wild's chemistry engine (GDC 2017, "Breaking Conventions")
   give objects a fuel amount and an ignition temperature and spread
   between neighbours; offline, Melek and Keyser 2002 move a burn front
   over the mesh. Here: per-vertex fuel and char on a body, heat read from
   Flow (sample_temperature) and from PhysXFireSpread3D, a Flow emitter on
   the burning part, char in a vertex colour PT reads. Embers: Albini's
   spotting distance (1979) with Flow's updraft carrying lofted embers that
   ignite cells where they land.
2. Weather wets the fire. FARSITE and WRF-SFIRE take dead fuel moisture
   from Nelson's model (2000): each size class relaxes towards an
   equilibrium moisture from air temperature and humidity with a time lag
   (1 h fine fuel), and rain wets it directly. WeatherMap3D already has
   rain, temperature and humidity; a moisture grid in the fire node would
   follow them, and rain over burning cells cools them out.
3. Fire burns grass and trees. Far Cry 5 and Unreal's landscape grass
   both read a mask in the foliage shader; here the vegetation and tree
   shaders read the existing fire_char_map and fire_char_rect globals:
   grass shortened and black where burnt, a scorch height on bark from
   flame length (Byram), leaves browned in the band above it (crown scorch
   height, Van Wagner 1973). No new state, both renderers see it.
4. Open-world checks (item 4 of the order of work): PxPruningStructure for
   streamed static regions, a pool for Blast and debris bodies, sleep and
   removal by distance, relative origin; a bench with thousands of static
   bodies per region, as Mortal Plane needs.

Sources: Nelson 2000, "Prediction of diurnal change in 10-h fuel stick
moisture content", Can. J. For. Res. 30; Van Wagner 1973, "Height of crown
scorch in forest fires", Can. J. For. Res. 3; Albini 1979, "Spot fire
distance from burning trees", USDA INT-56; Melek and Keyser 2002,
"Interactive simulation of fire"; GDC 2017 "Breaking Conventions with The
Legend of Zelda: Breath of the Wild".

### Fire burns grass, plants and trees (picked 17 Nov, first of four)

What a grass fire leaves (fire ecology field descriptions; Van Wagner 1973;
Byram 1959): the front consumes the fine fuel from the top down, leaving
black stubble a few centimetres tall with patches of white ash; small
plants are consumed to their bases. On trees the bark is charred to about
the flame length, higher on the lee side where the flame wraps the stem,
and above it the foliage is killed (scorched, browns within days) up to
the crown scorch height, where the plume cools to about 60 C:
hs = 4.483 I^(7/6) / ((0.107 I + U^3)^(1/2) (60 - T)) (Van Wagner 1973; I
fireline intensity kW/m, U wind m/s, T air temperature C), as BehavePlus
and FARSITE use. Foliage below the flame is burnt black.

How games show it: Far Cry 2 and 5 swap burnt grass to black stubble and
char the ground through a mask the foliage shader reads; Unreal landscape
grass reads a runtime virtual texture or render target the same way.
Nothing simulates the plants themselves.

Technique: a state map the fire node publishes beside the char map
(`fire_state_map`, sharing `fire_char_rect`): fuel burnt, glow, the peak
flame length and the peak crown scorch height each cell has seen (Byram's
flame length from the front's fireline intensity, Van Wagner's scorch
height from the same intensity and the wind). The vegetation shaders read
it, with no state of their own:
1. Grass blades burn down to 3-5 cm stubble, char black with ash flecks;
   the burning band's blade tips glow.
2. Small plants: tissue under the flame is charred, the rest scorched
   brown; the burning band glows.
3. Trees: bark charred up to the flame length with a ragged edge, leaves
   under the flame black, leaves up to the scorch height brown.
The vegetation module declares the globals, so its shaders compile with or
without PhysX; raster and the path tracer run the same lines.

Status (18 Nov): built. Found while building it:
- The path tracer reruns vertex() for a merged grass MultiMesh with the
  field's transform, not each blade's, so anything read there at the
  blade's base came out for the field's origin (every blade burnt). Grass
  and plants read the fire in fragment() at the point's own position.
- Seen from below, a crown is mostly light through the leaves: scorched
  leaves kept the species' green backlight until the backlight took the
  leaf's new colour too.
- The ground's char decal glowed red up tree trunks standing in its box;
  it now fades on upright faces (normal fade).
- Patchy fuel (`fuel_patchiness`, scene's request): metre-scale spread
  rate and load noise, damp patches and bare gaps make an irregular
  ellipse; the char edge is ragged at tuft scale (a world-space noise on
  the level set's coverage). A patchy bed burns about a third less area
  in the same time than an even one, as discontinuous fuel does.

### Weather wets the fire (picked 17 Nov, second of four)

How fire models do it: FARSITE, FlamMap and WRF-SFIRE do not hold dead fuel
moisture fixed; each fuel size class relaxes towards an equilibrium
moisture content (EMC) set by air temperature and humidity with its time
lag (1 h for fine fuel), and rain wets it. WRF-SFIRE (Mandel et al. 2014,
Kochanski et al.) uses the Canadian equilibrium curves of Van Wagner and
Pech (1985), a drying EMC above a wetting one with no change between them:
Ed = 0.942 H^0.679 + 11 e^((H-100)/10) + 0.18 (21.1 - T)(1 - e^(-0.115 H))
Ew = 0.618 H^0.753 + 10 e^((H-100)/10) + 0.18 (21.1 - T)(1 - e^(-0.115 H))
(%, H relative humidity %, T air C), and a rain term up to a saturation of
about 2.5 times the dry weight. Nelson's model (2000) is the physical
alternative, heavier than a game needs.

Technique: the fire node's moisture follows the active WeatherMap3D.
1. Base moisture: starts at `fuel_moisture`, moves to Ed or Ew from the
   map's air temperature and humidity with `moisture_timelag` (1 h; games
   can shorten it). Per-cell patchiness still multiplies it.
2. Rain: WeatherMap3D.get_rain_at on 4 m blocks; a fine fuel bed holds
   about a third of the rain on it (interception, 0.2-0.5 mm), as water
   per kg of fuel (mm/h is kg/m2/h over the load), drying several times
   faster than the fuel. Wet fuel slows the front through Rothermel's
   moisture damping and stops it at the moisture of extinction.
3. Burning cells wetter than the extinction moisture go out: no more
   heat or flame, a few dull embers; what was left stays unburnt.
4. Crown scorch takes the map's air temperature (Van Wagner's 60 C
   lethal temperature less the air's).
Cost: a few dozen weather samples a quarter second, one pass over the
grid.

Status (18 Nov): built. Checks (short grass, 2 m/s wind, 60 s time lag):
dry air (30 C, 20%) keeps it burning, about 70 m2 in 30 s; humid air (15 C,
90%) wets the fuel past its 12% extinction and the fire dies at about 11
m2; 4 mm/h of rain from 10 s raises the fuel from 7% to 12% in 20 s, and the
front stalls and goes out.

### Burning wood and debris, embers spot new fires (picked 17 Nov, third of four)

How games do it: Far Cry 2 (2008) spreads fire through objects and grass
cells that each hold fuel and catch from heat nearby, with the wind; Breath
of the Wild's chemistry engine (GDC 2017, "Breaking Conventions") gives
objects a material that burns, an element state passed between neighbours
and burning embers that drift and light grass. Both are rules, not
physics; the look is a flame on the object and char spreading over it.

The physics to borrow:
- Ignition of wood under a heat flux (Babrauskas 2001, "Ignition of wood:
  a review"): piloted ignition from about 12 kW/m2; a thick solid ignites
  after t_ig = (pi/4) k rho c (T_ig - T0)^2 / q^2 (Quintiere, Fundamentals
  of Fire Phenomena), with k rho c about 1.2e5 for wood and T_ig about
  350 C: some 10 s at 30 kW/m2, a minute at 13. The dose builds with q^2
  and leaks away when the heat drops.
- Burning: flame spreads over wood at millimetres to centimetres a second;
  burning wood gives off about 100-200 kW/m2 (cone calorimeter) and chars
  inwards at about 0.65 mm/min (Eurocode 5, softwood). Heat of combustion
  about 15 MJ/kg.
- Radiation to neighbours: a point source, q = chi_r HRR / (4 pi d^2),
  chi_r about 0.3 (Modak's point source model, SFPE Handbook).
- Embers (firebrands): lofted by the plume and carried by the gas, they
  burn out in seconds to tens of seconds; Albini (1979) gives spotting
  distances from the plume's height and the wind. Manzello et al. (2006,
  2008) found glowing firebrands ignite dry grass and litter beds (below
  about 11% moisture), flaming ones more readily, damp beds not at all.

Technique: a `PhysXBurnable3D` node on a prop, log, crate or Blast piece.
1. Heat in: the ground fire under it (PhysXFireSpread3D intensity and flame
   length at its base), Flow hot gas at its bounds (sample_temperature),
   and radiation from other burning burnables. Ignition by the dose model
   above, at the hottest point (the char starts there).
2. Burning: the burning area grows from that point at a set flame spread
   speed over the object's surface (its bounds), heat release 150 kW/m2
   of burning area, fuel from its mass (or set), mass loss at the heat of
   combustion; one or more Flow sphere emitters at the burning area feed
   the flame (the same combustion and light as the ground fire). It burns
   out to embers, then cold char.
3. Look: the meshes under it get their materials' shader with instance
   uniforms: char spreading from the ignition point with a ragged edge
   (distance plus a noise), the char front's band glowing, and glowing
   cracks in the char while it smoulders (raster and path tracer both
   read instance uniforms).
4. Embers: while flaming, it releases embers at a rate with its heat
   release; each is carried by Flow's gas velocity (sample_velocity) with
   drag and gravity, glows and burns out over seconds, and where it lands
   on a PhysXFireSpread3D's ground it may light it, more likely as the
   fuel is drier. Drawn as small emissive particles (a MultiMesh).
5. Rain (the weather step) puts it out as it does the ground.
Cost: a few samples per burnable a tick, a few hundred embers.

Status (18 Nov): built as above (1192aad2). Checks: crates in a grass fire
catch at about 30 s of flame contact and reach about 320 kW by 50 s; a crate
off the fire's path stays unburnt. The first char cracks (the difference of
two noises) read as lava worms; they are now the cell edges of a Voronoi
pattern, as alligator char splits into blocks, thin and dull red to orange,
faded by the pixel footprint so they do not alias at a distance. Embers stay
few without Flow's updraft (headless runs). Raster and path tracer both keep
the char and cracks.

Look rework (18 Nov, after docs review: the char read as a printed crackle
over whole faces, the front as a cream rim, the flames sat beside the
object). How burning wood looks, from fire investigation (Babrauskas 2005,
"Charring rate of wood as a tool for fire investigations", Fire Safety J.
40) and the timber fire design code (EN 1995-1-2, about 0.65 mm/min
charring): behind the front the char deepens with time and splits into
alligator blocks, smooth and fine-checked when fresh, larger and deeper
behind; at the front a thin glowing line; ahead of it the pyrolysis zone,
browned and darkened wood; then unburnt wood. A flame on a wall attaches
to it and climbs (the wall fire plume). Built that way: the shader's
distance behind the front stands for the char depth, cracks open with it
and vary over the surface with a low noise, crack glow only near the
flame and in smouldering patches, the front is a thin broken orange line,
and a band of scorched brown leads it. The single Flow emitter inside the
box (masked by the body as a solid) became one emitter just outside each
burning face (sides and top) over the part the patch covers, pushed up.

### Open-world physics checks (picked 17 Nov, fourth of four)

What the module does now: one PxScene per space with the default scene
query trees (static and dynamic both eDYNAMIC_AABB_TREE, rebuilt in the
background over about 100 steps) and the default CPU broad phase (PABP in
PhysX 5.10), or the GPU broad phase when CUDA is up. Each body goes in with
its own addActor the moment it enters the space. Blast pieces already have
piece_lifetime and kill_y; nothing else pools or culls bodies.

How it is done elsewhere:
- PhysX's guide (scene queries, "Pruning structures"; "Origin shift"):
  for streamed content, build a PxPruningStructure for a batch of static
  actors (at load or off the main thread) and add it with
  addActors(pruningStructure): the batch's query tree merges into the
  scene's in one step instead of each actor dirtying it. addActors with an
  array is cheaper than single adds for the broad phase too. Static
  aggregates (PxAggregateType::eSTATIC) put a region's statics into the
  broad phase as one box. shiftOrigin moves every object by a vector, cost
  O(n), meant to be called rarely as the player travels.
- Unreal 5: physics state for a streamed level is created over several
  frames (async physics state creation, a time budget per frame); Chaos
  runs in doubles with Large World Coordinates, so it dropped origin
  rebasing. Destruction debris: lifetime and "remove on sleep", with users
  reporting it does not stop frame drops, so pooling is the game's job.
- Unity (PhysX 4): no origin shift; projects run a floating origin script.

Checks, in order (cloud CPU first; GPU numbers go on the PC queue):
1. Bench (docs/bench/physx_openworld): regions of 500 m with a few
   thousand static bodies each (boxes, convexes, trimeshes); stream one in
   during a frame, measure that frame, the next steps, and raycast cost
   before and after the query tree settles. The same with debris: a few
   thousand dynamics settling, sleeping, then far away.
2. Batched adds: bodies entering a space during a frame wait in a list and
   go in with one addActors before the step; a batch of many statics goes
   in through a PxPruningStructure. Keep it only if the bench shows the
   spike drops; no behaviour change either way (same actors, same step).
3. Sleep and distance: bodies beyond a distance from the active camera stop
   simulating (eDISABLE_SIMULATION) and resume when back in range, as an
   opt-in setting first, since game logic may rely on far bodies.
4. Precision: PhysX runs in floats, so at 10 km a step's position step is
   about 1 mm. Measure jitter of resting bodies at 1, 10 and 50 km. If it
   shows, add an origin shift through PxScene::shiftOrigin that follows the
   renderer's relative origin.

Status (18 Nov): bench in docs/bench/physx_openworld (run headless with
--fixed-fps 60, so each frame is one step). Cloud CPU numbers:
- Streaming: a region of 5,000 statics costs one hitch frame, about 15 to
  20 ms creating the nodes and shapes plus 6 to 12 ms for the first step,
  growing with the statics already loaded (merging into the query tree and
  broad phase). After it, steps are about 0.15 ms with 20,000 statics:
  statics cost nothing while nothing touches them. 20,000 raycasts take
  about 12 ms either way. Unloading 3,000 takes about 18 ms (one removal
  each). The Performance monitor TIME_PHYSICS_PROCESS is a once-a-second
  average, so it smeared the hitch over a second in the first runs.
- Batched adds built (statics wait for the next step or query and go in
  with one addActors, through a PxPruningStructure from 64): the hitch
  frame drops about 12% (21 to 31 ms against 24 to 36 ms). Kept: same
  actors, same step, queries in the same frame still see them.
- Debris: 2,000 boxes settle and all sleep within 3 s; asleep, the step is
  about 0.2 ms. Sleep already does what distance culling would, so culling
  by distance is only worth it for bodies that never settle (it stays a
  later opt-in).
- Precision: resting boxes at 1, 10 and 50 km neither drift nor jitter.
  Moving ones are bounded by float spacing: about 1 mm at 10 km and 4 mm at
  50 km, so an origin shift is only needed for fine motion past 10 km.
Next, if the hitch matters on the PC: create a streamed region's bodies
over several frames (Unreal's time-budgeted physics state creation), and
batch removals the same way. GPU broad phase numbers go on the PC queue.

### Next-round options (card posted 18 Nov)

Logs and deadwood burn. Trees mark deadwood meshes (PlantSpecies meta
"deadwood", mesh meta "tree_deadwood_axis"); a PhysXFireSpread3D could
attach a burnable to each such mesh in its area itself, so fallen trunks
and stumps catch from the ground fire with no setup. A log is a cylinder,
not a box: char by distance along and around the axis, flames along its
top line. Large logs smoulder for hours in the wild (heavy fuels, the
1000-hour time lag class); here, a long smoulder with glowing cracks.

Cloth burns and tears. Melek and Keyser (2003, "Interactive simulation of
burning objects"; 2007, "Driving object deformations from internal
physical processes") burn a surface by spreading a front over its mesh,
charring ahead of it and removing material behind; thin fabric burns
through, leaving a hole with a glowing, curling edge. Here: a front over
the cloth's particles (heat from the fire's flux, as burnables take it),
char in the cloth shader by the particle's burn state, and burnt-through
triangles removed through the cloth tearing already built, so the cloth
falls apart under its own weight and the hot gas pushes it.

Status (18 Nov, deadwood): built in the fire node rather than as burnables
per log, since logs are MultiMesh instances in the field: at grid build, each
cell under a log (get_deadwood_at, side below 0) takes large wood, 500 kg/m3
times pi r^2 over the log's 2r width. It ignites while the fine fuel under
it flames and is dry enough, flames for Anderson's 384/sigma minutes
(18,900 d seconds, d in m; a 68 cm beech trunk about 3.6 h), adding its heat
to the cell so the Flow flames stay over it, then smoulders below 40% left
at a third of the rate with a patchy glow. The bark shader takes that glow
from fire_state_map in its furrows within half a metre of the ground. Rain
quenches a flaming log to a smoulder.

Status (18 Nov, cloth): PhysXClothBurn on the CPU cloth solver. Each
particle takes heat (flame contact from the fire under it, 60 sqrt(I/600)
kW/m2 up to 60; hot Flow gas 80 t^2; burnables' radiant flux) and cools
below 10 kW/m2; thin fabric ignites at an areal dose of 0.26 kg/m2 times
380 K, about 99 kJ/m2 (thermally thin ignition). Flame spreads over the
mesh's own neighbour graph at flame_spread (1 + 7 up^2), so it climbs a
hanging curtain fast and creeps down; each particle burns through after
burn_through_time (0.7 to 1.3 of it). The shader eats the fabric along a
noisy threshold on the interpolated burn (a dissolve with a glowing rim), so
holes are round; a triangle and the constraints it held leave the solver
once all three corners have burnt through. Char, glow and scorch go to vertex colour,
read by a material override built from the cloth's own. Up to four Flow
emitters sit over the burning clusters (farthest-point picks), sized small
so the fabric shows. SIM_AUTO takes the CPU path while any fire exists.

Smoother streaming. Unreal registers a streamed level's components over
several frames within a budget (s.LevelStreamingComponentsRegistration
Granularity and the streaming time limit). Here: bodies entering a space
already wait in a batch; creating their PhysX shapes and actors could also
be split over frames within a time budget, with queries flushing the rest
first so nothing is missed, and removals batched with removeActors.

Status (18 Nov, streaming): built. Statics waiting to enter keep their world
bounds; a step adds statics_per_step of them (1024, oldest first) plus any
whose bounds touch an awake dynamic or kinematic actor grown by half a second
of travel (areas skipped; past 512 movers, all at once). Queries add the
waiting statics their bounds reach (ray segment, point, shape bounds swept
along the motion, the body swept for test_body_motion); past 32 such queries
in a step they add everything. A static moved or reshaped while waiting
refreshes the bounds. Static bodies leaving are removed in one removeActors
before the next step or query (userData cleared so nothing reports them).
Cloud bench (5,000 statics a region, 20,000 total), worst of the first ten
frames: 4 to 6 ms against 6 to 9 ms for the first three regions, about the
same at 20,000. Most of what was left was not the adds: with nothing added in
the region's first step the step still cost 1 to 8 ms, growing with the
statics already in the scene.

Status (18 Nov, profile): a PhysX profile build (zones are compiled out of
release; a PxProfilerCallback summing zones per step, plus zones added
round the unzoned start of NpScene::simulateOrCollide) put it in
syncMaterialEvents: 3.5, 5 and 7.8 ms for regions 1 to 3. Every body made
its own PxMaterial, and each new material is an event every scene applies
at its next simulate (growing with the materials already there); PhysX
material handles also stop at 65,535, which an open world passes. Bodies
now share one material per (friction, bounce), reference counted, the last
release deferred as before; changing a body's friction moves its shapes to
the matching material. Bench after (worst of a region's first ten frames,
5,000 statics a region): all at once 5 to 8 ms, flat with world size;
spread (default) 3.4 to 4.5 ms, flat (before: 6 to 14 ms, growing).
Spreading has one cost: statics added in pieces go into the incremental
part of the static query tree, so raycasts run about 1.5 to 2 times slower
until PhysX's progressive rebuild finishes a few seconds later (measured
back to normal 600 frames on). Building one pruning structure per piece was
no better; forcing a rebuild after the last piece fixes it at once but
costs 0.6 to 1.9 ms at 5,000 to 20,000 statics and grows with the world, so
it is left out. The game side (creating the nodes, 15 to 20 ms a region) is
now the larger part: stream nodes in over frames.

### Embers start new fires downwind (picked 18 Nov, second of three)

Spotting is how wildfires jump roads and breaks: burning bark, twig and
grass flakes rise in the plume, the wind carries them, and a glowing one
landing in dry fuel starts a new fire ahead of the front. The physics:
- Lofting: the convection column over a line of fire rises at about
  1.7 (g I / (rho c_p T))^(1/3) (Byram 1959; Lee and Emmons 1961), about
  5 m/s for 1,000 kW/m, holding with height while the source looks like a
  line, then slowing as z^(-1/3) in still air and z^(-1/2) bent over by the
  wind (Briggs 1975); it widens by about 0.2 z (Morton, Taylor and Turner
  1956) and leans downwind by U/w per metre of rise. Albini (1979, 1983)
  builds his spotting distance models on the same column.
- Flight: quadratic drag to a terminal velocity of 2 to 6 m/s for bark and
  twig flakes of 2 to 10 mm (Tohidi, Kaye and Bridges 2015), falling as
  the brand loses mass; glowing life grows with size (Tarifa et al. 1965).
- Generation: burning trees and shrubs throw hundreds of hot brands per kg
  burnt, most needles and leaves too small to light fuel (Caton-Kerr et al.
  2021); wood and bark throw the long lived ones.
- Ignition: glowing brands light dry grass and litter, damp beds not at
  all (Manzello et al. 2006, 2008). BehavePlus uses Schroeder's (1969)
  probability of ignition from the heat to ignite the fuel, Q_ig, from its
  moisture and the air temperature.
How engines do it: Far Cry 2 and Breath of the Wild drift embers that light
grass by rule; WRF-SFIRE and FARSITE add spotting as Albini's distance with
a probability. Here the brands fly, so the distance comes from the plume
and the wind.

Status (18 Nov): built. PhysXFirebrands (shared, so burnables can use it
next) holds up to 400 brands. A PhysXFireSpread3D throws them from its
flaming cells (2, 3 and 5 per kg for short grass, tall grass and litter,
10 for burning logs, times spotting_scale), lofted by Flow's gas in the
flames and the column above its 128 strongest front tiles, carried by the
weather map's wind at their height (or the wind on a log profile), landed
by a ray along their path or the fire grid's ground. On unburnt fuel of any
fire grid below its moisture of extinction a brand lights a spot fire with
Schroeder's probability times sqrt of what is left of it. Drawn as short
glowing streaks along their flight. Test (tall grass upwind, short grass
downwind past a 5 m gap, wind 8 m/s): brands land 15 to 40 m downwind and
light 12 spot fires in 10 s at 4% moisture, 3 at 10%, none above 12%.

### Rain soaks cloth (picked 18 Nov, third of three)

What happens to fabric in rain, and the numbers:
- Uptake: a woven fabric holds about its own weight of water once soaked
  (cotton about 1 kg/kg after dripping, wool and towelling more, polyester
  about 0.3); past that it drips. Rain is mm/h = kg/m2/h on the ground; a
  surface catches it by how it faces the drops' slant in the wind (drops of
  2 mm fall at about 6.5 m/s, Gunn and Kinzer 1949), so a sheet facing the
  wind wets almost as fast as a flat one and one edge on only through gusts.
- Drying: each face evaporates as open water does while the weave is wet
  (the constant rate period), Penman's (1948) wind function 0.26 (1 + 0.54
  U) mm a day per hPa of vapour pressure deficit, then slower as the last
  water is held in the fibres (the falling rate period).
- Weight and drape: water doubles a cotton sheet's mass and plasticises
  the fibres, about halving bending stiffness, so wet cloth hangs straight
  and flutters less.
- Cling: a water film between fabric and skin or a surface pulls them
  together (capillary adhesion) and raises the friction (Gerhardt et al.
  2008: fabric on skin roughly doubles when wet).
- Look: water in the pores cuts the light that leaves the weave, a (1 -
  r) / (1 - a r) with r about 0.6 for water (Lekner and Dorf 1988, "Why some
  things are darker when wet"), which is what Lagarde's (2013) wet surfaces
  in games use; dark colours darken most. Only a soaked surface gets a film
  and a little gloss.
- Fire: wet fabric holds at 100 C until its water has boiled off (2.6 MJ
  per kg from 20 C), and thermally thin flame spread goes as one over the
  heat to bring the fabric ahead to ignition (de Ris 1969).
How games usually do it: a wetness value that darkens the material and
dries on a timer; the cloth's motion stays the same.

Status (18 Nov): built on the CPU cloth solver. PhysXCloth3D wetness (0 to
1 of water_capacity times its density) rises with the weather map's rain
over it (its rain fraction, the canopy, a roof ray above it) times its
catch, |n . d| / |d.y| over the cloth's triangles for the wind's slant plus
gust scatter of 0.3 U + 0.5 m/s, and falls by Penman's evaporation for the
map's air temperature, humidity and the cloth's wind (both faces, slowed
below a fifth). Wet: the solver's masses take the water's weight, bending
compliance follows half the stiffness at soaked, collisions cling (a film
gap of 1.5 vertex radii times wetness pulls the fabric on and damps it
peeling off; friction toward 1), the material darkens by Lekner and Dorf,
and the burn needs the water boiled off before the dose builds; flame
spread slows by the same heat. Tests (5 mm/h, wind 3 m/s, 15 C, 70%): a
flat sheet soaks in 2.5 min, a sheet facing the wind in 6, one edge on is
at 0.7 after 10; it dries at 6.5e-5 kg/m2/s, about 50 min; a soaked sheet
dropped on a ball stays on it where the dry one slides off; in a tall grass
fire the dry sheet lights at 2.3 s, the soaked one at 18.7 s.

Follow-up (18 Nov, after docs declined the first pair: sheets read as
boards). Three causes, all in the cloth: the wind's gusts were one vector
for the whole cloth, so a sheet tilted as a plane; there are now eddies
across it, two scales of smooth noise carried along by the mean wind
(Taylor's frozen turbulence) at 0.6 of wind_turbulence times the speed,
fed to the solver as per-particle air when no Flow gas drives it. Cloth
fell through thin colliders (a rope, a rail) because only overlaps were
tested; particles are now also swept along their step's path and stopped
where it meets a surface. That exposed explicit quadratic drag blowing up
when a sheet is stopped short; drag is now implicit in the normal speed.
And laundry hangs from pegs set closer than it is wide: move_pins() holds
pinned vertices at new places, so it falls in swags and folds. Pegged
sheets in a 2.2 m/s gusty breeze: dry ones billow out, soaked ones hang
about 20% longer and flutter less. On a 30 degree slope a soaked sheet
slides 0.18 m in 5 s against a dry one's 0.42 m.

Worn clothing (18 Nov): PhysXSkinnedCloth3D shares the model (ClothWetness:
rain from the map, roof ray, Penman drying). A garment on a moving figure
faces every way, so its catch is an upright cylinder's (rain through its
width at the drops' slant spread over pi times that width of fabric) on 85%
of it plus all that falls on the shoulders' 15%. Wet: the added weight
divides the push of extra_acceleration (wind), the damping and collider
friction rise, and a slight drive toward the skinned pose stands in for the
film holding it to the body (its effect grows with substeps: position
corrections per substep against a gravity step of dt squared, so it stays
small, 0.002 per frame at soaked). The render copy's shader darkens it by
Lekner and Dorf. Tests: 30 mm/h with 3 m/s wind soaks a skirt in 106 s (5
mm/h: about 11 min), dries about 2% a minute; under a 3 m/s2 sideways push
a soaked hem leans 1.1 cm against a dry one's 3.6, and with the hips
swaying 12 cm at 1.5 Hz it swings 3.2 cm against 4.4.

### Trees catch fire from a ground fire (picked 18 Nov, second of three)

How fire models do it: FARSITE, FlamMap and BehavePlus use Van Wagner's
(1977) crown fire initiation: a surface fire lights the crown when its
fireline intensity reaches I0 = (0.010 CBH (460 + 25.9 FMC))^1.5 kW/m (CBH
crown base height m, FMC foliar moisture %): about 480 kW/m for a crown 2 m
up at 100%, 2,500 at 6 m. Crowns lit one by one is passive crown fire
(torching); a stand dense enough carries it crown to crown (active, Van
Wagner's R0 = 3 / CBD m/min, CBD the canopy bulk density, typically 0.05 to
0.3 kg/m3, Cruz et al. 2003). A torching tree's flame stands about 1.5 to 2
times its height for tens of seconds (Albini 1979, "Spot fire distance from
burning trees"); the NIST burns of whole conifers (Mell et al. 2009,
Manzello et al. 2007) show heat release peaking about 10 s after ignition,
most fine fuel gone by 30 to 40 s, and a shower of brands, more than any
surface fire. Games: Far Cry 2 and 5 set trees alight as props with fixed
flame effects; none we found models the initiation threshold.

Technique: PhysXFireSpread3D keeps a list of crowns, from its
vegetation_field (VegetationField3D.get_crowns_in: position, radius, top,
the crown base as the 5th percentile height of the mesh's leaves or needles,
conifer, opacity) or add_tree(). Crown fine fuel is the crown's volume (cone
for conifers, paraboloid for broadleaves) at 0.5 or 0.25 kg/m3 inside the
crown, less for a sparse one. Each step:
1. An unlit crown lights when the strongest half-metre tile of the surface
   front within reach of its stem passes I0, with FMC from foliar_moisture
   (broadleaves 1.6 times as wet), or when a burning neighbour's flame
   (1.5 times its height, leaning by U / sqrt(g Hf)) reaches across the gap
   between crowns, growing over its first 4 s. It lights the ground under it.
2. A lit crown burns as a gamma curve, 1 - (1 + t/T) e^(-t/T), T = 8 s.
3. Its heat feeds a Flow emitter filling the crown (burning crowns take up
   to half the emitters), a plume (Byram's speed over its width) that lofts
   its brands, and 20 brands per kg (MAX_BRANDS raised to 1,000).
4. Plants see it through fire_state_map: the crown footprint's flame length
   rises from the crown base to 1.3 times its top as it flares (foliage
   charred black), scorch out to 1.6 radii, a glow while it flames. The map
   now stores the square roots of flame length / 32 m and scorch / 48 m, so
   tall trees fit and grass flames keep their detail in 8 bits.

Status (18 Nov): built. Test (tall grass at 5%, 4 m/s wind, 8 to 10 m
conifers): crowns with bases at 0.5 and 2.5 m and a broadleaf at 2 m
torch; a crown base at 6 m does not; a row of crowns 0.6 m apart with bases
at 7 m (too high for the ground fire) burns tree to tree downwind from a
low-crowned one, the same crowns 8 m apart do not. With trees the fire has
400 brands aloft against 60 without. Render: a 9.5 m pine torches with
flames about twice its height and a smoke column; afterwards its needles
are black and a neighbour's scorched brown. Found on the way: conifer needles
used a plain material with no fire code, so pines and spruces never charred
or scorched in any fire; needles now go through the leaves' shader with
flutter and the paler underside off (raster and path traced frames of an
unburnt pine match the old ones, mean difference under 0.1 of 255).

### Burning props throw firebrands (picked 18 Nov, third of three)

PhysXBurnable3D's embers were sparks that rose in Flow's gas and could
light the ground where they fell, with no plume above Flow's grid and no
wind. It now also throws PhysXFirebrands, the ground fire's brands: about
10 per kg of wood burnt (as the fire node's logs), released over its top,
lofted by its own column (Byram's speed for its heat release over its
width), carried by the weather map's wind (or its fire's), and landing with
Schroeder's ignition probability. The sparks stay for the look and no
longer light anything. Test: a 2 x 1.5 x 1.2 m woodpile reaching 2 MW in a
6 m/s wind (28 C, 25%) started 14 spot fires in dry grass 4 to 44 m
downwind in 90 s, which burnt 420 m2.

### Next-round options (card, 18 Nov evening)

1. Embers light props and buildings. Most homes lost in wildland-urban
   interface fires are lit by embers, not the front (Cohen 2000; Manzello
   et al. 2008 and 2020, NIST ember storms: brands collect in corners,
   gutters and on decks, smoulder in beds of litter and light them).
   Here: a brand landing on a PhysXBurnable3D adds its glow to the
   surface's heat dose where it rests (pile-up in corners and on top
   faces), so a woodpile, a fence or a cabin catches downwind of a fire.
2. Water puts fires out. Fire models take water as moisture added to the
   fuel (WRF-SFIRE's rain term, already used for rain); suppression drops
   coat the fuel ahead (FARSITE's suppression, Fire Dynamics Simulator's
   water spray: droplets absorb 2.6 MJ/kg). Here: MPM water and spray
   landing on the fire grid wet its cells (quenching burning ones) and
   cool burnables, so buckets, hoses and a burst pipe stop a fire.

   Status of 1 (18 Nov): built. A brand whose flight ray hits a
   PhysXBurnable3D's collider rests on it: where it lands on a face that
   faces up, at the foot of the wall for one that strikes a wall (the
   re-entrant corner); within 25 cm brands share one pile. A pile's glow
   (brands weighed by the square root of what is left of each, fading over
   20 s) heats the wood under it at 50 kW/m2 for 20 brands in a 6 m/s wind
   (Hakes et al. 2019 measured 40 to 80 under piles), less in still air;
   the burnable's ignition dose takes it like any other flux. Brands are
   now cast against walls while flying flat as well as falling. Near
   MAX_BRANDS aloft each new brand stands for several (a crown fire throws
   thousands), so landings, piles and spot fire chances count them all.
   Test (7 m/s, 32 C, 20%): a 3 x 4 x 2.4 m wooden shed 10 m downwind of a
   burning stand of 24 conifers gathered about 40 brands and caught at
   about 35 s; downwind of a grass fire alone it held a dozen and did not
   catch; with spotting off, none.
   Status of 2 (18 Nov): built. PhysXParticleFluid3D keeps a list of the
   liquid fluids in the world (granular subclasses left out) and hands out
   its particles' positions without a GPU wait (MPM: the render buffer it
   already reads back). Every quarter second PhysXFireSpread3D bins the
   particles lying on its ground (up to 0.4 m above a cell) and soaks each
   cell's fine fuel by the water on it, up to 2.5 times the fuel's dry
   weight as rain does; a burning cell over the moisture of extinction goes
   out (Rothermel 1972), and wet cells ahead of the front do not take it.
   With no weather map to dry it, the water dries at the fuel's time lag.
   PhysXBurnable3D counts the particles in its box, 10 cm grown, as a film
   over its surface (kg/m2 = mm): wet wood takes no ignition dose, a
   flaming surface damps and goes out at 1.5 mm (the rain rule), and the
   ember piles on it and the embers in its char drown. Water stands for
   cooling and wetting together; the FDS-style droplet heat sink (2.6
   MJ/kg) is left to the moisture rule, which is what the fire models use.
   Test (3 m/s wind, tall grass, 5%): a band of water 3 x 6 m laid across
   the path stopped the front, 75 m2 burnt against 120 m2 with none past
   the band; a 1 m crate burning at 39 kW went out a second after about
   a 0.6 x 0.6 x 0.3 m block of water fell on it.
   The same band dropped onto the burning
   patch behind the head put out 181 burning cells; the head, already past
   it, ran on.
3. Burnt crowns shed their needles. After a crown fire the fine fuel is
   gone: bare black branches, needles fallen as a grey ash layer, and
   burning twigs falling while it torches. Here: the tree shaders cut
   leaves by the crown's burnt share (alpha), the fire state map darkens
   the ground under it, a few PhysX debris twigs fall burning.

   Status of 3 (18 Nov): built. The tree leaf shader (needles too) drops
   foliage the flame passed through once the crown has burnt (the state
   map's burnt share over 0.55 to 0.9), leaf by leaf on a per-leaf hash,
   so a torched pine stands as bare black branches; killed brown foliage
   above the flame stays (it falls over months, not minutes). Alpha, so
   the path tracer's any-hit drops them too. Under a burnt crown the char
   decal lightens patchily to white-grey ash (mineral ash, albedo about
   0.35). The burning twigs are firebrands rather than rigid bodies: 1.5
   per kg of crown fuel, 1 to 3 cm thick, dropping from all through the
   crown from rest at 6 to 12 m/s (too heavy for the column), glowing for
   a minute or two where they land and able to light what they land on.
   Debris bodies would cost a PhysX actor each for a few glowing streaks.

### Wading wets clothing (taken on from world-links, 18 Nov)

ClothWetness (PhysXSkinnedCloth3D and PhysXCloth3D) asks the world water
contract (WorldContracts::water()->height_at, every water producer: sea,
lakes, rivers, pools, live water, standing water) for the surface under
the fabric. Fabric below it soaks in a few seconds (a woven fabric's
wicking takes up its fill within seconds once immersed, Kissa 1996), and
the water climbs about 3 cm above the line by capillarity. The soaked
band is held as a depth above the fabric's lowest point and dries as the
rest does (Penman). Its weight, stiffness and cling count as the band's
share of the fabric's height; the skinned cloth's shader darkens the
band up to a wavering tide line in world height. Test: a 60 cm skirt
waded 20 cm deep soaked to 23 cm in about 5 s (0.38 of its height, the
weight figure 0.38), with the tide line drawn in raster and path tracing.
Left: water drag on the submerged fabric (flow_at) and buoyancy.
Sources: Cohen 2000, "Preventing disaster: home ignitability in the
wildland-urban interface", J. Forestry 98; Manzello et al. 2008, Fire
Safety J. 43; McGrattan et al., FDS technical reference (water spray).

### Next-round options (card, 18 Nov night)

1. Water on fire makes steam. Water on burning fuel boils off (2.26
   MJ/kg) as a white plume that hides the flames for a moment, the
   look of every hose or bucket on a fire. Here: cells and burnables put
   out by water emit white, buoyant, fast-cooling vapour into Flow for a
   few seconds, more for hotter fuel.
2. Burning buildings and props break apart. Char eats a timber's section
   (charring rate about 0.65 mm/min, EN 1995-1-2), so beams give way and
   roofs fall in. Here: a PhysXBurnable3D on a Blast asset loses bond
   strength with its char depth, so a burning shed or fence breaks and
   falls in pieces that keep burning.
3. River current drags and floats clothing. Submerged fabric feels the
   current (Morison drag on the water's flow_at) and lifts (buoyancy
   less its weight), so a skirt or coat streams downstream and floats up
   when wading. Here: the waded band of PhysXSkinnedCloth3D and
   PhysXCloth3D gets drag and lift from the water contract.
4. Fire whirls. A fire in a sheared wind spins up vertical vortices
   (Forthofer and Goodrick 2011) that lift brands high and throw them
   far. Here: Flow's vorticity seeded where the front meets a wind
   shear or a lee slope; brands caught in it rise further.

### Water on fire makes steam (picked 18 Nov night, first of four)

Built. Water landing on burning or still-glowing fuel takes the hot bed's
heat once, as FDS's spray sink does: 2.6 MJ per kg to heat it to 100 C and
boil it. A grass cell gives its fuel and char at about 600 C (1.5 kJ/kg K)
and a second of its flames' output, never more than the water on it can
boil; a burning prop gives its char layer (about 1 cm at 500 C, 1.2 MJ/m2)
and a second of its heat release. That becomes puffs (a place, kg/s,
falling over 3 to 4 s) in PhysXSteam, which makes a PhysXFlow3D of its own
only while something steams and frees it 12 s after: steam is white where
the fire's smoke is grey, so it cannot share the fire's gas. Combustion off,
no glow (vapour near 100 C), a buoyant gas (smoke_buoyancy, as water vapour
is lighter than air), a thin box source on the wet ground pushing up and
outwards (boiling water swells some 1,700 times), and the cloud fading by
the air's humidity (smoke_fade 1.2/s in dry air to 0.25/s near
saturation). Fire at a shore (WorldContracts' water): a cell the front
reaches under the surface goes out in a puff, and flames beside the water
send a tenth of their heat into it as a thin steady steam. Test: a 2.6 m
square of water on a burning patch put out 100 cells and made 2.7 kg of
steam that rose as a white billow over a metre in two seconds.

### Burning buildings break apart (picked 18 Nov night, second of four)

Built. A PhysXBurnable3D whose target is a PhysXDestructible3D weakens its
Blast bonds as it chars. The section lost follows EN 1995-1-2's reduced
cross-section: a notional char depth of 0.8 mm a minute (softwood members,
corner rounding included) plus a zero-strength layer growing to 7 mm over
the first 20 minutes, from both faces of the box's thinnest side. Bonds in
the burning patch take damage in step with it, reaching their health at
collapse_share (0.4: a member loaded near half its capacity fails when
about 40% of its section is gone), with no shatter speed, so pieces break
off gently and the support graph (anchor Grounded) drops what is cut
loose. The destructible gained a material remap and instance shader
parameters applied to every piece, live and future, so the burnable's
char look (in the destructible's own space) stays on falling pieces. Test:
a 3 x 2 m wall of 4 cm boards (24 Voronoi chunks) burning at up to 1.8 MW
broke at 7 minutes and was all pieces by 8, as the section model says.
Left: flames stay at the intact shape's place after it breaks (the Flow
emitters do not follow pieces yet).

### River current drags clothing (picked 18 Nov night, third of four)

Built for worn clothing (PhysXSkinnedCloth3D). ClothWetness keeps the
water's surface under the garment and its current at the wet band's mid
depth (WorldContracts' water: height_at, flow_at). The cloth solver takes
both: particles under the surface keep 30% of their weight (woven fibre is
denser than water: cotton 1.54, polyester 1.38, wool 1.31 g/cm3, so soaked
fabric sinks slowly) and are dragged toward the current at Morison's
0.5 rho Cd |u| over the fabric's soaked areal mass (Cd about 1.2 for a
sheet across the flow, |u| the current plus 0.1 m/s for its own sway),
capped at 200/s. Test: a 60 cm skirt standing in water to 35 cm below the
waist, then in a 0.8 m/s current: the upstream panel pressed 16 cm in
against the legs while the downstream side held out, and it all hung back
once out of the water. Left: free PhysXCloth3D (flags and sheets dipping in
a river) does not take the current yet.

### Fire whirls (picked 18 Nov night, fourth of four)

Built. Research: fire whirls form where a fire's updraft meets ambient
vorticity, most often on the flanks of a front in a cross wind or at a
lee slope (Forthofer and Goodrick 2011, "Review of vortices in wildland
fire"). Kuwana et al. (2008) found whirls behind line fires when the wind
U is near the buoyant velocity scale w* = (g I / (rho cp T))^(1/3), and
L-shaped fronts favour them (Kuwana et al. 2013). Whirl cores are a
fraction of the flame length across, spin at a few times w* and stand
several flame lengths tall. Here: tiles of strong flame (I at least 300
kW/m) whose front normal runs across the wind (|n . w| under 0.35) are
flank sites. Each has a birth chance of 1 in 180 a second times a
log-normal favour exp(-(ln(U/w*)/0.5)^2), at most two whirls, 6 m apart.
A whirl has a core of 0.3 flame lengths (0.3 to 3 m), spins at 2 w* either
way round, rises at 2 w*, lives 10 to 30 s, drifts at a third of the wind
and ages three times as fast off flaming fuel. It draws as four
overlapping Flow sources in a helix up its core (tangential plus upward
velocity), so the flame is pulled into one spinning column, and adds a
firebrand plume (1.5 times its rise) so brands caught in it go higher.
Test: a 24 m grass fire, 90 s at each wind: none at 1 m/s, 7 at 2.5, 4 at
4, none at 6 and 9 m/s; the frame shows a flame column about twice the
height of the front's flames with smoke shed off its top. Left: the
column's vorticity is set by its sources, not grown by Flow from a sheared
wind field, and terrain lee slopes are not yet a birth site.

### Next-round options (card, 18 Nov late)

1. Burning pieces keep their flames. After a burning wall breaks, the
   Flow sources stay where the intact shape stood. Here: the burnable
   follows the destructible's live pieces (their AABBs and char state) and
   moves its emitters with them, so a falling plank burns as it falls
   and smoulders where it lands (as Teardown and BeamNG keep fire on
   debris).
2. River current drags flags, sheets and nets. Free PhysXCloth3D does not
   yet read the water contract. Here: the same Morison drag and reduced
   weight as worn clothing, per particle under WorldContracts' water, so a
   sheet dipped in a river streams downstream.
3. Wind lifts ash after a fire. Burnt grass leaves a light ash layer (a
   few hundred g/m2) that blows off in winds above about 4 m/s (Wagenbrenner
   et al. 2013, post-fire ash emission). Here: the char map seeds a heavy
   dust (negative smoke_buoyancy) in Flow where wind is strong over fresh
   ash, thinning the ash each gust, so a burnt hillside smokes grey in
   the wind for hours.
4. Fire whirls on lee slopes. Whirls also form in the lee of ridges where
   the wind separates (Forthofer and Goodrick 2011; Sharples et al. 2012
   on vorticity-driven lateral spread). Here: births also where the
   terrain wind (WeatherMap3D, terrain wind) turns back behind a ridge,
   and those whirls run along the slope, spreading the fire sideways.

### Burning pieces keep their flames (picked 18 Nov late, first of four)

Built. Once a burning PhysXDestructible3D has broken, PhysXBurnable3D finds
the burning patch again among the pieces by where each sat in the intact
object (PhysXDestructible3D.get_piece_rest_center: the middle of its
chunks in the node's space) and their size (get_piece_volume): a piece
whose rest centre lies within the burn radius of the ignition, give or
take half its size, is alight. The eight biggest carry the flames, a
Flow source riding just over each (Flow takes moving bodies as solids, so
the gas would be lost inside one), the heat release shared by volume and
the flame height by Heskestad as on the faces. Sparks and firebrands rise
from the same places, so they follow the debris too. Once the fuel is
spent the pieces smoulder: a faint warm smoke over each until the embers
die, with the char glow already carried on the pieces by the instance
shader parameters. This is how Teardown and BeamNG keep fire on debris:
fire is a property of the burning material and moves with it. Test: the
3 x 2 m board wall broke at 70 s (fast collapse_share); flames went down
with the pieces and burnt on the ground pile (heights 0.2 to 0.3 m) at
125 s. get_piece_center now gives the middle of all a piece's chunks with
the node's scale (it gave the first chunk's), which also puts rockfall
dust under the middle of a falling island.

### River current drags flags and sheets (picked 18 Nov late, second of four)

Built for the CPU cloth solver (PhysXCloth3D's default). The XPBD solver
takes the water's surface and current (ClothWetness reads them from
WorldContracts' water each step): particles under the surface keep 30% of
their weight and relax toward the current at Morison's 0.5 rho Cd |u| over
the soaked areal mass (Cd 1.2), capped at 200/s, as worn clothing does.
Test: a 1 x 1.5 m sheet hanging from its top edge with its lower 45 cm in
the water: still water left it hanging; a 0.8 m/s current streamed its
lower edge 0.94 m downstream, up to the surface, in two seconds, and it
hung straight again once out. Left: the GPU deformable path (PhysX
surface) does not take the current.

### Wind lifts ash after a fire (picked 18 Nov late, third of four)

Built. Research: fresh burn scars are among the strongest wind-erosion
dust sources (Wagenbrenner et al. 2013, 2017, Idaho rangeland fires:
emission from winds near 5 m/s until rain crusts the ash or plants come
back); ash loads run 0.1 to 5 kg/m2, the low end in grass (Bodi et al.
2014). Here: each burnt cell holds the ash its burn made (6% of the fine
fuel burnt, plus 0.15 kg/m2 under a fully burnt crown), less what has
blown. Once cold (three burn times, as steam's "hot") and dry (intercepted
water under 0.02 kg/kg: rain or fluid water holds it), it gives a dust
flux from the friction velocity u* = 0.075 U (log law to 2 m over 1 cm
roughness): White's horizontal flux Q = 2.6 rho/g u*^3 (1 - (u*t/u*)^2)
with u*t = 0.25 m/s (some 3.5 m/s near the ground), and the lifted dust
F = 2e-3 Q. Gusts are a noise field carried with the wind (Taylor's frozen
turbulence, +/- 35% of U), so as u*^3 the ash comes off in travelling
puffs. About-2 m patches feed PhysXAsh, which makes its own PhysXFlow3D
only while ash blows (no combustion, grey, smoke_buoyancy -0.4 so it hugs
the ground, slow fade) with eight low box sources pushing into the wind's
direction; the Flow's own weather wind carries it. The crown ash in the
char map thins as it blows off. Test: a 16 m tall-grass burn (about 10 kg
of ash): 0.04 kg blown in 5 minutes at 3 m/s, 1.5 kg at 5, 8.2 at 8 and
all of it within 3 minutes at 12 m/s; under a 10 m/s WeatherMap3D wind a
low grey sheet streamed downwind off the scar. Left: grass ash does not
show in the char map itself (only crown ash is drawn lighter).

### Fire whirls behind ridges (picked 18 Nov late, fourth of four)

Built. Research: wind over a ridge separates and rolls into a horizontal
eddy in the lee; a fire on the lee slope tilts that vorticity upright into
whirls that carry the fire across the slope, along the ridge, at rates
near the head fire's (vorticity-driven lateral spread: Sharples et al.
2012, Simpson et al. 2013 in coupled LES; the 2003 Canberra fires). The
operational test (Sharples et al. 2012): slope over about 20 degrees,
aspect within 40 degrees of the lee direction, wind over about 20 km/h.
Here: strong flaming tiles (at least 300 kW/m) where the fire's ground
(its height samples) falls away at more than tan 20 within 40 degrees of
downwind, in a wind over 5.5 m/s, are lee sites. Each has a 1 in 600
chance a second of a whirl, with no U/w* condition (the eddy is the
ridge's). A lee whirl lives 20 to 60 s, drifts along the contour at a
fifth of the wind (either way, plus a tenth of the wind downslope),
lights the fuel under its core as it goes, and otherwise draws and lofts
brands as a flank whirl. All whirls now sit on the ground's height. Test:
a 30 x 40 m tall-grass patch on a 28 degree slope falling away downwind:
at 8 m/s five lee whirls in two minutes widened the burn across the slope
to 22.5 m at one minute against 18.5 m without whirls; none with the
wind up the slope. (That test found the fire's length-to-width ratio
uncapped: Anderson's fit gives about 90 at 8 m/s, stalling the flanks;
it is now capped at 8 as FARSITE does.)
Left: the lee test reads the fire's own slope, not the terrain wind's
separation behind a ridge crest upwind; whirls (and so the lateral
spread) need the fire's Flow, as flank whirls do.

### Next-round options (card, 18 Nov late night)

1. Fire makes its own wind. A fire's plume draws air in at its base
   (indraft of a few m/s at a strong front: Clements et al. 2007, FireFlux),
   which tilts flames toward the fire, slows the backing front and pulls
   smoke, ash, grass and loose cloth inward. Here: the fire's Flow
   velocity near the ground, sampled at the front, feeds the spread's
   effective wind (WRF-SFIRE's two-way coupling, with Flow as the
   atmosphere), and is offered on the wind contract so grass, cloth and
   particles near a fire lean into it.
2. Burnt trees fall later. A ground fire chars a trunk's base (cambium
   killed, wood charred from the outside: Dickinson and Johnson 2001);
   days on, or in the next strong wind, fire-weakened trees fall (hazard
   trees, a main danger to crews). Here: the char depth at the stem foot
   from flame length and residence time, the section left (as buildings'
   EN 1995-1-2 model) against the wind's bending load, and a tree whose
   base gives way falls as a PhysX body (the storm bench's thrown-tree
   path).
3. Burnt grass shows its ash. Complete combustion leaves white-grey
   mineral ash, incomplete black char (Roy et al. 2010: ash albedo 0.3 to
   0.5, char 0.03 to 0.05); the mix follows fuel moisture and intensity.
   Here: the char map draws grass ash by combustion completeness, thinned
   by blowing ash and darkened by rain, so a burn scar reads grey and
   black in patches instead of uniform black.
4. Lee eddies from the terrain wind. Lee whirls now read the fire's own
   slope. Here: WeatherMap3D's terrain wind marks where flow separates
   behind a crest (reversed or slack near-ground wind in the lee), so lee
   whirls form only below a real ridge, and the lateral spread follows the
   ridge line.

### Fire makes its own wind (picked 18 Nov late night, first of four)

Built. Research: a fire's plume entrains air at its base; FireFlux
(Clements et al. 2007) measured an indraft of a few m/s at a grass head
fire, opposing the ambient wind ahead of it, and coupled models
(WRF-SFIRE, Coen's CAWFE, Clark et al. 1996) show it drawing fronts
together where they meet (junction fires: Viegas et al. 2012, Raposo et
al. 2018). Here: each 2 m stretch of burning front over 50 kW/m is a sink
drawing in, per metre and side, q = (pi/2) w* d0 (w* Byram's buoyant
velocity, d0 half the flame length), so the inflow just outside the
flames is about w*/2 and falls as 1/d along a front (a line sink) and
1/d^2 past its ends; the strongest 64 are kept each 0.25 s. It goes out
three ways: WorldFire.wind_at (a new contract member, summed over fires),
which PhysXCloth3D adds to its wind; shader globals fire_wind_map and
fire_wind_rect (2 m texels over the fire), which grass adds to its wind
(VegetationFire::WIND_CODE); and the spread, which takes only the pull of
fire more than 5 m away lying ahead of a front (a fire's pull on its own
front is in Rothermel's empirical fits already). Test: two 20 m fire lines
8 m apart in calm tall grass closed the gap in 65 s against 136 s without
it; a lone fire in a 3 m/s wind burnt exactly the same area either way;
the indraft 2 m outside a calm front was 0.3 to 0.6 m/s. Left: the Flow
gas itself does not take this wind (it has its own buoyant flow), and the
grass map covers the fire's own area only.

### Burnt trees fall later (picked 18 Nov late night, second of four)

Built. Research: fire kills and chars a trunk's base from outside, deepest
on the lee side where flames wrap round it (catfaces: Gill 1974,
Gutsell and Johnson 1996); burnt-through and hollowed bases are how
fire-killed trees fail, often months later in wind (hazard trees:
Dickinson and Johnson 2001, Hood et al. 2008). Charring in wood moves
inward at about 0.65 to 0.8 mm a minute of fire (EN 1995-1-2, the
notional rate), and the section left carries the bending load (the
reduced cross-section method). Here: PhysXFireSpread3D keeps, per cell,
the time heat stood at it (grass and litter by their heat over 50 kW/m2,
a flaming log fully, a smouldering one a third), so a log burning
against a trunk for an hour chars it about 5 cm and a grass front alone
barely marks bark. WorldFire.stem_char_at (a new contract member, the
most of any fire) gives it to TreeWind3D, which reads it for each tree
once a second while a fire provider exists. In the storm test the char
past the bark (a tenth of the radius, 5 mm to 3 cm), taken as half its
depth round the stem, leaves (1 - c/r)^3 of the foot's strength (times a
per-tree defect, 65 to 115%); if the wind's base moment beats that
before it beats the roots, the tree snaps at its foot: it falls as the
thrown-tree body without its root plate (no plate mass or hull, no pit),
hinged on its lee side at the foot, and its scorched low limbs (side
capsules below a third of its height) no longer prop it (crown scorch
height, Van Wagner 1973). Test: on the storm bench a spruce with 12 cm
of char (`--char=0.12`, a WorldFire stub) snaps at about 2.8 s in a
26 m/s storm and lies down on its upper whorls; without char nothing is
thrown at 26 m/s. Left: no stump is drawn (the whole tree falls), and
char does not yet weaken limbs or the standing tree's sway.

### Burnt grass shows its ash (picked 18 Nov late night, third of four)

Built. Research: how completely fine fuel burns (combustion completeness)
falls with its moisture: about 0.95 for cured late-season grass against
0.7 early in the dry season (Shea et al. 1996; Hoffa et al. 1999 in
savanna). A complete burn leaves white-grey mineral ash (albedo 0.3 to
0.5), an incomplete one black char (0.03 to 0.05), and the share of white
ash on a burn scar tracks the fuel consumed (Smith and Hudak 2005; Roy
et al. 2010). Wet ash darkens as wet soil does (water filling its pores:
Lobell and Asner 2002) and crusts (Woods and Balfour 2008). Here:
PhysXFireSpread3D keeps the fuel moisture each cell had as it caught;
completeness is 1.02 - 1.1 M (0.45 to 1); the char map lays white-grey ash
over the char from 0.72 completeness up, in patches at tussock scale
(noise at 0.35 and 1.4 m), deeper where the bed was heavier, thinned by
the ash the wind has blown off (blowing_ash) and dimmed by the rain the
cell holds. Crown ash is kept and takes the larger of the two. Grass
stubble reads the same map (VegetationFire::ASH_CODE, global
fire_char_map) for its share of white ash flecks (2 to 50%). Setting
grass_ash, on by default. Test: a 9 m tall-grass burn at 4% moisture
reads grey with black patches; at 13% mostly black with grey patches; at
22% the grass does not carry fire.

### Ridge whirls follow the terrain wind (picked 18 Nov late night, fourth of four)

Built. Research: vorticity-driven lateral spread needs flow separated in
a ridge's lee (Sharples et al. 2012; Simpson et al. 2013 with WRF-Fire);
separation behind a ridge sets in for lee slopes over about 0.3 (Wood
1995), and Winstral's Sx (steepest upwind rise within 100 m, Winstral et
al. 2002) is the terrain wind's measure of that shelter. The 5.5 m/s
threshold is the ambient (ridge-top) wind, not the slack air in the lee.
Here: WorldWeather gains terrain_lee_at (Sx in radians, NaN where no
terrain wind covers the point), which WeatherMap3D provides. A burning
lee tile is a site where Sx is over 0.3 rad on ground falling away within
60 degrees of downwind at over 10 degrees; with no terrain wind the old
local test (20 degrees, 40 degrees) stays. The wind for the threshold and
the whirls' cross-slope drift is the larger of the fire's wind and the
free wind at 10 m (WorldWeather.wind_aloft_at). Test: a 20 degree
windward face to a crest and a 28 degree lee slope under an 8 m/s
WeatherMap3D: the lee's own wind is 1.5 to 3 m/s (the old test saw no
whirls there); the new one spawned 6 lee whirls in 150 s and the burn
covered 659 m2 against 581 m2 without whirls.

### Next-round options (card, 18-19 Nov night)

1. Charred stumps and standing snags. A tree that snaps at a charred foot
   should leave its stump (now the whole tree falls), and fire-killed
   trees stand as snags that shed bark and limbs and fall over the
   following years, faster for small and rotten stems (snag fall rates:
   Russell et al. 2006; Dunn and Bailey 2016). Here: a stump mesh from
   the stem's foot with the char on it, kept static; killed trees (crown
   burnt out) lose fine branches and bark in later winds and fall by a
   per-tree hazard, using the storm path.
2. Rain washes the burn scar. Burnt soil turns water-repellent (Doerr et
   al. 2000), so rain on a fresh scar runs off at once, carrying ash and
   soil (ash slurry, "black water" in streams: Smith et al. 2011) and
   cutting rills. Here: the fire's burnt map lowers infiltration on the
   ground water contract, ash moves with the runoff into the mud and
   river systems (darker, cloudier water downstream), rills on steep
   scars through the terrain deformation layer.
3. Smouldering ground fire. Peat and deep duff burn slowly without flame
   (a few cm an hour, Rein 2009), with white smoke, under a front that
   has passed or after it is out, and can rekindle a flaming fire hours
   or days later when it reaches dry fuel (holdover fires). Here: a duff
   layer per cell from the fuel map, a slow 2D smoulder front (Rein's
   rates by moisture and depth), Flow smoke from it, rekindling the
   surface fire where it meets unburnt dry fuel.
4. Fire clouds. A strong fire's plume condenses into a pyrocumulus over it
   (Lareau and Clements 2016) whose downdrafts gust out at the ground and
   turn the fire. Here: the plume's heat flux and the weather's moisture
   give a cloud base and growth (Briggs plume rise with the lifting
   condensation level), drawn by the weather's cloud volume, and its
   outflow added to the fire's wind.

### Charred stumps (picked 19 Nov, first of four, part one)

Built. Fire-killed and fire-weakened trees break in the charred band at
the foot (the catface), leaving stumps of about 0.3 to 1.5 m (Gill 1974;
hazard-tree guides). Here: a snap now cuts the stem 0.3 to 0.9 m up (steady
per tree). TreeWind3D's piece meshes clip the stem's triangles exactly at
the cut (one-plane Sutherland-Hodgman, attributes interpolated): the
falling part keeps everything above, the stump the bark below. The stump
is a static body (a cylinder collider, on the trees' collision layer and
layer 1) with a fracture cap a third as tall as a green break (charred
wood breaks short and brash); the trunk's butt takes the matching face
and hinges on the stump's lee edge. The capsules, the Blast hulls and the
Blast soups of the stem below the cut are dropped from the part.

### Standing snags (picked 19 Nov, first of four, part two)

Built. Research: whether a fire kills a tree follows the share of its
crown scorched and its bark thickness (Ryan and Reinhardt 1988, FOFEM:
P = 1 / (1 + exp(-1.941 + 6.316 (1 - e^-BT) - 0.000535 CVS^2)), BT in
inches, CVS in %). Dead trees stand as snags: fine limbs drop in the first
years, roots and butt hold about a year and then rot, and snags fall at
the base with half-lives of about 5 to 10 years (Everett et al. 1999;
Russell et al. 2006; Dunn and Bailey 2016). Here: WorldFire gains
crown_scorch_at (Van Wagner's lethal scorch height of the front that
passed, PhysXFireSpread3D's peak_scorch, which a torching crown raises
over its top) and PhysXFireSpread3D now provides burn_age_at (years of
the weather's clock since each cell caught: the fire keeps a game-day
clock from WorldWeather.time_scale). TreeWind3D, once a second while a
fire provider exists, takes each tree's crown share below the scorch
height (foliage area of its bones) and bark of 4% of its diameter, and a
hash steady per tree against P; a killed tree's age dead follows
burn_age_at. In the storm test its limbs keep 0.2 + 0.8 e^(-t/3) of their
strength and its roots and butt 0.15 + 0.85 e^(-(t-1)/6), and a snag dead
over half a year fails at the butt (the snap path: stump, no plate). Test
(storm bench, `--snag=Y` stub, 18 m/s): alive, nothing breaks; dead 0
years, nothing; 3 years, 11 limbs; 8 years, 38 limbs and the spruce falls
at its butt. Left: the dead crowns' needles stay on in the shader (trees
thread: needle cast in the first year).

### Rain washes the burn scar (picked 19 Nov, second of four)

Built. Research: a hot fire leaves a water-repellent layer a few
centimetres down (DeBano 2000; Doerr et al. 2000), strongest where it
burnt long and hot (Huffman et al. 2001), weakening over months and
mostly gone in one to two years. With the litter gone and the soil
repellent, infiltration falls to about a tenth (Robichaud 2000), so light
rain that unburnt ground would soak up runs off, and the runoff carries
the ash (Cerda and Doerr 2008; Smith et al. 2011: ash and fines at tens of
kg per m3 of runoff). Here: WorldFire gains water_repellency_at (0 to 1,
max over providers). PhysXFireSpread3D provides it on burnt-out, unquenched
cells: 0.35 + 0.65 of the char time over a minute (0.5 under a heavy log),
fading as e^(-age / 0.75 yr) on the burn-age clock. GroundSurface3D reads
it with burnt_at every cell refresh: burnt litter is gone and its
infiltration (pore water and the runoff channels) is cut by 1 - 0.9 R.
Once a second the fire reads WorldGround.runoff_at on cold burnt 1 m tiles
and takes 30 kg/m3 of the unit discharge off each cell's ash (the ash map
dims, get_ash_washed counts it). Test (ground bench `--test=burnscar`,
3 mm/h on a 15 degree clay bank): repellent scar 0.036 mm sheet, the same
bank with the scar gone 0 mm; fire scratch run: a 2 mm sheet at 0.3 m/s
washes 1.3 kg of ash off a 10 m burn in 20 s. Left (water owners): the
washed ash should darken the stream it reaches (a turbidity or load term
on WorldWater).

### Smouldering ground fire (picked 19 Nov, third of four)

Built. Research: under the fine fuel lies duff and organic soil that a
passing front can light and that then smoulders for hours with no flame
(Rein 2009). It lights with a 50% chance near 110-120% moisture (dry
basis) in organic soil with little mineral matter, drier as the mineral
share rises (Frandsen 1987, 1997; Prat-Guitart et al. 2016 find 115-150%
by bulk density); it creeps sideways at 1-5 cm/h, slower when wetter
(Prat-Guitart et al. 2016: medians 1-5, spots 0.6-9 cm/h), and burns down
more slowly still; its smoke is pale (condensed tars and water, little
soot) and cool, so it hangs low; heavy rain puts it out, light rain does
not; and under unburnt fuel a breeze can turn it back into flame, the
holdover fire (Santoso et al. 2019). Ignition indices from duff codes
(Otway et al. 2006: 50% at DMC 27-29 in aspen duff) back the same
picture. Here: PhysXSmoulder (flow/physx_smoulder.*) keeps a duff grid
under PhysXFireSpread3D, on the weather's clock (time_scale): duff_depth
(by fuel type: 1, 1.5 and 5 cm) at 100 kg/m3, deeper under heavier fuel;
duff_moisture follows the weather over 12 game days (the DMC's time lag)
and soaks up half the rain; duff_mineral moves the 50% point
(1.2 - 1.5 (mineral - 0.1)). Half a burn time after the front, each cell
gets one steady-hash chance to light (logistic in moisture, width 0.12,
thinner beds less); a smouldering cell creeps into its four neighbours at
5 e^(-0.8 M) cm/h (a little faster in wind), burns down at a quarter of
that (about 1.6 kW/m2 at 8 MJ/kg, part of heat_at), goes out past 1.6
times the 50% moisture (rain, or poured fluid water, which boils off as
steam), and under unburnt dry fuel in wind over 1 m/s relights the fine
fuel (hazard up to once a game hour at 4 m/s). It glows dull red in
spots on the ground map, leaves pale ash where the duff burnt through,
and smokes from a gas of its own (2 m tiles, eight emitters, light grey,
buoyancy 0.8, freed 20 s after the last cell). Test (scratch scene, 10 m
litter fire, time_scale 3600): the duff lights behind the front (peak
1,437 of 1,600 cells), burns 465 kg in about a day and goes out; 20 mm/h
rain puts it out within ten game hours (446 kg burnt), 1 mm/h does not;
a fire stopped at 3.6 m2 with a 3 m/s wind relit the fuel around it 14
times in 100 game hours.

### Fire clouds (picked 19 Nov, fourth of four)

Built. Research: a fire's plume rises until its buoyancy is spent; where
it passes the lifting condensation level its moist air condenses into a
cumulus cap (pyrocumulus), and a plume that keeps rising on the latent
heat becomes a pyrocumulonimbus, with lightning and downdraft outflow
(Fromm et al. 2010; Lareau and Clements 2016, who find the plume's
condensation level a few hundred metres above the ambient one: the
plume's heat lifts it, the fire's water vapour lowers it, near evenly;
Tory et al. 2018 on the heat it takes). Plume rise: Briggs (1975), with
F = g Q / (pi cp rho T) from the convective heat Q (about three quarters
of the heat release: radiant fractions 0.15-0.3), 5.0 F^1/4 N^-3/4 calm
and 2.6 (F / (u N^2))^1/3 bent over, N about 0.0106/s for the standard
lapse rate; by day the plume first crosses the sun-mixed layer, which
reaches to about the cumulus base (Stull 1988). LCL: Espy's 125 m per
kelvin of dew point depression. Here: WorldFire gains plumes(), a list of
(base, convective heat) from every fire; PhysXFireSpread3D gives one per
node, from its flaming tiles and burning crowns. WeatherMap3D (weather
owners' code, flagged to them) turns each plume that reaches the LCL into
a storm entry marked fire: no rain, cumulus (cover 0.6-0.77, which sets
the type) over the bent-over plume, radius half the rise, growing over
five game minutes and evaporating over fifteen once the heat stops; at
2 km past the LCL it grows into a pyrocumulonimbus (cover 1: a tower,
10 m/s outflow, 2 mm/h), which WeatherLightning3D already strikes from by
its cloud top (Price and Rind). `fire_clouds` on by default;
get_fire_cloud_for(Q) gives the rise, LCL and strength. Numbers (30 C,
25% humidity, LCL 2.8 km): by day 1 GW caps itself, about 40 GW makes a
pyrocumulonimbus; at night (mixed layer 200 m) nothing under about 60 GW;
humid air (60%, LCL 1.1 km) caps a 1 GW fire fully. Scratch test with a
plume stub (50 GW, sun unknown): a cumulus cap over the plume after 20
game minutes (cloud type 0.79), gone within two hours of the heat stopping. Each fire cloud's
base sits at its plume's own condensation level (fire base plus the LCL),
not the map's cloud_base_height: storms carry an optional base, read by the
sky shader, the CPU density (shadow bake, path traced sky) and lightning;
round a storm the base levels out to about four shield radii (weather
bench `--view=clouds --fire=5e10`: base 2500 m against the map's 1500 m).

The same round adds `fire_age_map` for the trees thread (needle cast on
fire-killed crowns): R8, game years since each cell caught over 2 (about
3-day steps), 0 where nothing burnt, over fire_char_rect.

### Smoke haze dims the sun (picked 19 Nov morning, first of three)

Built. Research as in the card notes below: mass extinction about 4.5
m2/g at 550 nm, single scattering albedo about 0.9, Angstrom exponent
about 1.8 (Reid et al. 2005); emission factors of PM2.5 about 7 g/kg for
grass, 17 g/kg for temperate forest (Andreae 2019), about 30 g/kg
smouldering (Urbanski 2014); afternoons several kelvin cooler under thick
smoke (Robock 1988). Here: WorldFire gains smoke_sources() (x, y, z,
kg/s of PM2.5 in the fire's own real time). PhysXFireSpread3D gives two:
its flames (plume heat / 0.75 / 18.6 MJ/kg times the factor by fuel
type) and its smouldering duff. WeatherMap3D (weather_smoke.cpp, weather
owners' code, flagged) keeps cell_smoke, g/m2 in the column over each
map cell: the sources laid along the step's wind run (no puffs on long
steps), semi-Lagrangian advection by the wind aloft (log law to 1 km),
eddy diffusion at 300 m2/s, an e-folding smoke_lifetime of 3 days, and
the column shifts with the map. Effects at the map's centre: the
aerosol haze's extinction gains tau / 2000 m; the daily temperature
range is scaled by e^(-0.35 tau); SkyClock multiplies the sun (and moon)
light by the smoke's RGB transmittance, read where its ray crosses the
layer at 2 km, along the slant path, tau x (0.83, 1, 1.35) for red,
green and blue. Test (scratch, 10 kg/s, about a 10 GW fire, 3 m/s wind,
three hours): 2.8 g/m2 (tau 12.6) 1 km downwind, 1.9 at 250 m and 0.5 at
500 m across it, tau 6 at 1.9 km; Q / (u W) gives the same; gone two days
after the source stops. A 100 m2 grass fire gives about 5 g/s, too little
to see, as it should be. Left: the cloud shadow bake could take the
smoke's grey part so plumes shade the ground in patches (weather
owners); the haze is one value over the map's centre, not a field; the
lofted plume is treated as mixed through 2 km.

The sky's sun (20 Nov). The sun light already took the smoke by colour, but the sky's disc and the light its air
scatters only saw the smoke as part of the grey aerosol haze, so under a thick plume the disc stayed white and the
sky grey. Now the sky gets the same per-colour transmittance as the light (`WeatherSkyMaterial.set_sun_smoke`, set by
`SkyClock`) and the smoke's share of the aerosol (`set_aerosol`'s smoke_share). The sunlight reaching each height
takes the smoke column above it by colour, which reddens the forward-scattering aureole (g 0.6) and browns the sky.
Multiple scattering uses the transport approximation's forward share, exp(-(1 - w (1 + g) / 2) tau), about 0.28 tau
with w 0.9 and g 0.6, so the sky under thick smoke stays a dim orange-brown instead of going black. The disc is
drawn at the sun's irradiance, about 1/15000 of its radiance, so the smoke's transmittance on it is scaled by
1 / solid angle and clipped at the clean disc. Like the real sun, it stays a white disc in an orange glow until a
slant optical depth of about 8, then turns a dim yellow-orange and red (Angstrom exponent 1.8, Eck et al. 1999;
Reid et al. 2005).

### Post-fire mudflows (picked 19 Nov morning, second of three)

Split agreed by the three owners. Valence (WeatherRunoff3D) fires the
event: USGS M1 likelihood (Staley et al. 2017) and Gartner (2014) volume
at basin outlets, at P >= 0.5, one per basin per storm, signal
debris_flow on WeatherRunoff3D and WorldGround, with id, volume, path,
widths and severity. Mud (GroundDebrisFlow3D) moves the body:
Bingham/Voellmy on the path, Iverson entrainment, deposit written into
the ground, front getter get_debris_front(id) and the
debris_front_changed signal. PhysX (PhysXDebrisSurge3D, nodes/) draws what
rides the front. Research: post-fire debris flows arrive as surges, tens
of seconds to minutes apart (Kean et al. 2011, 2013); a bouldery snout
forms because the surface outruns the front and carries clasts forward
(Iverson 1997); peak discharge Q = 0.1 V^(5/6) and front speed v = 2.1
Q^(1/3) S^(1/3) (Rickenmann 1999); fans and deposition below about 4
degrees (Ikeya 1981). Built: a train of up to four surges (one per 400
m3, each 0.6 of the last); a front of its own on the path at
Rickenmann's speed, easing over a few seconds and stopping below
stop_slope, replaced by mud's front where its getter reports one;
boulders as rigid bodies drawn from a D^-2.5 power law up to the flow
depth, with quadratic drag toward 1.3 times the front speed behind the
snout and the front speed at it, the slurry's buoyancy, the force
clamped for stability; mud spray as ballistic drops. Test (scratch:
1500 m3, 25% channel then a 3% fan): peak Q 44 m3/s, front 3.8 m/s, 1.0
to 1.8 m deep; boulders stay within a few metres of the snout; the front
stops 30 m onto the fan with its boulders in the lobe; the second surge
stops 4 m short of the first. Left: drawing the body and levees is
mud's; dust ahead of dry fronts; rumble and ground shake.

### Burning wooden buildings (picked 19 Nov morning, third of three)

Built as PhysXBuildingFire3D (flow/physx_building_fire_3d.cpp), a
compartment-fire zone model per room rather than a PhysXBurnable3D mode:
a burnable is one surface, a house is rooms that pass fire between them.
Research: t-squared growth, 1 MW at 300 s for dwellings (EN 1991-1-2
Annex E); Kawagoe's ventilation limit 1.5 A sqrt(H) MW; McCaffrey,
Quintiere and Harkleroad's hot layer dT = 6.85 (Q^2 / (A sqrt(H) h_k
A_T))^(1/3) with a timber lining's h_k = sqrt(k rho c / t); Thomas's
flashover Q_FO = 7.8 A_T + 378 A sqrt(H) kW; glass falls out at 450 to
530 C (Babrauskas 2004); fully developed burning at 250 kW/m2 of floor
with 780 MJ/m2 of fuel (Annex E means for dwellings), the excess over
the air limit burning outside; external flame length L = H (2.37 (Q /
(A rho_g sqrt(g H)))^(2/3) - 1) (Annex B); linear decay after 70% burnt;
notional charring 0.8 mm/min (EN 1995-1-2) scaled by the layer's
temperature; light frame (50 mm members) fails at 60% section lost;
ember ignition of furnishings about 1 in 5 (Manzello et al. 2006) and
ember piles at wall feet (Manzello 2017); structure fires throw about
40 brands a second per MW (Suzuki and Manzello 2017).

Per room: states cold, growing, flashed, decay, out; doors between rooms
found where an opening lies on another room's face (mirrored to both
sides); spread by radiation through doors (half a blackbody at the layer
temperature, ignition dose as the cladding's), burn-through of walls,
floors and roofs, a collapsed room lighting the one above (and opening
its floor), and window flames on the facade breaking the window above
after 60 s of contact (leapfrog). Outside: each face's cladding takes a
dose from the ground fire's flames at its foot, Flow's hot gas,
PhysXBurnable3D and building radiation, and brand piles; it lights at
the thick-solid dose, spreads at 3 cm/s, lets the fire in where it
reaches an opening or burns through. Brands in an open window light the
contents. The window flames' radiation lights the ground fuel below.
Drawn as Flow emitters (inner fire, smoke from the opening tops before
flashover, flames after, roof, cladding), one flickering OmniLight3D per
room at its fire (the flames are its visible source), and the
PhysXBurnable3D burn material on the target's meshes. WorldFire: heat_at
over room footprints, burnt_at, plumes (0.7 of the heat), smoke (0.03
kg/kg). PhysXBurnable3D, cloth burn and firebrands read it.

Test (scratch house: two ground-floor rooms joined by a door, one above,
an open outer door, three glazed windows; fire time): window breaks at
6 min, flashover 8.7 min, the next room flashes 7 min after it caught
through the door, collapse 15 min after flashover, the upper room
catches through the burnt floor and flashes at 10 min, whole house out
after about 95 min. Peak 14 MW. Flux 5 m in front of a window about
4-6 kW/m2 (one 1.4 m2 window; a whole burning facade gives more).
Left: rooms are boxes (pitched roofs, attics and L-shaped rooms are
several boxes); no smoke filling rooms ahead of the fire (two-zone
smoke layer), no firefighting water on rooms yet (Flow/MPM water reaches
burnables, not buildings), glass is not drawn breaking.

### Water puts building fires out (picked 19 Nov, first of four)

Research: water boils off at 2.6 MJ/kg (heating and evaporation);
flames go down once the water takes about a third of the heat release
(Grimwood's gas cooling); Sardqvist's critical application rate for
compartment fires is about 0.1 to 0.3 kg/s per MW; steam swells about
1700 times and pushes the hot gas out of the openings. Built: water
in a room (PhysXParticleFluid3D water particles inside its box, or
apply_water() from a hose, a bucket brigade or a script) boils off over
about 8 s and takes 2.6 MJ/kg. The share it takes of 0.3 times what the
fire could release damps the burning at once (0.7 of it), and a room
held under it for 20 s goes out. A flashed room falls back to growing
below half, its hot layer cooling over 15 s. The fire regrows from what
is left when the water stops. Water on a face (particles within 0.5 m,
or apply_water near it) puts out burning cladding at 0.5 mm and keeps it
from catching, drying over ten minutes. The heat taken leaves as
PhysXSteam at the room's first open opening. Test (the scratch house,
room A flashed at 4 MW, water from 900 to 1500 s): 0.3 kg/s (0.075
kg/s/MW) holds it at 2.2 MW and 560 C, and it re-flashes when the water
stops; 0.6 kg/s puts it out in under two minutes; 1.5 kg/s also does.

### Smoke ahead of the fire (picked 19 Nov, second of four)

Two-zone layer per room (Quintiere; the zone model of CFAST). Plume
entrainment m = 0.071 Q_c^(1/3) z^(5/3) + 0.0018 Q_c kg/s (Heskestad),
Q_c = 0.7 Q, z the clear height; buoyant flow out of each opening below
the layer, m = (2/3) Cd W rho_s sqrt(2 g (1 - rho_s/rho_a)) dh^(3/2) with
Cd 0.7; through doors into the next room's layer (mass, soot and heat
mixed), out of windows, open roofs and 0.01 m2 of ceiling leaks; soot at
smoke_yield, visibility 3 / K with K = 8.7 m2/g times its concentration
(Mulholland); a layer with no fire under it cools over five minutes.
The opening emitters smoke from smoke_out and the layer's extinction
(also from rooms the fire has not reached). Test (scratch house, fire in
A): A's layer at 1.5 m after a minute (50 kW) and 0.5 m at 1 MW (5 min),
visibility 3 m at one minute and under 1 m by three; the next room B
fills through the door just behind A and shows the same visibility 4
minutes before it catches. Left: no smoke inside rooms drawn through
windows yet (Flow gas leaks through thin walls); stairs are not yet a
connection between floors.

### Glass and roofs that break (picked 19 Nov, third of four)

Research: window glass in fire cracks from the frame edge (the shaded
edge stays cool while the exposed centre expands; Pagni and Joshi 1991)
at about a 60-80 C difference, and falls out at about 450 to 600 C of
hot layer, in pieces of a few to some tens of centimetres (Babrauskas
2004; Shields et al.). Light timber floors and roofs sag as the char
eats their joists, deflecting fast just before they fail. Built:
glass_panes (on) draws a transmissive pane (IOR 1.52, thin-walled, a
soda-lime green edge) in each glazed opening; when the layer breaks it
the pane splits into about one piece per 0.06 m2 by random cuts, each cut
again along a diagonal into angular shards, 15% kept in the frame a
while, the rest rigid bodies pushed out at 0.5 to 2 m/s near flashover
(at most 300). Without a Blast target, the target's meshes over a room
(the top 0.3 m to 1.5 m above it: ceiling, the floor above, roof) sag
by span/20 times the cube of the share of section lost, and fall in as
rigid bodies (box colliders from their bounds, 400 kg/m3) when the room
collapses, the target's colliders over the room disabled. Left: shards
from the 15% left in frames; a pitched roof drawn as one mesh falls as
one piece.

Doorway smoke: the building's smoke goes to its own gas (BuildingSmoke:
no combustion, dark grey, slow fade), from a box across the smoky part
of each opening rather than a small sphere (which gave the grid a cell
or two of smoke that thinned to nothing within a metre).

Mudflow pooling (Valence: a 4 km2 burnt valley fires 68 events at once):
events under min_volume (100 m3) are left to the mud body; at most
max_events (6) events have surges, a bigger one replacing the smallest;
the boulder cap is shared. Storm test: 68 events, 39,000 m3: 6 events,
24 surges, 60 boulders.

### Village wind-fire bench (picked 19 Nov, fourth of four)

Research: in wildland-urban interface fires most houses are lit by
embers, directly or through fuels next to them (mulch, fences, yards),
and once a few burn, house to house spread by radiation, flame contact
and the houses' own firebrands takes over in dense parcels (Maranghides
and Mell 2011, Witch and Guejito fires; NIST TN 2252, Camp Fire, 2021).
Separation between structures is one of the main hazards in NIST's
parcel methodology (TN 2205); Cohen's home ignition zone puts what
decides a house's fate within about 30 m of it. Urban fire spread
models (Hamada; Himoto and Tanaka 2008) carry fire between buildings by
window flames, radiation and brands, which PhysXBuildingFire3D already
does.

Built: docs/bench/village_fire, a grass fire upwind of one or two rows
of two-room timber houses down a street, the wind blowing down it.
Options for houses per row, gap, street width, wind, the wildland's
fuel and the bare buffer before the first houses, lawns, a 1.5 m bed of
dry fuel against each house (the immediate zone), brands on or off,
speed and stills. PhysXBuildingFire3D now records its first catch:
get_ignition_time, get_ignition_cause ("direct", "firebrand", "heat")
and the ignited signal ("heat" includes beds that embers lit first).
Headless, 40 fire minutes take about 1.5 min.

First runs (2 rows of 6, tall grass 10 m upwind, 8 m/s, one run each,
so the counts are noisy): 6 of 12 lost (5 by heat from ember-lit beds,
1 by brands through a door) over 27 minutes; beds cleared, none lost;
houses throwing no brands, 4 lost (the first, from the wildland's
embers); a 15 m gap did not save the street (brands carried it, as the
surveys find); 4 m/s, 6 lost. A bare wall with no fuel against it does
not catch from a grass front 10 m off or from embers alone.

### Ash darkens snow (world-links hook, 19 Nov)

Research: light-absorbing particles on snow are counted as black carbon
equivalent (Warren and Wiscombe 1980; Flanner et al. 2007); burnt-area
ash and char blown onto snow cut its albedo for seasons after a fire
(Gleason et al. 2013, Oregon Cascades). Fine ash lifted to about 1.5 m
and falling at about 0.3 m/s settles over L = u h / v_s downwind. Built:
PhysXAsh hands the ash it lifts to WorldSnow.add_impurity once a second,
in three bands along the wind (e^-0 - e^-1, e^-1 - e^-2, e^-2 - e^-3 of
it), gathered into cells half L across, as 0.05 of its mass in black
carbon equivalent (char-bearing ash, darker than mineral dust's 0.013).
Test: 5.2 kg blown in 2 min at 10 m/s gives 247 g BCe on the snow out
to 150 m downwind, about 10 calls a second.

### Flow raster light matches path tracing (snow's request, 19 Nov)

The raster scattering pass weighed the multiple-scattering octaves
(Wrenninge et al. 2013; Hillaire 2016) as 1 + 0.5 + 0.25 of the light
at zero optical depth, so thin gas and the sunlit skin of thick gas got
1.75 times the light the path tracer gives them, and the lit and shaded
sides sat together in the tone curve's shoulder: evenly bright puffs.
The octaves are now normalised to 1 at zero depth (the extra octaves
still keep dense interiors from going black). Test puff, mean grey down
its middle (raster before, after, path traced): 185, 166, 152.
scattering_strength is a raster-only multiplier the path tracer does
not see; it stays at 1 for parity.

Mud's front wired (19 Nov afternoon): with front_source empty the surge
finds the scene's GroundDebrisFlow3D itself; only each event's lead
surge rides the mud body's front, the later surges run behind it as
pulses. Test (1500 m3 on the 25% gully): the lead surge tracks the mud
front to its stop at 148 m, the second surge follows at 3.3 m/s.

### Powder speckle checked against path tracing (19 Nov)

Tried: scatter texels holding light per unit of their opacity, multiplied
back by a full-resolution opacity march in the upsample (so the light
follows Flow's own full-res detail). On snow's avalanche powder it barely
changed the dark clumps, so it is not in. A path-traced still of the same
frame (640x360, 4 spp, blurred to strip noise) is as dark and as mottled
as the raster one (frame means 101 PT, 109 raster): the dark clumps are
the gas's real density seen against brighter snow and sky, not a raster
artifact. The brighter, smoother powder from before the parity change was
over-lit by the un-normalised octaves. Image:
/mnt/project-files/fork/physx/powder-pt-vs-raster.png (top raw, bottom
blurred; left PT, right raster).

### Gas single-scattering albedo (snow's request, 19 Nov)

The path tracer gave every Flow volume a fixed single-scattering albedo
of 0.9: each scatter keeps 90% of the light, so after the many scatters
inside a thick cloud little is left and snow powder goes grey however
white smoke_color is. Ice grains barely absorb in the visible (albedo
above 0.999, Warren 1982). PhysXFlow3D.scattering_albedo (default 0.9,
so smoke and dust are unchanged) now sets it. The raster pass weighs its
multiple-scattering octaves (Wrenninge et al. 2013) by albedo^i for
octave i, and the whole by albedo, both relative to 0.9, so its default
look is unchanged (powder frame mean 109.2 before, 109.5 after) and a
whiter gas keeps more of its deep light there too. Three octaves only
approximate the many orders PT follows, so raster brightens less than PT
for very white gas. Test puff, mean grey down its middle at 0.9 and 0.99:
raster 155.5 and 157.6, path traced 145.0 and 149.5.

Closing that gap (coordinator, 19 Nov evening): the orders past the third
are added as one wider octave (exp(-tau/8)) weighted by the geometric
tail sum over n >= 3 of (c albedo)^n, counted only above the 0.9 default,
so the default look is untouched. c = 0.6 fitted on the test puff; gain
from 0.9 to 0.99, middle grey (raster vs path traced): test puff +3.0% vs
+3.1% (raster 155.5 -> 160.1), denser full-res puff (check, not fitted)
+1.6% vs +1.2%. The absolute raster/PT gap at the default (about 7% on
the puff) is unchanged by design.

### Flow raster light matches path tracing, refit (19 Nov evening)

The first parity check (above) was wrong: its test puff sat under a plain
colour background, which Godot's raster lights the gas with (ambient
colour) and the path tracer does not, so raster looked too bright and the
octaves were normalised down to 1 at zero depth. Under a real sky, each
light on its own (same puff, middle of the gas against the sky):
- Sun only: raster 101 vs PT 129 (raster too dark). The raster single
  scattering also lacked the 0.9 albedo PT applies to every scatter.
- Sky only: raster 119 vs PT 79 (raster far too bright, brightest on
  top where PT is evenly dark: a grey-albedo medium lit from all around
  reflects little, Chandrasekhar's semi-infinite slab).
Refit: the in-scatter carries the albedo; the octaves give 1/0.95 of
single scattering at zero depth for the sun (SUN_MS_GAIN); the sky keeps
0.27 of the octave light (SKY_MS_SHARE) and reads 0.15 of the optical
depth straight up as its occlusion (SKY_OCCLUSION: the hemisphere's paths
out of the gas are mostly shorter). Results down the gas column, rows
top to bottom (raster vs PT): sun only puff 127 vs 129; sun only dense
puff 165 vs 160; sky only, column 105 79 76 71 68 66 65 vs 92 77 70 66
64 61 59; sun and sky, right column 184 165 146 129 119 120 117 vs 180
165 147 132 122 118 114. Sunlit smoke is brighter than before and its
shaded sides darker.

Banding (snow's powder): the sun's and sky's volumetric shadow maps took
a fixed 64 steps over the whole grid along the light, about a metre per
step through the avalanche cloud, so each step added a dense cell's whole
optical depth and the shading stepped in horizontal bands. They now step
once per gas cell along the light (up to 512) with a slice per two cells
(up to 128).

Snow's powder pair, same evening. Three more findings, each checked
against a path-traced still of the same avalanche frame (640x360, 4 spp):
- Flow's own raster ray march took each step's opacity as min(smoke, 1)
  times its step opacity, so gas denser than smoke 1 drew no denser.
  Patched to Beer-Lambert with the smoke scaling the optical depth,
  clamped at 2 as the path tracer's density majorant clamps it
  (misc/physx_patches/0010); the scattering pass clamps at 2 as well.
  Small on this powder, larger on thick smoke.
- The soft, translucent look of raster powder next to PT's billows is
  the light's resolution: the scattering pass runs at a quarter of the
  frame each way, so at 640 or 960 wide a texel spans about a gas cell
  and the cell-scale light and shade of the billows is averaged away.
  Full resolution shows the billows; half is between; the shadow map's
  size (128 or 512) makes no visible difference. At 1080p a quarter is
  as many texels as half at 960. Docs and test stills at low resolution
  should set scattering_resolution to full.
- The tail for scattering orders past the octaves (above) overshot once
  the sun's light was refit; it is gone. Powder clouds, mean grey (raster
  full res vs PT blurred): left cloud 86 vs 78, right column 100 vs 90,
  low band 115 vs 114.

Refit after the Flow opacity patch (same night): with dense gas now
properly opaque in Flow's own march, deep gas in raster came out darker
than PT (sun only, deep rows 55 vs 84). Refit on the puffs: SUN_MS_GAIN
1.3; the tail of orders past the octaves back, now at every albedo
(sum over n >= 3 of (0.45 albedo)^n); SKY_MS_SHARE 0.25. Puff under sun
and sky, rows down two columns against 32-spp PT: default albedo rms
5.6%, mean +2%; albedo 0.99 rms 9.6%, mean -6%. Against a fresh 16-spp
PT still of snow's powder (mountainside bench, 6 s) that fit left the
powder 14-22% bright, and no octave or tail setting fits both scenes, so
the constants split the difference: SUN_MS_GAIN 1.1, SKY_MS_SHARE 0.18.
Puff default mean -2% (rms 7%), puff at 0.99 mean -15%, powder band +3%
and its left column +9%.

### Gas gets its own path-tracing budget; ground light under raster gas (20 Nov)

Snow's v6 powder pair: path tracing read thinner and more see-through
than raster, raster had a dark, crisp fringe at the cloud base where PT
is soft and lit, and docs asked for an extinction/albedo check. Both
paths take the same numbers: extinction is smoke_density per metre times
the smoke clamped at 2, albedo is smoke_color times scattering_albedo.
The gaps were elsewhere:
- PT ended a path after the scene's bounce count, and every scatter in
  the gas counted as a bounce. Snow's bench uses 3, so light that
  scatters a dozen times through a white cloud was cut off and the cloud
  went grey and see-through. Gas scatters now have their own budget (32,
  RT_VOLUME_MAX_SCATTERS) and stop counting against the surface bounces
  (the usual split in production path tracers, e.g. Arnold's and
  Cycles' separate volume depth).
- Raster had no light from the ground. Under PT the sunlit ground below
  a cloud lights its base (snow reflects about 0.8), so the base is the
  brightest part; raster lit it from the sky's lower half only, hence
  the dark fringe. PhysXFlow3D.ground_albedo (default 0.25, powder 0.8)
  now adds the ground's light: the sun on the box floor below each
  point, in the gas's own shadow (one more fetch of the sun's volumetric
  shadow map), plus the sky, through the gas below the point.
- Thin gas: raster gave wisps the octaves' full multiple scattering.
  Light scattered more than once needs gas around it, so the octaves past
  the first now ramp in with optical depth (1 - exp(-2 tau), Schneider
  2015's powder term turned around), and thin edges stay see-through.
Test puffs under sun and sky against fresh 32-spp PT, rows down two
columns: albedo 0.9 mean +0.5% (rms 2.9%), albedo 0.99 mean +1.4% (rms
3.5%), with the constants unchanged (SUN_MS_GAIN 1.1, SKY_MS_SHARE 0.18)
and the ground light unscaled. Before, the same puffs were -16% and
-17% against the new PT. Snow's powder at 6 s (640x360, PT 16 spp against raster): the PT cloud
is no longer grey and see-through; mean grey down the cloud, rows top to
bottom, PT 150 159 149 132 120 114 109 107 112 114 115 vs raster 142 137
122 114 107 107 108 112 113 112 106 (raster -7%, lit top 10-14% under,
base even). Still open: PT's cloud is bluer (more sky light) where raster
is neutral grey, and raster's base keeps a thin crisp edge, which a test
of gas resting on flat ground traced to Flow's own opacity edge over a
shaded underside (about 20% under the body), not to a lighting fault.
Also: the light march now covers its stretch of the ray exactly
(stratified steps, the last cut at the scene) instead of skipping the
stretch before its first jittered sample.

### Raster gas lit from behind the camera (20 Nov)

Snow's v8 powder pair, sun on the camera's side so nothing shadows the
cloud: PT near white (mean grey 189 down the cloud), raster mid grey with
dark blotches (135). Raster gave every multiple-scattering octave the
same forward-leaning phase as single scattering (HG g 0.4), so a cloud
lit from the front got only the weak backward lobe of all its light.
Many scatters spread light evenly, so each octave now takes the
anisotropy g * MS_ECCENTRICITY^i (Wrenninge et al. 2013's eccentricity
attenuation, as in Frostbite's clouds, Hillaire 2016). Fitted on a
front-lit test puff (new 32-spp PT reference) together with the side-lit
ones: MS_ECCENTRICITY 0 (octaves isotropic), the tail of orders past
the octaves at c 0.6 (was 0.45), SUN_MS_GAIN 1.05 (was 1.1). Test puffs
against PT: front-lit +0.1% (rms 2.2%), side-lit +0.1% at albedo 0.9 and
+2.7% at 0.99. Snow's powder (640x360): raster 135 -> 157 against PT 189,
blotches gone; PT's frame is about 4% brighter on the plain snow too, so
about 13% is left, in large dense powder only (a deep cloud of albedo
0.99 reflects nearly like snow, through orders the octaves still cut
short). Raising the tail further brightens the puffs 6-11% past PT.

### Raster powder: rim, sky colour, sky against sun (20 Nov)

Docs' second read of snow's front-lit powder (v8 on 9d213a78): still grey,
a dark dotted rim, no blue cast, billows smoothed.
- Rim: the light pass and Flow's own opacity march sampled the gas with
  different jitter, so at thin edges Flow drew gas the light pass had not
  lit (and the reverse). Flow's ray march now hands over its
  transmittance in alpha (misc/physx_patches 0011); the light pass returns
  light per unit of opacity and the upsample weighs it by Flow's own
  coverage, so light and coverage come from the same samples. The frame
  keeps its own alpha (the copy back keeps the destination's).
- Blue cast: on the test puffs raster matched PT's brightness but was
  neutral (blue/red 1.05 against PT's 1.23). Sky-only puffs: raster half
  as bright and greyer (1.36 against 1.69). Three causes: the ground-light
  change had halved the sky (neutral, sunlit ground made up the
  brightness); without a sun the ground was lit from the sky's own
  stand-in lower half (a dull brown) rather than the blue sky; the sky
  taps were the radiance map's last mip, which averages the whole sphere,
  plus a tap along the view toward the pale horizon. The sky is now five
  taps over each half four mips above the last, the ground is lit by the
  sky above it, and SKY_GAIN 3.0 against SUN_MS_GAIN 0.7 is fitted to a
  new sky-only PT puff with the sun-and-sky ones.
Test puffs against PT: front-lit -1.4%, side-lit +1.0% (0.9) and +2.7%
(0.99), sky-only -6.8%; blue/red 1.09-1.11 against 1.21-1.26 (sky-only
1.56 against 1.69). Fine billow detail under front light (docs' point 2)
is not addressed: evenly scattered light carries no self-shadow, and the
PT's fine structure is mostly its 16-spp grain over a sparser cloud.
Snow's front-lit powder (640x360): mean grey 155 against PT 189 (156
before), blue/red 1.067 against 1.087 (1.035 before), the dotted rim
gone. The brightness gap in this large, dense albedo-0.99 cloud is
unchanged: light that goes deep and diffuses back out of the lit face is
beyond the octaves (a diffusion term, as in subsurface scattering, is
the next step).

### Raster powder: the thin-gas ramp looks along the sun's whole line (20 Nov)

The powder cloud is three times thinner per metre than the test puffs
(smoke_density 1.2 against 4), and the ramp that keeps wisps single
scattering judged "thin" by the gas between the point and the sun alone.
Lit from the camera's side, the visible face of a big cloud has almost
no gas toward the sun, so it got single scattering only, though deep gas
lay behind it to scatter light back. The ramp now takes the gas along
the sun's whole line through the point (the sun map's column: toward the
sun plus beyond). A new 32-spp PT puff at the powder's density lit from
the camera's side: raster -5.0% -> -1.1%. Dense puffs: front-lit +2.8%,
side-lit +1.4% (0.9) and +3.2% (0.99). Snow's powder: mean grey 155 ->
164 against PT 189 (PT's plain snow is 4% brighter too). A diffusion
term (similarity-theory albedo and decay, light going deep and coming
back out of the lit face) was tried: at a strength that would close the
rest it puts the test puffs 8-18% over PT, so it is not in.

### Raster gas: the gas colour compounds over the scattering orders (20 Nov)

Research: Wrenninge et al. 2013 (Oz) octaves with attenuation a, b and
eccentricity c; Hillaire 2016 (Frostbite clouds) the same with an
energy-conserving step; Hillaire 2020 (scalable sky) the orders past the
second as a geometric series 1 / (1 - f_ms); Kallweit et al. 2017
(Disney) a network trained on path-traced clouds, not usable here
without its training data. A per-point f_ms (Hillaire 2020) from the gas
around each point (up, down, toward and away from the sun, back to the
camera) was built and tried first: it darkens thin gas against dense,
the wrong way for the powder, so it is not in.

What was really wrong: the path tracer's albedo at each scatter is the
gas colour times its scattering albedo (raygen.glsl), so the colour
compounds: the n-th order carries its n-th power. Raster weighed the
octaves by the scattering albedo alone and tinted once at the end. The
test puffs are grey-blue (0.92, 0.94, 0.97, linear 0.83 to 0.93), so
under path tracing their deep light is much darker and bluer than
raster's, and raster's octave gains had been fitted down to it; the
powder is near white (0.98, 0.99, 1.0), keeps nearly all of its deep
light under path tracing, and came out grey in raster. Black gas in
both renderers on snow's bench gives the same silhouette, so the
cloud's opacity already matched and its "see-through" look under path
tracing is only its brightness against the snow behind.

Built: each octave takes the per-channel albedo (colour times scattering
albedo) to its order's power, the orders past the third a geometric
series in 0.68 times it, single scattering the albedo once; the final
colour multiply is gone. Refit on the 32-spp puffs: SUN_MS_GAIN 0.7,
SKY_MS_SHARE 0.21 (was 0.19), series 0.68 (was 0.6). Puffs: side-lit
+1.8% (0.9) and +3.1% (0.99), front-lit +2.2%, powder-density -1.2%,
sky-only -3.8% (was -7%); B/R 1.10 to 1.17 against PT's 1.13 to 1.26
(was 1.07 to 1.11). A dense powder block on bright ground: B/R 1.18
against PT 1.28 (was 1.08). Snow's powder (one-run pair, same gas): mean grey 166 -> 174 against PT
189, B/R 1.051 -> 1.075 against 1.087; the cloud now reads pale
blue-white over the snow. PT's plain snow is 4% brighter than raster's,
so about half of the rest is the scene, not the gas.

### Raster powder: no dark seam where the gas meets the snow (20 Nov)

Docs saw a faint dark seam along the powder's lower edge. Two causes:
- The ground light under the gas (the snow lit by the sun, in the gas's
  shadow) was taken at the Flow box's floor. On a slope that floor lies
  under the snow, where the line to the sun crosses the whole cloud, so
  the gas darkened toward its base (170 -> 147 over its last rows). It
  is now also taken at the bottom of the gas column under the point,
  found by bisection in the sky's shadow map, and the better lit of the
  two is used (the ground lies at or below both; for gas floating over
  flat ground the box floor is right, for gas lying on a slope the gas
  bottom). Test puffs unchanged to the pixel.
- A 1-2 pixel line where Flow's own march drew gas but the scatter march,
  ending at the snow a step earlier, found none, so the sliver showed
  Flow's unlit colour. The upsample now fills such pixels with their
  neighbours' light (a hole fill, 5x5 texels).

Follow-up (docs: a dotted line still showed at the contact). A render
with the gas hidden keeps the upper line along the release outline, so
that one is the snow's crown wall. The lower one is the gas's own: drawn
alone, its light falls from about 172 to 70-120 over the last few pixels
of thin gas against the slope (Flow's coverage is partial there, so the
snow behind shows through a darker veil). Under path tracing that base
dims only slightly (180 -> 173): light diffusing down through the cloud
to its foot, which the octaves' falloff under-gives. Not fixed yet. Two
robustness changes found on the way, neither of which moves the line:
the march's step budget counts only samples in gas (a ray grazing the
ground could spend it on empty blocks), and the bilateral upsample skips
texels whose march found no gas instead of blending them in as black.

### Raster powder: the cloud's foot, sky aperture and forward-scattered shadow (20 Nov)

Docs and snow asked to chase the darker foot. Split into sun, sky and
ground light on snow's bench, the sun held to the foot while the sky
(79 -> 32) and ground (134 -> 30) parts collapsed: raster judged both by
the gas straight above and straight below the point only. Unreal's cloud
ambient occlusion has an aperture (straight up to a whole hemisphere)
for the same reason. Built: the share of a point's sky and ground that
lies toward the camera (SKY_OPENING, 0.5) is open as far as the gas the
view ray crossed lets through. Test puffs: within 3.6%; sky-only -3.8%
-> -1.3%. The broad darkening of the foot is gone.

The last pixel-thin line was the snow under the foot, in the gas's own
cast shadow, seen through the thin edge (gone with the gas's shadows
off). Raster's gas shadow on the scene was Beer's law on the optical
depth: the direct beam only, where a bright, forward-scattering gas
passes much of its scattered light on toward the ground as well. Built:
the delta-Eddington scaling (Joseph, Wiscombe and Weinman 1976), the
forward peak's share g^2 of each scatter taken out of the extinction,
exp(-tau (1 - w g^2)), with w the gas's scattering albedo times its
colour's luminance and g its anisotropy, passed to the scene shader in
the directional light's spare words. Dark or isotropic gas keeps Beer.
A near-white powder block's ground shadow against PT: 118 (Beer) -> 127,
PT 125; the two-stream slab total (Meador and Weaver 1980) was tried
first and gave 134, too bright for a finite cloud. The grey-blue test
gas already matched PT and barely moves.
On snow's bench, the darkest point along the cloud's lower edge
(averaged over the edge): 161 -> 167 against a body of 175, a dip of 8,
as under PT (180 against 188).

### Window flames as tongues (picked 19 Nov afternoon, first of three)

Research: EN 1991-1-2 Annex B and Law (1978) give a window plume as wide
as the opening, leaving through its upper two thirds, standing off the
wall by about a third of the opening's height, and rising along the
facade to the flame length. Built: the flame emitter is a slot across
the opening's top (box, wide as the opening, 0.35 of its height), and a
thinner slab of fuel stands against the wall over the lower half of the
flame length, so Flow burns the gas along the facade rather than in a
ball at the window. Wind bends both. Left: a door's flames, being wider
and taller, still read broad.

### Wind-driven building fires (picked 19 Nov afternoon, second of three)

Research: in NIST and FDNY's wind-driven fire tests (Kerber and
Madrzykowski 2009), wind of 9-10 m/s into a broken window drove
corridor temperatures over 1000 C and pushed the flames out through the
leeward openings. Built: wind pressure per opening, Cp +0.7 facing the
wind, -0.5 in the lee, -0.6 on the sides and roof (EN 1991-1-4); each
room's pressure balances the orifice flows (Cd 0.6) to the outside and
the rooms next door (four Gauss-Seidel sweeps, bisection per room). Air
driven in raises the ventilation limit to 3 MJ per kg of air (Huggett);
flames leave only by openings the gas flows out of, taking the inflow
openings' share; gas pushed through a door adds its heat over the door's
area to the next room's dose. Test (rooms A, B beside it, C above,
wind 10 m/s into A's door): C, lit by the flames out of A's window,
catches at 12 min instead of 24.

### Smouldering rubble and rekindles (picked 19 Nov afternoon, third of three)

Research: char and timber under the ash of a burnt or hosed-out room go
on smouldering for hours; smoulder fronts speed up with air flows of
only centimetres a second through the pile (Ohlemiller 1985), so wind
fans them, and a smoulder can turn back to flame (Rein 2009). Rekindles
are a recognised cause of second fires hours after crews leave (NFPA),
mostly from deep pockets a hose stream did not reach. Built: a burnt-out
room's ember (0..1) falls by e in about 30 min in still air, faster
under rain or water. Where the debris lies open (fallen in, roofless or
with an opening to the outside), ground wind over 0.5 m/s fans it up
logistically at (u - 0.5)/1200 per second. Water leaves deep pockets
worth 0.15 of the share of fuel left. At 0.6, with over 5% of its fuel
left and dry, the room relights (signal rekindled). Glowing debris in a
wind over 4 m/s sheds embers that hop downwind, and thin, cool smoke
drifts off it. Test (A hosed out at 25 min): in still air its ember dies
within the hour; in an 8 m/s wind it relights 80 min after the hose
stopped.

### Next-round options (card, 19 Nov evening)

Firefighters' hose streams. apply_water already knocks fires down.
Plan: a hose node whose jet is a PhysX particle stream (a solid stream
reaches about 20 to 30 m; Freeman's nozzle formula), aimed at
openings, feeding apply_water where it lands, with steam and the
knock-down seen through the windows.

Embers through vents and gutters. Post-fire surveys and IBHS tests show
embers getting in through attic and foundation vents and catching the
dry debris in gutters and on decks; mesh of 3 mm or finer stops most
(Manzello et al. 2011, NIST ember generator). Plan: vents and gutters
as openings with a mesh size and a debris load, which brands can light.

Fire in roof spaces and voids. Fire that gets into an attic or a wall
cavity runs unseen along it and comes out far from where it started;
it is a common way a whole roof goes (NFPA; BRE). Plan: concealed
voids as rooms with a low fuel load and no openings to the outside,
linked by gaps to the rooms below and next to them.

Raster smoke speckle (fix, no pick needed): opacity-matched upsampling
of the quarter-resolution light (the light per unit of the low-res
opacity, times the full-res opacity).

### Next-round options (card, 19 Nov afternoon)

Window flames as tongues. External flames now read as round fireballs
because Flow burns the fuel in a ball around a sphere emitter. Real
window plumes are flat sheets that hug the facade above the opening and
taper, at Eurocode's length (Law 1978; EN 1991-1-2 Annex B). Plan: slot
emitters along the top of the opening, angled up the wall, with fuel
rising to the flame tip, and the wall acting as a solid in Flow.

Wind-driven building fires. Wind on a broken window pushes air through
the building: the room burns hotter and flames and smoke move toward the
leeward openings (NIST and FDNY wind-driven fire experiments, 2008-2009;
Kerber and Madrzykowski). Plan: wind pressure on each opening
(Cp ρ u²/2) drives the flow between rooms, and the leeward openings
carry the flames.

Smouldering rubble and rekindles. Collapsed timber keeps glowing for
hours under the ash, and wind can reignite it, which causes many second
alarms (NFPA rekindle data). Plan: collapsed rooms leave glowing debris
using PhysXSmoulder, which throws brands and can relight nearby fuel in
wind.

Firefighters' hose streams. Water reaching rooms is in already
(apply_water). Plan: a hose node whose jet is a PhysX particle stream
(Freeman's nozzle flow, a reach of about 30 m) that aims at openings and
feeds apply_water where it lands, plus steam and the knock-down seen
through the windows.

### Next-round options (card, 19 Nov late morning)

Water on burning buildings. Hose streams and MPM water reaching a room
take heat at 2.6 MJ/kg (heating and boiling); a room needs about 0.1 to
0.3 kg/s per MW to knock the flames down (Sardqvist's critical flow
rate). Steam expands about 1700 times and pushes the hot layer out of
the openings. Plan: water particles entering an opening or hitting the
cladding feed PhysXSteam and a cooling term in the room's heat balance.

Smoke ahead of the fire. A two-zone layer (Quintiere; CFAST) fills each
room from the ceiling. The smoke leaves through the top of the openings
above a neutral plane and passes through doors to the next rooms well
before any flames do. That gives smoke from the eaves and grey windows
in the early minutes. Plan: a layer height and smoke density per room,
drawn as Flow smoke at the openings.

Windows and roofs that break. Glass that falls out at 450 C drops as
Blast shards (Pagni and Joshi's crack pattern from the frame edge).
Roofs sag and fall in where the room collapses, rather than the whole
target breaking at once. Plan: optional glass and roof Destructibles per
opening and per room.

A settlement in a wind-driven fire. In wildland-urban interface fires,
most homes are lost to embers rather than flame contact, and home to
home spread then dominates (Maranghides and Mell 2011, Waldo Canyon;
Camp Fire NIST 2021). Plan: a village bench where firebrands and the
radiation between buildings carry a grass fire's front through a street.

### Next-round options (card, 19 Nov)

Research notes for the card that follows the four picks above.

Smoke haze dims the sun. Thick wildfire smoke reaches aerosol optical
depths of 1 to 5 at 550 nm, with a mass extinction of about 4.5 m2/g
and a single scattering albedo near 0.9 (Reid et al. 2005). The
Angstrom exponent of 1.5 to 2 removes blue light first, so the sun goes
orange and then red. Under dense plumes the day cools by a few kelvin
(Robock 1988). Plan: the fire's smoke mass rate (WorldFire.plumes gains
a smoke term) feeds a column-load field on the weather grid, advected
by the wind aloft and settling out over days. The weather's haze
(rt_params 80-84) and its sun read the field, so sun drying and air
temperature follow.

Post-fire mudflows. In the first one or two years, rain bursts on burnt
slopes steeper than about 23% start runoff-generated debris flows. USGS
thresholds sit near 15-minute intensities of 10-25 mm/h (Cannon et al.
2008; Staley et al. 2017). They grow by scouring rills and channel beds
as they run (progressive bulking; Kean et al. 2013). Plan: the burn
scar's runoff (water_repellency_at, runoff_at) already concentrates the
flow. A channel that passes the threshold picks up ash and soil into a
Bingham mud body (the mud thread's model) that runs down the gully, then
fans out and stops where the slope eases. The ground and mud threads own
that code, so it needs their sign-off.

Burning wooden buildings. Embers light buildings through vents, gutters
and decks (Manzello 2017). Inside, a room fire grows until flashover,
when the hot layer reaches about 600 C and the floor takes about 20
kW/m2 (Drysdale). After that it burns ventilation-limited at about
1.5 A sqrt(H) MW (Kawagoe), and flames lean out of the windows. Timber
members char at about 0.65 mm/min (EN 1995-1-2), which is the char
rate the stem foot already uses, and they fail as the section thins.
Plan: a PhysXBurnable3D building mode made of rooms with openings,
using flashover and Kawagoe's heat release, external flames as Flow
emitters, members charring, and the frame collapsing through Blast when
the remaining section falls under its load.

### Ray tracing cost

Props and Blast pieces: TLAS transforms only. Cloth: refit per frame with
staggered rebuilds, culled when off screen. Volume BLAS: AABBs only, rebuilt
when blocks are allocated or freed. Budget set by profiling on the RTX 4090.

### Order of work

1. Module on a branch, CI (SDK build + cache), engine selection, README.
2. Cloth (skinned and free), umbrella, wind-driven props, Blast props.
3. Traced Flow volumes, then the RR comparison on hardware.
4. Open-world checks: many static bodies, streaming by region, relative
   origin, debris pooling.

### Sources (research pass)

- https://www.nvidia.com/en-us/geforce/news/batman-arkham-city-graphics-breakdown-and-performance-guide/
- https://gdcvault.com/play/1015769/Enhancing-Games-with-Clothing-and
- https://developer.download.nvidia.com/presentations/2010/gdc/Authoring_Physically_Simulated_Destruction_with_NVIDIA_APEX.pdf
- https://dev.epicgames.com/documentation/en-us/unreal-engine/destruction-overview
- https://dev.epicgames.com/documentation/en-us/unreal-engine/clothing-tool-in-unreal-engine
- https://dev.epicgames.com/documentation/unreal-engine/panel-cloth-editor-overview
- https://dev.epicgames.com/documentation/unreal-engine/node-reference/Dataflow/SimulationAerodynamicsConfig
- https://dev.epicgames.com/documentation/en-us/unreal-engine/heterogeneous-volumes-in-unreal-engine
- https://dev.epicgames.com/documentation/en-us/unreal-engine/sparse-volume-textures-in-unreal-engine
- https://forums.unrealengine.com/t/pixelated-blocky-glitched-heterogeneous-volumes-in-pathtracing-5-3/1343243
- https://matthias-research.github.io/pages/publications/XPBD.pdf
- https://matthias-research.github.io/pages/publications/sca2012cloth.pdf
- https://d1qx31qr3h6wln.cloudfront.net/publications/paper_v3.pdf
- https://cs.dartmouth.edu/wjarosz/publications/novak14residual.html
- https://jannovak.info/publications/SDTracking/index.html
- https://cs.dartmouth.edu/wjarosz/publications/miller19null.html
- https://graphics.cs.utah.edu/research/projects/volumetric-restir/
- https://raw.githubusercontent.com/NVIDIAGameWorks/Streamline/main/docs/ProgrammingGuideDLSS_RR.md
- https://raw.githubusercontent.com/NVIDIAGameWorks/dxvk-remix/main/src/dxvk/shaders/rtx/pass/composite/composite.comp.slang
- https://nvidia-omniverse.github.io/PhysX/flow/index.html
- https://nvidia-omniverse.github.io/PhysX/blast/index.html
- https://developer.nvidia.com/blog/best-practices-for-using-nvidia-rtx-ray-tracing-updated/
