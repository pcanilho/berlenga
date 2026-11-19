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

- **Smoke on the ground** (PhysXFlow3D `terrain_floor`, `smoke_buoyancy`): Flow gas rests on the terrain and slides downhill, and heavier-than-air smoke pools as dust or fog. In raster, smoke, dust and snow clouds shade themselves as under path tracing, darker in the body under bright tops.
- **Gas pushes bodies** (`PhysXFlow3D.push_bodies`; `PhysXCloth3D.flow`): Flow smoke, blasts and fire updrafts blow debris, Blast pieces, flags and tarps, and the cloth stirs the gas back.
- **Spreading fire** (`PhysXFireSpread3D`, `fuel_patchiness`): grass and leaf litter catch and burn at Rothermel's rate of spread, faster downwind and uphill and stopped by wet fuel or poured water (which billows up as white steam, also where fire meets a shore), behind a glowing front with Flow flames and smoke. The fire leaves a ragged burn scar: grass burns to charred stubble with glowing tips, small plants and crops char, tree bark chars to the flame length and leaves brown up to the scorch height. Burnt grass shows its ash, white-grey where dry fuel burnt out and black char where it was damp, thinned by wind and darkened by rain (`grass_ash`); strong wind lifts ash off cold burnt ground as low grey dust that darkens snow downwind (`blowing_ash`). Duff smoulders for hours behind the front under pale low smoke until heavy rain puts it out, and wind can relight it (`smouldering`); burnt soil sheds rain, and the runoff washes its ash away. The fire draws air in at its base, and nearby plants, hair, falling leaves, ash, embers, smoke and dust lean or drift into it.
- **Fuel moisture from the weather** (`PhysXFireSpread3D.use_weather_moisture`): fuel dries or damps toward the air's equilibrium moisture, and rain wets it, slows the fire and puts it out.
- **Burning objects** (`PhysXBurnable3D`, `embers`, `collapse_share`): wooden props catch fire from burning ground nearby or from embers piling at wall feet and on top faces, char with glowing cracks, smoulder in rain, go out under poured water, break apart as their timber chars through (EN 1995-1-2) with the pieces keeping their char, or carrying their flames and embers as they fall and smouldering where they land, and throw embers that ride their plume and the wind and start spot fires downwind.
- **Burning buildings** (`PhysXBuildingFire3D`: `rooms`, `growth_time`, `collapse_share`): wooden buildings catch from embers and ground fire; rooms flash over, the window glass breaks into falling shards (`glass_panes`) and flames run up the wall above as sheets bent by the wind; wind blows in through windward openings, pushes hot gas through the doors and drives the flames out to leeward; fire spreads room to room, ceilings and roofs sag and fall in, and the frame collapses. Burnt-out rooms smoulder for hours, smoking and shedding embers in the wind, and can rekindle. A hose or fluid water knocks a room back below flashover and puts it out, with steam pouring from its windows; a weak stream only holds it (`apply_water()`).
- **Burning logs** (`PhysXFireSpread3D.vegetation_field`): fallen logs and branches catch from the fire under them, flame for minutes, then smoulder with glowing bark.
- **Debris flows** (`WeatherRunoff3D.debris_flows`; `GroundDebrisFlow3D`: `levee_height`, `push_bodies`; `PhysXDebrisSurge3D`: `surge_count_max`, `max_boulder`, `stop_slope`): after a fire, intense rain on steep burnt basins sends debris flows down their channels in surges with boulders at the front, throwing mud; the flow gathers loose bed on the way and spreads into a fan of fresh mud where it stops, with levees along its edges, burying ruts and shoving logs and props aside.
- **Spot fires** (`PhysXFireSpread3D.spotting`, `spotting_scale`): burning grass, litter and logs throw embers that the plume lifts and the wind carries; one landing on dry fuel starts a new fire ahead of the front.
- **Crown fire** (`PhysXFireSpread3D.crown_fire`, `foliar_moisture`): trees and shrubs catch from the ground fire under them (Van Wagner): low crowns torch, high ones only scorch, dense stands burn crown to crown, and torching trees throw far more embers; fire whirls spin up on strong flanks in a cross wind, on steep lee slopes and where the terrain wind separates behind a crest, loft firebrands higher and run across the slope, spreading the fire sideways (`fire_whirls`); a torched tree drops burning twigs and stands as bare black branches over grey ash. Fire chars the stem feet: fire-killed trees stand as snags and, years later, snap at the charred foot in a storm and leave a stump.
- **Fire clouds** (`WeatherMap3D.fire_clouds`): a plume that rises past its own condensation level, above or below the day's cumulus, grows a pyrocumulus; the largest tower into a pyrocumulonimbus with lightning.
- **Fire light and embers** (`PhysXFlow3D.fire_light`, `embers`): a burning Flow emitter lights its surroundings with a soft-shadowed light that flickers with the flame's puffing, and throws up glowing embers, in raster and path tracing.
- **Heat shimmer** (`PhysXFlow3D.heat_shimmer`): hot air above fires bends the view behind it, following the simulated temperature, in raster and path tracing.

  ![A campfire in a stone ring at night, seen from above, lighting the ground and stones](media/physx-campfire.jpg)

  *Fire light and embers: flame tongues light the stone ring and the ground around it, and small embers streak up from the fire.*

- **Particle fluid surface** (`PhysXParticleFluid3D.render_mode`, `foam_enabled`): particle fluids draw as clear water on a real isosurface mesh that casts shadows, shows in reflections and is path traced; Screen Space is the lighter alternative. Spray falls as clear drops, and foam and bubbles form only where water strikes water, so a smooth pour stays clear.

  ![Water pouring from a wall pipe into a wooden bucket, raster](media/physx-fluid-pour.jpg)

  *Particle fluid: a clear stream pours into a bucket and churns up foam and bubbles where it lands.*

- **Cloth tearing** (`PhysXCloth3D.tear_factor`, `physics/physx_3d/cloth/tearing`): overstretched edges split and the rip runs.
- **Burning cloth** (`PhysXCloth3D.burnable`): cloth catches fire, chars with a glowing edge and burns through in holes until it falls apart.
- **Wet cloth** (`PhysXCloth3D` and `PhysXSkinnedCloth3D`: `weather_wetting`, `wetness`, `water_capacity`): rain soaks cloth by how it faces the rain and it dries in the air; wet cloth is heavier, darker, clings and is slow to catch fire, and worn clothing swings less, clings to the body and wets up to a ragged tide line when wading; river current drags worn clothing and loose flags and sheets under water, where soaked fabric sinks slowly and streams downstream.
- **Blast materials** (`PhysXBlastAsset.materials`, `bond_healths`): pieces carry several materials and each bond its own strength, so a limb breaks into bark, wood and leaves, and a twig joint gives before a trunk joint.
- **Blast without stalls** (`PhysXBlastAsset.prepare_render()`; `initial_linear_velocity`, `initial_angular_velocity`): piece meshes are built once per asset, off the main thread, so a large break does not stall the frame; destructibles can start already moving.
- **Streaming without stalls** (`physics/physx_3d/streaming/statics_per_step`): static bodies of streamed regions enter over a few frames, nearest first, and removals are batched; bodies with the same friction and bounce share one material, so the cost stays flat as the world grows.
- **World contact** (`physics/3d/world_contact/auto_join`): every moving body bends plants, presses prints into ground and snow, and leaves ripples in water.

![Cloth tearing: a ribbon rips off its post and a flag frays in rising wind](media/cloth-tearing.gif)

- Builds without the SDKs skip the module (`modules/godot_physx/README.md`).
