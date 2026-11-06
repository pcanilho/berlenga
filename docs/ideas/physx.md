# PhysX 5 in this fork

Status: research note for later work. Nothing here is implemented.
Date: 2026-11-06. Scheduled after the NVIDIA rewrite (`nv-port`) is on
`main`, together with the other features taken from community forks
(see `godot-rtx-forks-survey.md`).

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
commit ("Based on uno1982/godot <commit>"), in a source comment, in
COPYRIGHT.txt, in CHANGELOG.md and in the README Credits, with the NVIDIA
BSD-3 notices for PhysX, Blast and Flow.
