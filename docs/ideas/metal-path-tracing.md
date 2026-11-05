# Metal path tracing: assessment of josephsmithberger/godot-metal-path-tracing

Status: assessment only. No integration work has started.
Date: 2026-11-05. Fork `main` at 7a537a2.

## Question

Could https://github.com/josephsmithberger/godot-metal-path-tracing give this
fork path tracing on macOS (Metal), next to the existing Vulkan path tracer and
DLSS Ray Reconstruction? Today the fork's macOS release build is raster only.

## What the project is

- A fork of NVIDIA-RTX/godot (`nvidia-pt-dlss`), Godot 4.8-dev. Its history
  contains the same base commit as this fork, `135dff3887 NVIDIA: Pathtracer +
  DLSS`. It is an engine fork, not a module or GDExtension.
- Branches: `nvidia-pt-dlss` (default: showcase, tests, benchmark, planning
  notes), `metal-rt` and `metal-rt-clean` (the engine port intended as a pull
  request to NVIDIA-RTX/godot). Tag `v0.5.0` (2026-07-22). No public binary
  release yet.
- Licence: MIT (Godot's). The port adds no new third-party code apart from a
  two-line change to the vendored SPIRV-Cross. MetalFX is an Apple system
  framework. Licence is not an obstacle.
- Maturity: one developer, built as a hackathon entry with heavy AI assistance
  (stated in its README). About 60 commits by one author on top of the
  NVIDIA base; the last is dated 2026-07-24 and nothing has landed since. The README calls it "an experimental demo fork ...
  not a production-ready renderer". NVIDIA-RTX/godot has not taken it: neither
  `nvidia-pt-dlss` (2026-11-02) nor `nvidia-pt-dlss-dev` (2026-11-03) contains
  the Metal RT files.

## How it works

The NVIDIA path tracer is written as a Vulkan ray-tracing pipeline (raygen,
miss, closest-hit, any-hit, intersection with `traceRayEXT`). SPIRV-Cross, which
Godot's Metal driver uses to turn SPIR-V into MSL, cannot lower those stages.
The port therefore:

1. Adds Metal acceleration structures (BLAS/TLAS build, refit, lifetime gated
   on GPU completion) to `drivers/metal` (about 3,400 new lines).
2. Re-expresses the path tracer as a compute kernel using ray queries
   (`GL_EXT_ray_query`), which SPIRV-Cross lowers to
   `metal::raytracing::intersection_query`. The kernel is a new file,
   `scene_raytracing_compute.glsl` (743 lines), that shares the material,
   light and BRDF includes with the Vulkan raygen shader but has its own main
   loop. An all-opaque fast path patches the generated MSL to use the native
   `intersector`.
3. Custom spatial shaders are compiled into the same compute kernel as one
   evaluator per material source, selected by a switch (no visible function
   tables). Alpha scissor runs before the hit is confirmed.
4. Uses MetalFX Temporal Denoised Upscaling (macOS 26) as the denoiser, fed
   with the path tracer's albedo, normals, roughness, depth and motion vectors.
   This plays the role DLSS Ray Reconstruction plays on Windows.
5. Gates at runtime: macOS on Apple Silicon (later commits add Intel Macs),
   `supportsRaytracing`, function pointers, GPU addresses, tier-2 argument
   buffers, macOS 13 or later. Without them it falls back to raster.
   `SUPPORTS_RAYTRACING_PIPELINE` stays false on Metal.

Shader Execution Reordering has no Metal equivalent and is compiled out.

## What it renders

A full path tracer, not a demo kernel: it renders the Bistro scene with
textured, alpha-tested and custom materials, skinned and blend-shape geometry,
multimeshes, procedural AABB geometry, and MetalFX denoising. Its own planning
notes still mark exported projects, CI, soak tests and M1/M2 coverage as
planned, and say "Metal matches Windows" is not yet a valid claim. Only one
machine has been tested (Apple M5, macOS 26.5).

## Performance claims

From its README, Apple M5 MacBook Pro, 1920x1080, Bistro, path tracer pass
only: 3.8 ms at 1 spp / 1 bounce, 18.4 ms at 4 spp / 2 bounces, 64.2 ms at
16 spp / 2 bounces. Whole-frame 53.8 FPS at 4 spp / 2 bounces. These are the
author's numbers, not reproduced here (no Mac available).

## Fit with this fork

A trial merge of its `nvidia-pt-dlss` head (fa56ac81) into fork `main`
(7a537a2), both descending from `135dff3887`:

- 12 files conflict, 31 conflict hunks, all in the path tracer
  (`render_raytracing.cpp` 11, `render_raytracing.h` 4, the raygen shader 3,
  `render_forward_clustered_pt.cpp` 3, `scene_shader_raytracing.cpp` 3, four
  shared includes, plus README and docs). The Metal driver files merge
  cleanly. The textual merge is a day or two of careful work.
- The real cost is behind the merge. Since `135dff3887` this fork has added
  about 1,000 lines to `scene_raytracing_raygen.glsl` and 550 to the shared
  includes: the traced light cache for transparents and particles, particles
  in reflections and refractions, the light grid with resampled importance
  sampling (up to 4096 lights), traced fog lit by omni, spot and sun lights,
  ray-cone mip selection and anisotropic filtering, emission without a
  texture, orthogonal camera rays, shadow-only casters on bounce and shadow
  rays, stochastic transparency for hashed alpha, and UV2/triplanar in custom
  shaders. Features in shared includes would mostly carry over. Features in
  the raygen main loop, or that depend on any-hit shaders, must be ported to
  the compute kernel by hand, and the any-hit work (hashed alpha, shadow
  transmittance) has to become ray-query candidate handling.
- After that, every path tracer change in this fork would have to be written
  twice (raygen and compute) and tested on both. NVIDIA's own branches have
  also moved a long way since July (their 2026-11-01/02 refresh: volumetric
  fog, decals, OIT, alpha, ray cones), and the Metal port does not track them.
- Validation needs a real Apple Silicon Mac, ideally M3 or later (its native
  intersector lane is gated to Apple9, M3 and newer; M1/M2 are untested). The
  cloud containers have no GPU and the Windows PC cannot run Metal. Whether
  GitHub's hosted macOS runners expose Metal ray tracing inside their VMs is
  unverified, so the CI release build could compile it but not test it.
- MetalFX denoising would replace DLSS RR on Mac. It is a different
  denoiser, so fine-detail behaviour would need its own raster-vs-PT checks.

## Options

A. Park and watch. Record the project, re-check when it lands in
   NVIDIA-RTX/godot or resumes. Mac stays raster. No cost.

B. Spike on a branch. Merge the port into a fork branch, resolve the 31
   hunks, build macOS arm64 in CI, run its test suite on a borrowed or owned
   M-series Mac, and list which fork PT features are missing on Metal. Two to
   four days plus Mac time. Answers "how far off" with evidence; nothing ships.

C. Full support. B, then port every fork PT feature into the compute kernel,
   add MetalFX settings, extend release CI with a tested Metal RT build, and
   keep both paths in step from then on. Weeks up front and a permanent
   doubling of path tracer work.

## Recommendation

A, unless a Mac with an M3 or newer chip is available for testing and macOS is
a target that matters soon. The port is real and technically sound, the
licence is fine, and it shares this fork's base, so it is the best starting
point if Metal path tracing is ever wanted. But it is a single-author
experiment that stopped in July, upstream has not adopted it, it lacks most of
this fork's path tracer work, and it cannot be tested from the current
machines. If upstream NVIDIA merges it, the cost of option C drops sharply,
because the dual-path maintenance would then be shared.
