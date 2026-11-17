---
title: "Physics (PhysX 5)"
weight: 90
group: "Simulation"
summary: "PhysX 5 alongside Jolt: cloth, tearing, destruction, path-traced smoke, lit smoke in raster."
---

![PhysX, path traced: cloth flag in wind, a Blast wall broken by a ball, rigid crates, Flow smoke lit by a lamp](media/physx-pathtraced.webp)

Windows x64 and Linux x86_64. Choose `PhysX` or `Jolt Physics` in **Project Settings > Physics > 3D > Physics Engine**.
- Either engine: tearable cloth, skinned cloth, Blast destruction, Flow smoke and fire, ocean buoyancy, compute-shader fluids.
- PhysX only: vehicles, CUDA fluids and deformables.
- **Path-traced smoke and fire** (`rendering/pathtracing/simulated_gas`).
- **Lit smoke in raster** (PhysXFlow3D `ambient_scattering`, `sun_scattering`, `cast_shadows`, `scattering_temporal`): smoke takes sky and ambient light and the sun through scene shadows, and shadows the scene; `scattering_temporal` removes step banding at its edges.

  ![Smoke over a broken wall at a low side sun, raster, cast_shadows off and on](media/physx-smoke-shadow.jpg)

  *Smoke shadows: the plume throws a soft dark shadow across the floor toward the camera.*

- **Smoke on the ground** (PhysXFlow3D `terrain_floor`, `smoke_buoyancy`): Flow gas rests on the terrain and slides downhill, and heavier-than-air smoke pools as dust or fog.
- **Fire light and embers** (`PhysXFlow3D.fire_light`, `embers`): a burning Flow emitter lights its surroundings with a soft-shadowed light that flickers with the flame's puffing, and throws up glowing embers, in raster and path tracing.

  ![A campfire in a stone ring at night, lighting the ground and stones, with embers rising](media/physx-campfire.jpg)

  *Fire light and embers: the fire lights the stone ring and the ground around it, and embers rise and drift off.*

- **Particle fluid surface** (`PhysXParticleFluid3D.render_mode`, `foam_enabled`): particle fluids draw as clear water on a real isosurface mesh that casts shadows, shows in reflections and is path traced; Screen Space is the lighter alternative. Spray falls as clear drops, and foam and bubbles form only where water strikes water, so a smooth pour stays clear.

  ![Water pouring from a wall pipe into a wooden bucket, raster](media/physx-fluid-pour.jpg)

  *Particle fluid: a clear stream pours into a bucket and churns up foam and bubbles where it lands.*

- **Cloth tearing** (`PhysXCloth3D.tear_factor`, `physics/physx_3d/cloth/tearing`): overstretched edges split and the rip runs.

![Cloth tearing: a ribbon rips off its post and a flag frays in rising wind](media/cloth-tearing.gif)

- Builds without the SDKs skip the module (`modules/godot_physx/README.md`).
