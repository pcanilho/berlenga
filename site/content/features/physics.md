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
- **Gas pushes bodies** (`PhysXFlow3D.push_bodies`; `PhysXCloth3D.flow`): Flow smoke, blasts and fire updrafts blow debris, Blast pieces, flags and tarps, and the cloth stirs the gas back.
- **Spreading fire** (`PhysXFireSpread3D`, `fuel_patchiness`): grass and leaf litter catch and burn at Rothermel's rate of spread, faster downwind and uphill and stopped by wet fuel, behind a glowing front with Flow flames and smoke. The fire leaves a ragged burn scar: grass burns to charred stubble with glowing tips, small plants and crops char, tree bark chars to the flame length and leaves brown up to the scorch height.
- **Fuel moisture from the weather** (`PhysXFireSpread3D.use_weather_moisture`): fuel dries or damps toward the air's equilibrium moisture, and rain wets it, slows the fire and puts it out.
- **Burning objects** (`PhysXBurnable3D`, `embers`): wooden props catch fire from burning ground nearby, char with glowing cracks, smoulder in rain and throw embers that start new fires.
- **Burning logs** (`PhysXFireSpread3D.vegetation_field`): fallen logs and branches catch from the fire under them, flame for minutes, then smoulder with glowing bark.
- **Fire light and embers** (`PhysXFlow3D.fire_light`, `embers`): a burning Flow emitter lights its surroundings with a soft-shadowed light that flickers with the flame's puffing, and throws up glowing embers, in raster and path tracing.

  ![A campfire in a stone ring at night, seen from above, lighting the ground and stones](media/physx-campfire.jpg)

  *Fire light and embers: flame tongues light the stone ring and the ground around it, and small embers streak up from the fire.*

- **Particle fluid surface** (`PhysXParticleFluid3D.render_mode`, `foam_enabled`): particle fluids draw as clear water on a real isosurface mesh that casts shadows, shows in reflections and is path traced; Screen Space is the lighter alternative. Spray falls as clear drops, and foam and bubbles form only where water strikes water, so a smooth pour stays clear.

  ![Water pouring from a wall pipe into a wooden bucket, raster](media/physx-fluid-pour.jpg)

  *Particle fluid: a clear stream pours into a bucket and churns up foam and bubbles where it lands.*

- **Cloth tearing** (`PhysXCloth3D.tear_factor`, `physics/physx_3d/cloth/tearing`): overstretched edges split and the rip runs.
- **Burning cloth** (`PhysXCloth3D.burnable`): cloth catches fire, chars with a glowing edge and burns through in holes until it falls apart.
- **Blast materials** (`PhysXBlastAsset.materials`, `bond_healths`): pieces carry several materials and each bond its own strength, so a limb breaks into bark, wood and leaves, and a twig joint gives before a trunk joint.
- **Blast without stalls** (`PhysXBlastAsset.prepare_render()`; `initial_linear_velocity`, `initial_angular_velocity`): piece meshes are built once per asset, off the main thread, so a large break does not stall the frame; destructibles can start already moving.
- **Streaming without stalls** (`physics/physx_3d/streaming/statics_per_step`): static bodies of streamed regions enter over a few frames, nearest first, and removals are batched.

![Cloth tearing: a ribbon rips off its post and a flag frays in rising wind](media/cloth-tearing.gif)

- Builds without the SDKs skip the module (`modules/godot_physx/README.md`).
