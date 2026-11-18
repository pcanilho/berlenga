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
