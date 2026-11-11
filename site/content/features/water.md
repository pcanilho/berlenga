---
title: "Water"
weight: 30
summary: "Refraction, murky water, caustics, ocean, currents, boat wakes, white water, rivers and waterfalls, shore waves, camera under water."
---

![A pool with a checkered floor and coloured posts, refraction off and on](media/water-refraction.jpg)

*Refraction off, then on: the posts shift at the surface and the floor darkens with depth.*

- **Refraction** (`rendering/pathtracing/refraction`): water and glass blocks bend light by their IOR, with total internal reflection and depth tint; glTF `KHR_materials_volume`, `KHR_materials_ior`.
- **Murky water** (same setting): scattering media; `IOR`, `ABSORPTION` and `SCATTERING` in shaders; glTF `KHR_materials_scatter` draft.
- **Caustics** (`rendering/pathtracing/caustics`): sunlight lines under rippled water.
- **Caustic cascades** (`rendering/pathtracing/caustics_cascades`): sun caustics out to 256 m, near lines unchanged.
- **Path-traced ocean** (`PhysXWaterSurface3D`: `pathtraced_absorption`, `pathtraced_scattering`, `pathtraced_roughness`): waves refract, tint and scatter, with caustics on the seabed.
- **Ocean swell** (`wave_cascades`): up to four wave layers give long swell under the chop without visible tiling, whitecaps where they add up, on a shallow-water (TMA) spectrum; set `depth` to the real depth for open sea (the 3 m default damps long waves).
- **Measured wave spreading** (`wave_spreading`, `swell`): Donelan-Banner spreading as Horvath (2015) fits it, with swell; slopes at 14 m/s match Cox and Munk's measurements.
- **Raster reflections** (`raster_reflections` on PhysXWaterSurface3D and PhysXRiver3D): raster sea and rivers reflect the sky and the sun's glitter by the exact Fresnel term, matching the path tracer; raster waterfalls, white water, sea foam and wet sand take the scene's shadows, sky light and cloud shadows.
- **Lake ripples** (automatic from `fetch`): on short fetches the wave grid gets finer, up to 256 points, so lakes keep their short wind ripples and sun glitter; open sea is unchanged.
- **Whitecaps by wind speed** (`whitecap_auto`, on; `whitecap_cover_scale`): whitecap cover follows the wind speed as measured at sea (Monahan 1980), whatever the wave settings.
- **Finer water mesh up close** (`mesh_detail`).
- **Shore waves** (`shore_waves`): swell turns with the seabed, grows in the shallows, breaks and runs up the sand (Stockdon run-up).
- **Wave groups** (`shore_wave_groups`): shore waves arrive in sets with their own Rayleigh heights, so each breaks at its own place.
- **Plunging breakers** (`shore_wave_curl`, `shore_spray`, `shore_spray_amount`): the crest throws a sheet of water that lands ahead and sinks into the bore, with spray where it is thin and where it lands.
- **Currents** (`current`, `flow_period`): flow round rocks and along shores carries waves, foam and floating objects.
- **Faster flow bakes** (`physics/physx_3d/water/multigrid_flow`): multigrid-preconditioned conjugate gradients bake the flow 7 to 10 times faster than SOR.
- **Live flow** (`live_flow`, `wall_friction`): near the player, rocks and banks shed wakes and eddies.
- **Boat wakes** (`PhysXWaterWake3D.dispersive`, `breaking_slope`): waves stepped with deep-water dispersion in Fourier space form the Kelvin pattern, a 19.5 degree wedge of transverse and diverging waves.
- **White water** (`flow_foam`): foam in fast water, trailing rocks and drifting downstream.
- **Rivers** (`PhysXRiver3D`: `discharge`, `roughness`, `max_slope`): drawn as a curve; finds its depth and never runs uphill.
  - Cascades into white water; `carve` emits channel brushes for a terrain deformer.
  - Rocks part the current and trail foam (`obstacle_collision_mask`); near the camera they shed eddies and a swinging wake (`live_flow`).
  - Joins lakes, the sea and other rivers (`mouth_target`), ponding to the water level; tributaries add their flow.
  - Waterfalls (`fall_min_height`): a falling sheet that thins, whitens and breaks up, with a boiling pool and spray.
  - Waterfall mist (`fall_mist_amount`, `PhysXWaterMist3D`): a simulated mist grid per fall, drawn as a raster FogVolume and path traced.
  - Living waterline (`edge_lap_height`, `edge_relief`, `edge_foam`): edges lap with the flow, look ragged and carry a thin foam band.
