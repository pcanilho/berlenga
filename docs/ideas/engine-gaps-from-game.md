# Engine gaps found in the game's scripts (22 Nov)

A sweep of the game's scripts turned up effects and systems it builds for itself. This note checks each one
against what the fork already has, names the established technique, and proposes how to fold it into the
existing systems (WeatherMap3D, BaseMaterial3D wetness and glaze, RainFall3D, the water bodies, PhysX)
instead of adding a separate one. Raster and path tracer must match for anything drawn: the same GLSL goes
into the path tracer's raygen, as rain_ripples() already does.

## What the fork already covers

- **Rain rings on puddles (game: puddle_ripples.gd).** Covered. `rain_ripples()` (Lagarde's procedural rings,
  raytracing_rain_inc.glsl) is drawn by BaseMaterial3D's standing water, the ground patch, PhysXRiver3D and
  PhysXWaterSurface3D, in both renderers, driven by `weather_rainfall_at`. The game only needs to port to it.
- **Head and eye gaze, limb IK, springs (game: gaze, IK, weight shift scripts).** Mostly covered upstream:
  LookAtModifier3D, AimModifier3D, TwoBoneIK3D, IKModifier3D, SpringBoneSimulator3D, BoneConstraint3D and
  LimitAngularVelocityModifier3D are all in the fork. The multi-bone gaze (neck, head and upper spine
  sharing the turn) is a chain of LookAtModifier3D with weights. Contact on skinned meshes and idle weight
  shift are game logic on top. This is a game port, not an engine gap.
- **Lamp-lit haze under path tracing.** Already exists.

## Gaps, grouped

### A. Rain on surfaces: glass and walls (game items 1 and 6)

The game's RainOnGlass (rain_on_glass.gd, 1,400 lines) simulates drops, merging, runners and rivulets on a
CPU grid, after Kaneda et al. 1993 ("Animation of water droplets on a glass plate"), with rivulet cusps and
pearling after Podgorski, Flesselles and Limat (PRL 2001) and Le Grand-Piteira, Daerr and Limat (PRL 2006).
The game's wall_film.gd lays streaks on windward walls by ISO 15927-3's wind-driven rain,
q = (2/9) U R^(8/9) cos(D - theta) (Lacy 1965; Blocken and Carmeliet 2004).

In the engine, BaseMaterial3D wetness (Lagarde 2013, "Water drop 3b") darkens and smooths surfaces by
WeatherMap3D's film. It has no wind direction, nothing runs down vertical faces, and transmission glass gets
nothing.

**Proposal: one "surface rain" layer in BaseMaterial3D, on by default.**
- **Catch.** For each pixel, the catch is the wind-driven rain on that face: ISO 15927-3 with the weather's
  rain map and wind map (both already globals), using the world normal against the wind. Faces that look up
  catch the vertical rain. Faces in the lee and under cover (the shelter map) catch none.
- **Drops and runners.** These are stateless and procedural, so raster and path tracing match without a
  per-object simulation. Layers of hashed cells hold sitting drops, and runners slide down the surface's
  fall line (gravity projected on the face, leaned by the wind) with a trail of beads. This is the
  established real-time approach: Lagarde 2012 "Water drop 2b" layered drop cells, and the "Heartfelt"
  layered drop cells and trails (Martijn Steinrucken 2017). The catch rate sets the density of live
  layers, as rain_ripples() does with impact rate.
- **Kaneda's affinity grid.** The meandering bed a runner keeps comes from a fixed per-surface noise of
  wettability that runners follow. Each runner's lateral wander is a function of that noise, not of history.
