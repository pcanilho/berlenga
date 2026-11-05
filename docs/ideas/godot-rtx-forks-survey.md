# Survey of other Godot RTX forks

Status: assessment only. Nothing here is implemented on `main`.
Date: 2026-11-05. Our base for any follow-up: the NVIDIA rewrite (`nv-port`).

## Method

GitHub repository search for forks and related projects ("godot rtx",
"godot path tracing", "godot dlss"), then for each candidate: fetch its
branches, find the merge base with NVIDIA-RTX/godot (`nvidia-pt-dlss` or
`nvidia-pt-dlss-dev`), count the commits on top that are not upstream Godot
merges, and read the subjects and the relevant diffs. About 25 forks are
plain mirrors of NVIDIA's branch with no changes; they are not listed.
`Rman887/valence-engine-rtx` has its own notes (`valence-engine-rtx.md`,
`valence-engine-rtx-takeaways.md`) and `josephsmithberger/godot-metal-path-tracing`
has `metal-path-tracing.md`.

## Attribution

Same rule as the Valence notes: anything taken from these projects is
credited in the commit message (`Based on <owner>/<repo> <commit>`), a
source comment at the feature, COPYRIGHT.txt for copied code with its
licence, and the CHANGELOG line and README references. Licences are listed
per project below; check each before copying code.

## Worth taking

### Diginaat/godot (branch `dev`, 2026-11-02; MIT; PhysX module MIT by Wild Ox Studios, PhysX/Blast/Flow BSD-3)

NVIDIA's branch merged with current Godot master, plus:

- DDGI: a `DDGIVolume` node with ray traced irradiance probes (Majercik et
  al. 2019) for the Forward+ raster path: cascades around the camera,
  infinite-world scrolling, moving lights and objects, probe baking, DDGI-lit
  volumetric fog, adaptive update budget, half-resolution apply. Gives a
  raster mode real bounce light. Useful for an open world where full path
  tracing is too expensive, and as the GI of a lower quality tier.
- Planar reflections: `PlanarReflector3D` (pixel-projected reflections with
  a temporal filter) and a mirror pass that renders the reflected camera for
  the reflector covering most of the screen; reflection probes captured
  within a per-frame face budget and lit by DDGI. Raster-mode reflections on
  water, wet floors and mirrors.
- PhysX 5 physics server (`modules/godot_physx`, from uno1982/godot): GPU
  rigid bodies with CPU fallback, GPU particle fluids, sand/snow, gas, cloth,
  GPU soft bodies, NVIDIA Blast destruction with an in-editor fracture tool,
  NVIDIA Flow smoke and fire, vehicles, FFT ocean water with caustics, boats.
  Windows and Linux binaries; GPU dynamics need CUDA. The open-world game is
  the obvious user; it is a large module to carry.
- In-editor installer for the Streamline/DLSS runtime (asks, verifies the
  SHA-256, installs next to the editor). Fits our rule of not shipping the
  DLLs in releases.
- Path tracer: emissive meshes sampled as lights, importance-based light
  choice, alpha blend and refraction, custom `vertex()` displacement, fog.
  Mostly covered by NVIDIA's rewrite and our own work; compare when we do
  emissive lights.

### plankatron/godot (branch `linux-rtx-v4-dev`, 2026-11-04; MIT; NGX SDK under NVIDIA's licence, fetched at build time, not vendored)

- DLSS on Linux through the NGX SDK (super resolution and Ray
  Reconstruction), opt-out at runtime with `GODOT_NGX_NO_DLSS=1`. Our Linux
  builds have no DLSS today. Fixes worth noting: camera matrices passed as
  dangling pointers to NGX, the feature recreated when switching between SR
  and RR, specular motion vectors fed to RR.
- Small path tracer fixes from 2026-11-03/04, each worth checking against
  `nv-port`: surfaces trace with the default hit group while their own one
  compiles; the scene uniform set reused while unchanged; per-type bindless
  tables so array, 3D and cube samplers path-trace; CUSTOM0 to CUSTOM3 in
  custom hit groups; dFdx/dFdy stubs in custom hit shaders; material
  uniform arrays packed with the raster's std140 packer;
  `RAY_FOOTPRINT_X/Y` from the ray cone; the RR sky albedo guide is the
  tone-mapped sky, not a constant.
