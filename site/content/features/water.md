---
title: "Water"
weight: 60
group: "World"
summary: "Refraction, murky water, caustics, ocean and storm seas, currents, boat wakes, white water, rivers and waterfalls, live water that floods and drains, shore waves, camera under water."
---

## Light in water

- **Shadows on water** (always on): rivers, lakes and the sea take shadows from terrain, objects and clouds in raster, as they already do under path tracing, on the surface and inside the water, seen from above or below. The open sea's raster colour matches the path tracer's. At a low sun, waves shade their own lee sides (`wave_shadows`).
- **Refraction** (`rendering/pathtracing/refraction`): water and glass blocks bend light by their IOR, with total internal reflection and depth tint; glTF `KHR_materials_volume`, `KHR_materials_ior`.

  ![A pool with a checkered floor and coloured posts, refraction off and on](media/water-refraction.jpg)

  *Refraction off, then on: the posts shift at the surface and the floor darkens with depth.*

- **Murky water** (same setting): scattering media; `IOR`, `ABSORPTION` and `SCATTERING` in shaders; glTF `KHR_materials_scatter` draft.

  ![The pool with clear and with murky water](media/murky-water.jpg)

  *Murky water: a green glow fills the deep end.*

- **Caustics** (`rendering/pathtracing/caustics`; raster `caustics_enabled`): sunlight lines under rippled water. In raster, sunlight focused by the waves moves over the bed under clear water, following the scene's sun.

  ![A rippled pool, caustics off and on](media/caustics.jpg)

  *Caustics: bright lines on the pool floor.*

  ![Clear shallows over sand, raster, caustics_enabled off and on](media/water-raster-caustics.jpg)

  *Raster caustics: sunlight focused by the waves lies in a web over the sand of the shallows.*

- **Caustic cascades** (`rendering/pathtracing/caustics_cascades`): sun caustics out to 256 m, near lines unchanged.

## Sea and lakes

- **Clear water** (`clear_water`): in raster, as under path tracing, the bed shows through the shallows of sea, lakes and rivers, refracted, and fades with depth.

- **Path-traced ocean** (`PhysXWaterSurface3D`: `pathtraced_absorption`, `pathtraced_scattering`, `pathtraced_roughness`): waves refract, tint and scatter, with caustics on the seabed.

  ![Ocean shallows with floating crates, raster vs path traced](media/ocean.jpg)

  *Ocean: sand and caustic light through the water.*

- **Ocean swell** (`wave_cascades`): up to four wave layers give long swell under the chop without visible tiling, whitecaps where they add up, on a shallow-water (TMA) spectrum; set `depth` to the real depth for open sea (the 3 m default damps long waves).

  ![Deep open sea under overcast sky with three wave layers, raster](media/ocean-swell.jpg)

  *Ocean swell: three wave layers run long swell under the chop with no repeating pattern.*

- **Measured wave spreading** (`wave_spreading`, `swell`): Donelan-Banner spreading as Horvath (2015) fits it, with swell; slopes at 14 m/s match Cox and Munk's measurements.

  ![Sea surface height from above, 160 m at 14 m/s, four spreading models](media/ocean-spreading.jpg)

  *Wave spreading: cos² gave long parallel crests; the measured spreading breaks them up, and swell adds long crests back.*

- **Whitecaps by wind speed** (`whitecap_auto`; `whitecap_cover_scale`): whitecap cover follows the wind speed as measured at sea (Monahan 1980), whatever the wave settings.

  ![Sea from above at 6, 14 and 20 m/s, fixed whitecap threshold and whitecap_auto](media/whitecaps.jpg)

  *Whitecaps: with a fixed threshold the cover barely changes with the wind; now it grows from none at 6 m/s to many at 20 m/s.*

- **Storm seas** (`crest_spray`, `crest_spray_amount`): above 9 m/s of wind, breaking crests throw puffs of spume downwind; above 14 m/s, gale foam lies in thin broken streaks along the wind.

  ![Storm sea at 20 m/s looking downwind, without and with storm spray, raster](media/water-storm-seas.jpg)

  *Storm seas: at 20 m/s spume blows off the far crests and foam lies in broken streaks along the wind.*

- **Torn spray sheets** (`crest_sheets`): in a gale the wind tears thin sheets of water off folding crests, in raster and path tracing.

  ![A gale sea from 2 m up across the wind, without and with torn spray sheets, raster](media/water-spray-sheets.jpg)

  *Torn spray sheets: veils of spray stream downwind off the folding crests.*

- **Crisp sea foam** (`foam_crisp`, `foam_relief`): sharp patch edges, torn lobed holes and bubble relief, up close and at range.

  ![Sea foam on a rough sea a few metres away, without and with crisp foam, raster](media/water-foam-crisp.jpg)

  *Crisp sea foam: soft foam turns into torn patches with ragged holes, floating rafts and raised bubble relief.*