- **Camera under water** (path traced automatically; raster with `underwater_enabled`): fog, scattering, Snell's window, image split at the water line.

## Images

![The pool with clear and with murky water](media/murky-water.jpg)

*Murky water: a green glow fills the deep end.*

![A rippled pool, caustics off and on](media/caustics.jpg)

*Caustics: bright lines on the pool floor.*

![Ocean shallows with floating crates, raster vs path traced](media/ocean.jpg)

*Ocean: sand and caustic light through the water.*

![Open sea with one wave layer and with three](media/ocean-swell.jpg)

*Ocean swell: one layer repeats the same foam patches; three layers add long swell.*

![Sea surface height from above, 160 m at 14 m/s, four spreading models](media/ocean-spreading.jpg)

*Wave spreading: cos² gave long parallel crests; the measured spreading breaks them up, and swell adds long crests back.*

![Raster sea toward the sun, before and with raster_reflections](media/water-reflections.jpg)

*Raster reflections: the flat dark sea now shows the sky and a band of sun glitter.*

![Sea from above at 6, 14 and 20 m/s, fixed whitecap threshold and whitecap_auto](media/whitecaps.jpg)

*Whitecaps: with a fixed threshold the cover barely changes with the wind; now it grows from none at 6 m/s to many at 20 m/s.*

![A lake looking into the sun, raster, with wind ripples and glitter](media/lake-ripples.jpg)

*Lake ripples: short wind waves and sun glitter on a lake, where the surface used to lie flat.*

![Waves breaking on a beach, raster](media/shore-waves.gif)

*Shore waves: swell breaks into foam on the beach.*

![Waves run up a sloping beach and draw back](media/shore-runup.gif)

*Run-up: the wash climbs the sand and drains back.*

![A plunging breaker throwing a sheet of water with spray, raster](media/shore-curl.jpg)

*Plunging breakers: the sheet thrown off the crest, with spray along its thin edge.*

![Floating boxes drift on a current past a rock](media/currents.gif)

*Currents: the boxes drift past the rock.*

![Flecks carried past a rock, baked flow vs live flow](media/live-flow.gif)

*Live flow: the streaks roll up into a wake behind the rock.*

![Wake heights from above at 4, 6 and 10 m/s, before and now](media/boat-wakes.jpg)

*Boat wakes: before, a flat wedge; now transverse and diverging waves form the Kelvin pattern.*

![Foam lines trailing a rock in a current](media/white-water.gif)

*White water: foam peels off both sides of the rock.*

![A river with a cascade, raster vs path traced](media/river.jpg)

*Rivers: the cascade and the white water below it.*

![A river parting round a rock, raster vs path traced](media/river-rocks.jpg)

*Rivers round rocks: foam trails behind the rock.*

![A river with live flow: a rock sheds a swinging wake and a floating box swirls in it](media/river-live.gif)

*River live flow: the wake swings behind the rock.*

![Two rivers join and flow into a lake, raster vs path traced](media/river-joins.jpg)

*River joins: two streams merge and spread into the lake.*

![A waterfall from a river lip into a pool, raster vs path traced](media/waterfall.jpg)

*Waterfall: the sheet breaks up into white water above the pool.*

![A waterfall into a pool, raster, before and with mist](media/waterfall-mist.jpg)

*Waterfall mist: a haze now rises from the pool and drifts past the fall.*

![A raster waterfall in a shaded gorge, before and lit by the scene](media/waterfall-lighting.jpg)

*Waterfall lighting: the raster fall no longer glows white in the shade of the gorge.*

![Shore foam in shade, raster, before and lit by the scene](media/sea-foam-lighting.jpg)

*Foam lighting: shore foam in shade is no longer bright white.*

![Under water near crates, raster vs path traced](media/underwater.jpg)

![The camera dives through the water surface](media/underwater-dive.gif)

*Under water: light scattering and the surface seen from below.*