- Vulkan: retry device creation without ray tracing when the driver
  refuses it; do not chain BufferDeviceAddressFeatures next to
  Vulkan12Features.

### Puddlefunk/godot (branches `pt-diffuse-emission`, `pt-transmission-ui-tint`, 2026-08; MIT; godot_voxel MIT)

- Emissive triangle table built per frame on the host with alias sampling,
  a reverse map for MIS, and incremental updates for moving instances. A
  second reference, next to Valence's, for emissive lights.
- Delta dielectric transmission with Beer-Lambert attenuation of shadow
  rays through transmissive media; thin-walled diffuse transmission through
  BACKLIGHT; emission normalised into the RR diffuse guide (fixes smearing
  above about 2x glow energy).
- Sampling: Russian roulette past diffuse depth 2, R2 low-discrepancy draws
  at the first vertex, ray-cone texture LOD.
- godot_voxel vendored with a tetrahedral mesher, terrain streamed and
  stitched across LODs with LOD fade. Open-world terrain reference.

### m-bo-one/godot-rtx (2026-09-20; MIT)

Small fixes to check against `nv-port`: DLSS surviving the raster/path
tracing switch and getting the camera Streamline expects; FXAA stopped under
a temporal upscaler; raster DLSS fed the inverse manual camera exposure
instead of its own estimate; alpha-to-coverage surfaces stay opaque under
the tracer; a camera-hidden instance with GI_MODE_DISABLED leaves the
secondary rays; reflection atlas buffer freed with its face views.

### iniside/godot-rtx (2026-09-13; MIT; adds RTXDI and NRD, NVIDIA licences to check)

391 commits by one author, far from NVIDIA's design: the path tracer was
removed in favour of RTXDI (ReSTIR DI) + NRD denoising on raster surfaces,
RTXGI DDGI cascades, a Slang shader migration, and "microgeometry", a
Nanite-like paged geometry format rasterised and ray traced on the GPU with
cluster acceleration structures. Also open-world scene streaming: entity
scenes stored as text directories by grid cell and streamed around the
editor and runtime cameras on worker jobs, and directional shadow cascade
caching (cascades reused while the camera and casters hold still,
invalidated by caster bounds). Too divergent to merge. Reference for the
open world: cell streaming, cascade caching, microgeometry.

### Gl1ch5/godot-rtx (2026-10-11; MIT)

Hybrid ray tracing for the raster path through ray queries: RT ambient
occlusion, RT reflections, RT diffuse GI replacing SSIL, RT shadows for
directional, omni and spot lights, an edge-aware spatial denoiser. Written in
one day and not run on hardware (its README notes lavapipe does not execute
its ray queries). An idea for a mid quality tier between raster and full
path tracing; the code itself is unproven.

### Others with small useful items

- ofrik/godot-rtx `survivor-dlss-fixes` (MIT): free a retired context's
  Streamline viewport before reuse; WorkerThreadPool runs a group's leftover
  items while waiting on it.
- Opinder1/godot_RTX_DLSS (MIT): script access for manual ray tracing
  shaders and DLSS Frame Generation inputs.
- bailaiOWO/rtx-godot (MIT): fills the standard normal-roughness buffer from
  primary hits under path tracing (screen-space effects keep working).
- cn2005/godot-ntc (MIT GDExtension, LibNTC under NVIDIA's licence): NVIDIA
  Neural Texture Compression on load; 835 MB of 4K PBR sets shipped as 153 MB.
  Download size only, not VRAM. Relevant to an open world's install size.

## Not worth taking

Plain mirrors of NVIDIA's branch, CI-only or build-script forks
(laaielichter, Pcniado, yoshimune), and forks whose only changes are
upstream Godot merges (ofrik `worktree-dlss-fg-reflex`).

## Suggested order

1. Check the small fixes (plankatron, m-bo-one, ofrik) against `nv-port`
   and port the ones still missing. Low cost, several are bugs.
2. Linux DLSS through NGX (plankatron).
3. For the open-world game: DDGI and planar reflections as the raster
   tier (Diginaat), then PhysX 5 with Blast and Flow as a module.
4. Emissive lights: compare Valence, Puddlefunk and Diginaat before writing
   ours.
