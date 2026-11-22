---
title: "Physics (PhysX 5)"
weight: 120
summary: "PhysX 5 alongside Jolt: cloth, tearing, destruction, path-traced smoke."
---

![PhysX: cloth flag in wind, a Blast wall broken by a ball, rigid crates, Flow smoke](media/physx.gif)

Windows x64 and Linux x86_64. Choose `PhysX` or `Jolt Physics` in **Project Settings > Physics > 3D > Physics Engine**.
- Either engine: tearable cloth, skinned cloth, Blast destruction, Flow smoke and fire, ocean buoyancy, compute-shader fluids.
- PhysX only: vehicles, CUDA fluids and deformables.
- **Path-traced smoke and fire** (`rendering/pathtracing/simulated_gas`).
- **Cloth tearing** (`PhysXCloth3D.tear_factor`, `physics/physx_3d/cloth/tearing`): overstretched edges split and the rip runs.

![Cloth tearing: a ribbon rips off its post and a flag frays in rising wind](media/cloth-tearing.gif)

- Builds without the SDKs skip the module (`modules/godot_physx/README.md`).
