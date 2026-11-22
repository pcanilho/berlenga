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