- **Glass.** Drops add a normal and a thickness into transmission, so each drop is a small lens refracting
  the scene behind (the path tracer refracts through the same normal), and a thin film darkens nothing.
  Hooks: `rain_wipe` (a mask texture or a wiper arc uniform) and `rain_slipstream` (a velocity in the
  surface's frame for vehicles, which pushes runners back and up).
- **Walls.** The same runners at a lower drop density become a film of streaks (wall_film's look). Where the
  catch is high, the porous darkening follows the streaks rather than an even film.
- **PT parity.** The functions live in one include used by the material and by the raygen, as
  rain_ripples() does.
- **Reference look.** The game's rain_on_glass.gd (the downpour streams were approved 28 Oct) and
  wall_film.gd. The engine version must match at the game's camera distance.

### B. Puddles on built surfaces (game item 4)

Standing water exists on terrain (the ground patch, GroundSurface3D, ruts) but not on meshes: pavements,
roofs and plazas.

The technique is Lagarde 2012 "Water drop 2a/2b": a puddle mask from the surface's height or AO and a noise
field, filled by accumulated rain and emptied by evaporation. WeatherMap3D already keeps both per cell
(cell_puddle and the film), so its puddle state drives the level, and up-facing faces fill where the mask
is lowest. Water shading and rain_ripples() reuse the ground patch's water look.

Draining to gutters is a second stage: a flow direction from the mesh's slope (or a painted flow map) and
WeatherRunoff3D's routing for built ground.

### C. Rain curtains in the haze (game item 7)

The precipitation haze is a uniform horizontal layer (weather_haze.cpp). The game's gust_haze.gd varies the
froxel density with a noise field carried by the wind (Taylor's frozen turbulence) and drawn out upright
into shafts, keeping the mean.

In the engine, the haze FogVolume's density would be modulated by the weather's own rain map (the storm
cells are already uneven) times an advected, vertically stretched noise. The path tracer reads the same
volume. The froxel technique is unchanged (Wronski 2014; Hillaire 2015).

### D. Camera occlusion cut-out (game item 2)

This is a screen-space dithered hole through geometry between the camera and the player, as in Diablo IV
and Baldur's Gate 3. Each opaque material discards pixels inside a soft circle around the player's screen
position when the pixel is nearer the camera than the player (a depth test against the player's view
depth). It uses an ordered dither so TAA or upscaling resolves it.

For the engine, put this in BaseMaterial3D behind a global (`occlusion_cutout_center`, radius,
depth). Under path tracing, primary rays skip hits in the cut cylinder, so both renderers show the same
hole. With the fixed isometric camera this is the usual approach in shipped games.

### E. Shadow budget by importance (game item 3)

Godot's positional shadow atlas assigns quadrants by screen size, and when it is full a light silently
loses its shadow. The established techniques are Unity HDRP's "max shadows on screen" with per-light update
modes (every frame, on enable, on demand) and UE's per-light shadow priority, which rank lights by
importance (intensity / distance squared x screen coverage) and keep shadows for the top N, updating the
far ones less often.

For the fork: a project setting for the maximum number of shadowed positional lights, an importance sort,
and staggered updates for the rest. This is raster only, since the path tracer traces every light.

### F. Texture streaming by VRAM budget (game item 9)

The game builds its own 2K and 1K copies (texture_lod_loader.gd, model_detail.gd). Mortal-plane will need
the same at open-world scale. The established techniques are UE's texture streaming pool
(r.Streaming.PoolSize: mips requested by screen-space texel density, dropped from the far end to fit) and
Unity's mipmap streaming with a memory budget. Godot has none (proposal godot-proposals #3177).

This is the largest item, an RD-level mip residency manager and an importer change, so it should get its
own thread.

### G. Failing-lamp flicker (game item 11)

The classic technique is Quake's light styles: a per-light pattern string, sampled at 10 Hz, mapping
letters to brightness. Source and id Tech keep it. The game's lamp_flicker.gd draws seeded random spells.

For the engine: a `flicker` resource on Light3D (pattern or seeded random spells) driving energy and an
emissive partner, the same in both renderers (the path tracer reads light energy each frame). It is small.

## Order proposed

1. A (glass and walls), then B and C, since they share the rain inputs.
2. D, the cut-out.
3. E, the shadow budget.
4. G whenever it is cheap.

F gets its own thread.

## Status (22 Nov)

- A: surface rain include, wall streaks with `rain_wipe` and `rain_slipstream`; glass shading with Valence.
- B: `weather_surface_puddle` on up-facing built faces from WeatherMap3D's puddle state. Gutters not yet.
- C: `WeatherMap3D.rain_curtains`.
- D: `camera_cutout` globals in BaseMaterial3D and the raygen. Module shaders still to adopt it.
- E: positional shadows by importance, `max_shadowed_lights` budget.
- G: `Light3D.flicker_mode` (pattern or failing, seeded), with `flicker_material` following.
- F: with the quality presets thread.

## Gaps found porting the game onto these (23 Nov)

The PC side-by-side of the game's port (its PR #104) showed where the engine's features stop short of what
the game needed. Each is listed with whether it looks generic enough to build; none is started.

### H. Shadow budget around a subject (generic: yes)

`max_shadowed_lights` ranks lights by energy times screen size. A game with a fixed camera and one player
wants the lights nearest the player kept, so the shadows where the action is never drop (the game ranks by
distance to the player, and the engine's ranking picked other lamps: a brighter, leakier street). Proposal:
an optional focus point (a viewport or camera property, or a global) whose distance weighs the importance,
with the same hysteresis. Third-person and isometric games, and any streamed open world with a hero, gain.

### I. Rain curtains lit by local lights (generic: yes)

`WeatherMap3D.rain_curtains` darkens the air but local lights do not scatter in it, so a work light or a
street lamp has no glow in the rain; the game's fog-volume haze does. Proposal: feed the curtains into the
volumetric fog froxels (or a FogVolume) so they take the clustered lights' in-scattering in raster, as the
path tracer's medium already does. Any night rain scene gains.

### J. Externally driven cloth and the weather (generic: yes, small)

PhysXCloth3D adds the weather map's wind and wetting on top of a script that already sets its `wind`, so a
game with its own wind field gets the wind twice. `weather_wind` and `weather_wetting` exist but default on.
Proposal: when a script sets `wind` (or a `wind_source` enum: weather, manual, both), the weather's stops
adding unless asked; document the contract in world-links. Every project that ports its own wind gains.

### M. Moving gust fronts on the weather map (generic: yes)

The game's wind (StormWind, read by 14 of its scripts: rain, props, tape, coats, haze) has authored local
events the map cannot hold: a squall's gust front sweeping the roof after a near strike, and a train's
slipstream running along the track. The map has mean wind, Kaimal gusts, gust patches, downburst outflow
(add_storm) and vortices, but no moving front. Proposal: `WeatherMap3D.add_gust_front(origin, direction,
peak, half_width, reach, speed, rise, decay)`, read by get_wind_at and the wind texture, so a game's wind
can be the map's and every reader (cloth, trees, rain, snow, dust) gets one wind. Squall lines, vehicle
slipstreams and blast waves all fit it.

### K. Camera cut-out opt-in by default for a project (generic: yes, small)

Every BaseMaterial3D takes part in the cut-out, so a game must reach every material built before its own
hook to keep it out (the roof's water tank vanished). Proposal: a project setting for the default
(`rendering/camera_cutout/default_enabled`), and a per-GeometryInstance3D `camera_cutout` override, so a
game opts in what may open (buildings, roofs) instead of opting everything else out.

### L. Puddles in real road relief (generic: yes; in progress)

The game's street water simulation (iWave) stands in for puddles that sit in actual dips. Owned by the
PhysX thread: a road-relief node (ruts, crown, camber, a world-anchored depression field, potholes) feeding
a street-puddle water mode.