- **Crest glow** (`crest_glow`): low sun shines through a thin line along the high crest tops in the water's own colour, scaled to the sea's chop, in raster; path tracing already scatters through them.

  ![A low sun ahead over a moderate sea from 1.3 m up, without and with crest glow, raster](media/water-crest-glow.jpg)

  *Crest glow: the low sun lights the thin top of the near wave crest in the water's own turquoise.*

- **Spilling rollers** (`roller_strength`): breaking crests carry a raised, churning roller of white water.
- **Ocean detail** (`detail_waves`, `far_filter`; `bubble_strength`): short ripples down to 8 cm; on every wave cascade, waves smaller than a pixel pass into the surface roughness, so near and far sun glitter keeps the right width, and the far sea mirrors the sky through their mean Fresnel instead of turning into a pale sheet; whitecaps age into lace with bubble grain, and a turquoise bubble plume glows under fresh foam.

  ![Sun glitter on a moderate sea from 2 m up, without and with detail waves, raster](media/water-ocean-detail.jpg)

  *Ocean detail: the sun's path breaks into fine glints instead of a few smooth streaks.*

  ![A fresh whitecap seen steeply from 4 m up, raster](media/water-bubble-plume.jpg)

  *Ocean detail: a turquoise bubble plume glows under a fresh whitecap.*

  ![The far sea toward a low sun from 1.3 m up, without and with far filtering, raster](media/water-far-sea.jpg)

  *Far filtering: grainy far crests and dark posts along the horizon settle into smooth distant sea.*

- **Lake ripples** (automatic from `fetch`): on short fetches the wave grid gets finer, up to 256 points, so lakes keep their short wind ripples and sun glitter; open sea is unchanged.

  ![A lake looking into the sun, raster, with wind ripples and glitter](media/lake-ripples.jpg)

  *Lake ripples: short wind waves and sun glitter on a lake, where the surface used to lie flat.*

- **Raster reflections** (`raster_reflections` on PhysXWaterSurface3D and PhysXRiver3D): raster sea and rivers reflect the sky and the sun's glitter by the exact Fresnel term, matching the path tracer; raster waterfalls, white water, sea foam and wet sand take the scene's shadows, sky light and cloud shadows; the water's own colour is lit by sun and sky, so the sea darkens at dusk.

  ![Raster sea toward the sun, before and with raster_reflections](media/water-reflections.jpg)

  *Raster reflections: the flat dark sea now shows the sky and a band of sun glitter.*

  ![A raster waterfall in a shaded gorge, before and lit by the scene](media/waterfall-lighting.jpg)

  *Waterfall lighting: the raster fall no longer glows white in the shade of the gorge.*

  ![Shore foam in shade, raster, before and lit by the scene](media/sea-foam-lighting.jpg)

  *Foam lighting: shore foam in shade is no longer bright white.*

- **Finer water mesh up close** (`mesh_detail`).
- **Boat wakes** (`PhysXWaterWake3D.dispersive`, `breaking_slope`): waves stepped with deep-water dispersion in Fourier space form the Kelvin pattern, a 19.5 degree wedge of transverse and diverging waves.

  ![Wake heights from above at 4, 6 and 10 m/s, before and now](media/boat-wakes.jpg)

  *Boat wakes: before, a flat wedge; now transverse and diverging waves form the Kelvin pattern.*

## Shore

- **Shore waves** (`shore_waves`): swell turns with the seabed, grows in the shallows, breaks and runs up the sand (Stockdon run-up); lakes get shore waves from their own wind only.

  ![Waves breaking on a beach, raster](media/shore-waves.gif)

  *Shore waves: swell breaks into foam on the beach.*

  ![Waves run up a sloping beach and draw back](media/shore-runup.gif)

  *Run-up: the wash climbs the sand and drains back.*

- **Wave groups** (`shore_wave_groups`): shore waves arrive in sets with their own Rayleigh heights, so each breaks at its own place.
- **Plunging breakers** (`shore_wave_curl`, `shore_spray`, `shore_spray_amount`): the crest throws a sheet of water that lands ahead and sinks into the bore, with spray where it is thin and where it lands.

  ![A plunging breaker throwing a sheet of water with spray, raster](media/shore-curl.jpg)

  *Plunging breakers: the sheet thrown off the crest, with spray along its thin edge.*

- **Shore wetness** (`contact_wetness` on PhysXWaterSurface3D and PhysXRiver3D): sand, rocks and plants stay wet up to where the waves ran or the river reached, and dry with the weather.

## Currents and rivers

- **Currents** (`current`, `flow_period`): flow round rocks and along shores carries waves, foam and floating objects.

  ![Floating boxes drift on a current past a rock](media/currents.gif)

  *Currents: the boxes drift past the rock.*

- **Live flow** (`live_flow`, `wall_friction`): near the player, rocks and banks shed wakes and eddies. Splashes and wakes near the camera wash up beaches and banks and drain back.

  ![Flecks carried past a rock, baked flow vs live flow](media/live-flow.gif)

  *Live flow: the streaks roll up into a wake behind the rock.*

  ![A river with live flow: a rock sheds a swinging wake and a floating box swirls in it](media/river-live.gif)

  *River live flow: the wake swings behind the rock.*

- **Live water** (`PhysXLiveWater3D`, `streaming`): water from springs, rain or a breaking dam runs downhill over any ground, fills hollows and floods. The live area follows the camera; water left behind stays in place and flows again on return.
- **Buoyancy from shapes** (PhysXBuoyancy3D `volume_mode`, `mass_from_densities`): bodies take their volume from their collision shapes and their mass from per-shape densities, so wood, ice and steel hulls float at their real drafts.
- **Bodies push water** (`PhysXWaterPusher3D`): legs, hulls and props push live water, seas and rivers aside as they move, heaping water ahead and leaving a wake behind.
- **White water** (`flow_foam`): foam in fast water, trailing rocks and drifting downstream.

  ![Foam lines trailing a rock in a current](media/white-water.gif)

  *White water: foam peels off both sides of the rock.*

- **Rivers** (`PhysXRiver3D`: `discharge`, `roughness`, `max_slope`): drawn as a curve; finds its depth and never runs uphill.

  ![A river with a cascade, raster vs path traced](media/river.jpg)

  *Rivers: the cascade and the white water below it.*

  - Pools behind bumps, draws down before steps and ends fast reaches in a hydraulic jump; the surface banks up round bends (`gradually_varied_flow`, `bend_superelevation`).
  - A train of smooth standing waves below a weak hydraulic jump, and cross-waves where fast water narrows (`standing_waves`).
  - A river leaving a lake carries what the lake passes over its sill, drowned or free, and the lake settles at the level where its outlets pass its inflow (`source_lake`, `level_from_flows`).
  - Cascades into white water; `carve` emits channel brushes for a terrain deformer.
  - Rocks part the current and trail foam (`obstacle_collision_mask`); near the camera they shed eddies and a swinging wake (`live_flow`).

    ![A river parting round a rock, raster vs path traced](media/river-rocks.jpg)

    *Rivers round rocks: foam trails behind the rock.*

  - Joins lakes, the sea and other rivers (`mouth_target`), ponding to the water level; tributaries add their flow.

    ![Two rivers join and flow into a lake, raster vs path traced](media/river-joins.jpg)

    *River joins: two streams merge and spread into the lake.*

  - Waterfalls (`fall_min_height`): a falling sheet that thins, whitens and breaks up, with a boiling pool and spray.

    ![A waterfall from a river lip into a pool, raster vs path traced](media/waterfall.jpg)

    *Waterfall: the sheet breaks up into white water above the pool.*

  - Waterfall mist (`fall_mist_amount`, `PhysXWaterMist3D`): a simulated mist grid per fall, drawn as a raster FogVolume and path traced; the mist wets the rock and ground it drifts over (`wet_surfaces`).

    ![A waterfall into a pool, raster, before and with mist](media/waterfall-mist.jpg)

    *Waterfall mist: a haze now rises from the pool and drifts past the fall.*

  - Living waterline (`edge_lap_height`, `edge_relief`, `edge_foam`): edges lap with the flow, look ragged and carry a thin foam band.
- **Contact waterfalls** (`PhysXWaterfalls3D`, `contact_falls`, `wet_margin`, `surface`): water that clings to steep rock, as wide as its flow, clear at the lip and white below the inception point, with soft wandering edges, wet rock beside it and spray off ledges. Thin falls over stepped rock run ledge to ledge as threads with wet rock between them, as their flow sets (`rock_step`); big flows stay fully covered. The sheets follow cliff relief ledge by ledge.

  ![A contact waterfall running down a rock face, raster](media/contact-falls.jpg)

  *Contact waterfall: white water down the face, thinning at its soft edges, with darker wet rock beside it.*

- **Plunging waterfalls** (`plunge_falls`, `column_density`, `wind_exposure`): tall falls break up into a falling column of clumps slowed by the air, inside a spray veil and soft mist; where the face leans into the jet, the fall keeps to the rock and fans out down it.

- **Faster flow bakes** (`physics/physx_3d/water/multigrid_flow`): multigrid-preconditioned conjugate gradients bake the flow 7 to 10 times faster than SOR.

## Under water

- **Camera under water** (path traced automatically; raster with `underwater_enabled`): fog, scattering, Snell's window, image split at the water line. In raster, omni and spot lights glow and scatter in the water, as under path tracing.

  ![Under water near crates, raster vs path traced](media/underwater.jpg)

  ![The camera dives through the water surface](media/underwater-dive.gif)

  *Under water: light scattering and the surface seen from below.*
