# Water bodies: rivers, waterfalls, lakes and ocean

Research note, 7 Nov 2026. What shipped engines do for open-world water, and
what the fork could take. Sources are public talks, docs and press; Crimson
Desert has no technical talk, so its entries are press-level.

## What others do

### Crimson Desert (Pearl Abyss, BlackSpace)

- FFT ocean plus a shallow water simulation. The simulation runs water over
  terrain: rivers around rocks and sea waves at the shore (80.lv, GamesRadar).
- Water reacts to characters and objects. Ice arrows freeze river water into
  a chunk that drifts downstream and breaks on rocks. Only the parts of a body
  or horse that touch the water get wet (GamesRadar hands-on).
- Rendering: per-pixel RT diffuse GI and hybrid SSR plus RT reflections
  (Digital Foundry via VGTimes). Nothing public on refraction, caustics or
  foam.

### Unreal Engine Water plugin

- Water bodies are splines:
  - Ocean: a closed loop.
  - Lake: a closed loop at one height.
  - River: an open spline with per-point width, depth and velocity.
- River velocity is written into a flow map. Rivers have no waves.
- Waves are Gerstner, 16 by default, set by wind angle, spread and steepness.
  They fade in shallow water.
- Terrain is carved under each body from a depth curve.
- A tiled water mesh covers the bodies; a far mesh covers the horizon.

### Fluid Flux (Unreal add-on)

A 2D shallow water simulation that handles slopes and waterfalls, with
caustics on slopes and the surface drawn as 128x128 mesh tiles. Published
detail on how it is baked or run at runtime is thin.

### Horizon Forbidden West (Guerrilla, SIGGRAPH 2022)

- No runtime fluid sim. Houdini simulations of small areas are baked, then
  instanced and blended at runtime.
- Breaking waves are artist-laid wavefronts (curves baked to a grid, streamed
  with world tiles). A compute shader builds the wave mesh from one baked
  cross-section per frame.
- Rivers: standing waves, eddies and ripples are baked 3D deformation volumes
  placed procedurally by Houdini. These also form the waterfall impacts.
- Foam is per vertex, so mesh density limits it.
- Artist control was preferred over pulling shapes from a simulation.

### FFT ocean in Godot

GodotOceanWaves (MIT, 2Retr0) is an inverse FFT ocean in Godot compute:
- JONSWAP/TMA spectrum, Stockham FFT, several cascades to hide tiling.
- Foam where the surface folds (negative Jacobian), spray as GPU particles.
- It already runs on RenderingDevice, so it is a reference that fits the fork
  directly.

### iWave (Tessendorf)

Interactive ripples from a convolution kernel on a height grid, with
obstacles as a mask. Good for local interaction, not for open ocean. The
current game already uses it for puddles.

## What this means for the fork

The split agreed in the PhysX discussion still holds:
- iWave for puddles and local ripples.
- PhysX fluids only for hero splashes that read the iWave height.
- An FFT ocean for the open world only.

Rivers and waterfalls fit into that split as below.

| Item | Method | Game |
|---|---|---|
| Rivers | Spline water bodies (width, depth, velocity) baked to a flow map. The surface scrolls normals and foam along the flow (two-phase flow mapping). A shallow water sim (Crimson Desert, Fluid Flux) is the upgrade for water that must react: run it in a window around the camera, baked flow outside. | Open world |
| Waterfalls | Horizon's approach: a mesh sheet with flow-scrolled material, baked deformation at the impact, spray particles, and mist from the same lit sprite and traced haze system the current game uses for steam. | Open world |
| Lakes | Flat body plus iWave around the player and Gerstner or small FFT waves. | Open world |
| Ocean | FFT cascades (GodotOceanWaves as the reference), fading in shallow water. | Open world |
| Puddles, gutters, wet streets | iWave as today. | Current game |
| Interaction | iWave in a window around the player for wakes and ripples. PhysX hero splashes read its height. | Both |
| Path traced water | Refraction already comes from the traced transparency in NVIDIA's code. Missing: Beer-Lambert absorption and scattering inside the water (the "water medium" item already on the Valence list), so deep water darkens and tints by depth. | Both |
| Caustics | A plain path tracer can't find caustics from small lamps through water. Built for the sun as a caustics map traced from the light (step 2 below). | Open world |
| Streaming | Flow maps, baked deformations and water tiles stream with world cells, like Horizon's wavefront grid. | Open world |

## Suggested order (open-world game, after the current engine list)

1. Water medium in the path tracer: absorption and scattering by depth.
2. Spline rivers and lakes with flow maps, streamed with the world.
3. FFT ocean cascades.
4. Waterfalls: sheet, impact, spray and mist.
5. Shallow water sim near the camera, where the game needs water to react.
6. Caustics.

## Step 1 design: refraction and a water medium in the path tracer (9 Nov)

### What the path tracer does today

- Water drawn with alpha blending is peeled straight through: no bending, no Fresnel split
  between reflection and refraction, no darkening or tint with depth.
- Opaque water shaders (the game's puddles and drains) reflect only.
- Transmission (#153, #155) is thin-walled: light passes straight on, with no medium behind.
- `brdf_inc.glsl` has GGX reflection only. There is no BTDF, no IOR and no notion of being inside
  a medium. The smoke volume (`raytracing_volume_inc.glsl`) already does delta tracking with a
  Henyey-Greenstein phase, so scattering inside water can reuse it later.

### How others do it

- **glTF.** `KHR_materials_transmission` with `KHR_materials_volume` (`thicknessFactor`,
  `attenuationColor`, `attenuationDistance`) and `KHR_materials_ior` describe a solid dielectric.
  Light inside is absorbed by Beer-Lambert, `T = attenuationColor ^ (d / attenuationDistance)`,
  so `sigma_a = -ln(attenuationColor) / attenuationDistance`.
- **Unreal Single Layer Water.** It takes absorption and scattering coefficients per metre and a
  phase G, which is the physical description of water. Raster only, but it is the parameter set
  artists already know.
- **Falcor and RTXPT.** StandardBSDF with specular transmission, IOR and volume absorption. Nested
  media are tracked with an interior list and nested priorities (Schmidt and Budge 2002), so a ray
  knows which medium it is in when it hits the next interface.
- **Offline renderers** (Arnold, Cycles, RenderMan): a rough dielectric BTDF (Walter et al. 2007)
  picks reflection or refraction by Fresnel and handles total internal reflection. Shadow rays
  through glass and water skip the refraction offset (Arnold's transmit shadows), since a path
  tracer cannot find lights through a bending interface by itself.
- **Pure water** (Pope and Fry 1997) absorbs about 0.35 per metre in red, 0.06 in green and 0.01
  in blue. That is why clear water turns blue-green with depth. Lakes, rivers and the sea add
  dissolved matter and sediment on top.

### Plan

1. **Material.** The Transmission section gains `thin_walled` (on by default, so nothing
   changes), `ior` (1.33 for water), `attenuation_color` and `attenuation_distance`, matching
   glTF. Custom shaders get `IOR` and `ATTENUATION` (absorption per metre), so the game's own
   water shaders can use them. Raster keeps Godot's screen-space Refraction, with the offset taken
   from the IOR.
2. **BSDF.** A rough dielectric lobe (Walter et al. 2007): a GGX VNDF half vector, then
   reflection or refraction picked by the exact dielectric Fresnel for the IOR, with total
   internal reflection. It replaces the thin lobe when `thin_walled` is off.
3. **Medium tracking.** The path payload carries the current medium: absorption in fp16 RGB and
   the IOR. Refracting into a front face enters it, and leaving through a back face exits. One
   medium at a time in v1. Nested priorities and cameras under water come later.
4. **Absorption.** Every segment inside the medium is multiplied by `exp(-sigma_a * t)`. That
   covers camera, bounce and shadow rays. A shadow ray from a point under water passes the
   surface straight, weighted by `(1 - F)` and the absorption to its hit distance. This is the
   transmit-shadow approximation: sunlight reaches the riverbed tinted, without caustics.
5. **Scattering** (later step): the existing smoke delta tracking with a homogeneous medium,
   for murky water and light shafts under the surface.
6. **Ray Reconstruction.** The refracted path's hit distance and albedo go in the specular guides,
   as for glass. This needs the PC check.
7. **Setting.** `rendering/pathtracing/refraction` (on). Off traces these surfaces as today.
8. **Test scene.** `docs/bench/materials --scene=water`: a pool sloping from 0 to 4 m with a
   checker floor and coloured posts, sun and sky. Depth tint is checked against Beer-Lambert at
   known depths, and raster is shown next to it.

Caustics stay out of step 1. Without them, the bed under bright sun is evenly lit rather than
patterned. Options after step 1: a projected caustic texture from the surface normals, or the
ray traced caustics of Ray Tracing Gems II ch. 30.

## Step 1 as built: refraction and depth tint (9 Nov)

- **Material.** Transmission has `transmission_thin_walled` (on by default), `transmission_ior`,
  `transmission_attenuation_color` and `transmission_attenuation_distance`. glTF
  `KHR_materials_volume` (a thickness above 0 turns thin-walled off) and `KHR_materials_ior` are
  imported and exported; an imported IOR also sets `specular` so F0 matches it.
- **Record.** The IOR takes the spare byte of `emission_indirect_transmission` (1 to 3 in steps of
  2/255). The absorption goes in the last word, renamed `medium`: a 9-bit log code per channel
  (2^-12 to 2^12 per metre, about 3% steps), with bit 29 set so the word is never 0 and is a normal
  float. New flag `RT_MAT_FLAG_VOLUME`. Such surfaces are traced with culling off, since rays leave
  the water through its back faces.
- **Lobe.** `sampleDielectricRefraction()` in `brdf_inc.glsl`: a GGX VNDF half vector, `refract()`
  with eta 1/IOR on front faces and IOR on back faces, weight (1 - F) G2/G1 with the exact dielectric
  Fresnel. The base lobes keep the surface's F0 reflection, as for thin glass. At total internal
  reflection the lobe reflects the rest, (1 - F0 Fresnel) G2/G1, so looking up from below past the
  critical angle is a mirror. The eta^2 radiance scaling is left out; it cancels on the way out.
- **Medium tracking.** A refracted continuation sets `MEDIUM_CHANGE_FLAG` (payload bit 17) and puts
  the new medium code in `bsdf_pdf` (0 when leaving). Raygen keeps the code per sample, multiplies
  each segment inside by `exp(-sigma t)` (the surface it reached and all it sends back), then
  switches medium. One medium at a time; the camera starts outside.
- **Shadows.** A shadow ray that leaves a medium through a back face is multiplied by
  `exp(-sigma t)` to that face, in the any-hit shader and in ray-query shadows (fog), on top of the
  surface's (1 - F0) tint. The bend is ignored (transmit shadows), so the floor is evenly lit.
- **Raster.** The screen is read where the view ray refracted by the IOR meets the depth buffer,
  falling back to straight behind when that point is in front of the surface, and tinted by
  `color^(d / distance)` with d the distance to that point.
- **Setting.** `rendering/pathtracing/refraction` (on). Off traces these surfaces as thin glass.
- **Test scenes.** `docs/bench/materials --scene=water` (pool sloping 0.1 to 4 m, checker floor,
  posts) and `--scene=water_depths` (unshaded white tiles under 0.25 to 4 m, orthogonal view, linear
  tone mapping) to read Beer-Lambert off the pixels.
- **Not yet.** Shader outputs for IOR and absorption (custom water shaders stay thin), scattering,
  caustics, nested media and a camera under water. Shadow rays inside the water that never cross
  its surface (a lamp under water) are not absorbed.

## Step 1b as built: scattering and shader water (9 Nov)

- **Parameters.** glTF's draft `KHR_materials_scatter` (open PR #2579, volumetric mode):
  `transmission_scatter_strength`, `transmission_scatter_color` (the multi-scatter albedo, the
  colour the medium looks after many bounces) and `transmission_scatter_anisotropy` (Henyey-Greenstein
  g). The attenuation colour and distance now give the extinction; the single-scatter albedo from
  Kulla and Conty's fit, `s = 4.09712 + 4.20863 c - sqrt(9.59217 + 41.6808 c + 17.7126 c^2)`,
  `albedo = (1 - s^2) / (1 - g s^2)`, splits it into scattering and absorption. Computed on the CPU
  and stored as unorm8 albedo plus g in a new word; `RT_MaterialData` grows to 160 bytes.
- **Medium code.** A path entering a base material medium carries `RT_MEDIUM_CODE_GEOMETRY` (bit 30)
  plus the TLAS entry, and raygen reads the medium from that entry's material (raygen now binds the
  geometry and material buffers). Shader media carry the inline extinction code.
- **Tracking.** Spectral one-sample MIS free flight (Wilkie et al. 2014; Miller, Georgiev and Jarosz
  2019): the collision distance is drawn on a random channel and weighed by the mean of the three
  channel densities. A collision before the surface replaces it with a scattering event: every
  light through `rt_volume_direct()` (shared with the smoke, now taking g) and a Henyey-Greenstein
  continuation. No collision: transmittance over the mean no-collision probability. Clear water
  (no scattering) keeps exact Beer-Lambert; the depth tiles still match within 1%.
- **Raster.** Kulla and Conty's idea in reverse: a dense medium looks like a diffuse surface of its
  multi-scatter colour, so the surface gets that albedo in the share `1 - T` the depth hides.
- **Shader outputs.** `IOR` (1 or more turns the surface into a medium boundary) and `ABSORPTION`
  (per metre) with `TRANSMISSION`; path traced only, no scattering. Such shaders are traced from both
  sides and their shadows are absorbed leaving the medium.
- **Test scene.** `--scene=water --water=murky|shader`.
- **Limits.** Multiple scattering ends at the bounce limit, so dense media come out darker than
  they would with no limit. Sky light reaches a scattering point only through further bounces, not as
  a light sample. Murky water is noisy at low sample counts.

## Step 2: caustics from the sun (9 Nov)

### Why a plain path tracer misses them

Under water, a point's sun sample is a shadow ray straight up through the surface. The real light
reached it along a bent path through a ripple, which a straight ray never finds (the sun is a
point-like source seen through a specular surface: a specular-diffuse-specular path). So the floor
is evenly lit, tinted by depth.

### Techniques looked at

- **Photon mapping** (Jensen 2001): trace photons from the light, gather them at the visible
  points. Every real-time caustics method below is a version of it.
- **Ray-guided water caustics** (Gruen, Ray Tracing Gems 2019; Yang and Ouyang, Ray Tracing Gems II
  ch. 30, 2021, NVIDIA's UE4 RTX branch, shipped in JX3): draw the water from the light into a
  caustics map, trace one refracted ray per texel, and turn the landing points into light: splatted
  footprints sized by the spread of neighbouring rays ("photon difference scattering"), or a mesh of
  the landing points ("procedural caustic mesh"); cascades like shadow maps for oceans. Lights the
  screen in a deferred pass, so it is not seen in reflections, and only one bend counts.
- **Adaptive anisotropic photon scattering** (same chapter): photons with ray differentials through
  several bounces for glass and metal objects, splatted on screen. For glass objects, later.
- **Manifold next event estimation** (Hanika, Droske and Fascione 2015; Blender Cycles "shadow
  caustics" since 3.2): at the floor point, Newton-solve for the point on the water surface that
  bends the sun towards it. Per pixel and exact, but it needs smooth geometric normals and their
  derivatives. Game water gets its ripples from normal maps and shaders (iWave, FFT normals), which
  it cannot use.

### What was built

The ray-guided caustics map, but feeding the path tracer's sun light instead of the screen:

1. **Photons.** Raygen shoots one photon a pixel from the first directional light across a square of
   `rendering/pathtracing/caustics_area` metres (default 16), centred on the surface the middle of the
   screen showed last frame (at least half the width ahead of the camera, so it works for a close
   third-person view and a far isometric one alike), jittered every frame. A photon hitting the front of a medium's surface (Transmission not thin-walled, or a shader
   writing `IOR`) is bent there by the surface's own shading normal, so normal maps and shader
   ripples count. The closest-hit shader reports the surface instead of shading it
   (`CAUSTIC_PHOTON_FLAG`).
2. **A straight twin.** From the same entry point a second photon goes on unbent. Both land on the
   first surface past the medium and add to the light-space texel above their landing point
   (512 x 512, bilinear). The straight twins also add their depth.
3. **The ratio.** A texel's bent count over its straight count is how much the surface focuses the sun
   there: 1 under flat water, above 1 in the bright lines, below 1 between them. The twins replace the
   footprint sizing of the published methods: the same jittered photons give both counts, so no
   neighbour lookups or areas are needed, and they cancel where a texel's photons were blocked.
4. **Use.** The sun's shadow ray already crosses the water straight, with Fresnel and Beer-Lambert
   absorption (step 1). `lights_evaluate_directional()` multiplies the sunlight by the ratio at the
   shaded point, at every bounce, so caustics also show in reflections and light the pool by bounce.
   A point whose texel saw no straight twin, or whose depth is more than 0.3 m off theirs, keeps the
   plain sun.
5. **No extra pass.** The map has three slots: a frame adds to one, reads the one the last frame
   wrote, and each pixel's thread clears a share of the one it read last frame. One frame of delay.
6. **Radiance cache.** A rough surface the first bounce reaches normally reads the radiance cache
   and stops. Under water that averaged the lines away within a few frames, since a 0.25 m cell holds
   a whole ripple. Points the map covers shade on instead.
7. **Setting.** `rendering/pathtracing/caustics` (on). Off, or with refraction off, is the old even
   light.

### Limits

- One directional light. Lamps under or over water cast none (a point-light version would need a
  cube map or MNEE).
- One bend: light reflected off the surface onto walls, and light bent twice, is missing.
- The rasterizer draws no caustics.
- Sharpness against reach: 512 texels over 16 m is 3 cm a texel. At 32 m the lines of small ripples
  blur into soft bands; at 8 m they are sharp. About 8 photons a texel at 1080p; the denoiser and
  accumulation smooth the rest.
- Caustics stop at the square's edge (cascades, as the chapter does, would extend it).

## Sources

- 80.lv, Crimson Desert's BlackSpace engine features:
  https://80.lv/articles/pearl-abyss-demonstrated-crimson-desert-s-blackspace-engine-tech-advancements
- GamesRadar (via Yahoo), Crimson Desert engine hands-on:
  https://tech.yahoo.com/gaming/articles/saw-dirty-little-secrets-crimson-140000043.html
- VGTimes on Digital Foundry's Crimson Desert analysis:
  https://vgtimes.com/gaming-news/149317-digital-foundry-dazzled-by-crimson-deserts-blackspace-engine-tech.html
- Epic, Water system and water body actors:
  https://dev.epicgames.com/documentation/en-us/unreal-engine/water-system-in-unreal-engine
- 80.lv, Fluid Flux waterfall:
  https://80.lv/articles/real-time-waterfall-simulation-made-with-the-fluid-flux-add-on/
- Malan, Water in Horizon Forbidden West, SIGGRAPH Advances 2022:
  https://advances.realtimerendering.com/s2022/SIGGRAPH2022-Advances-Water-Malan.pdf
- 2Retr0, GodotOceanWaves: https://github.com/2Retr0/GodotOceanWaves
- Tessendorf, Interactive Water Surfaces (iWave):
  https://jtessen.people.clemson.edu/reports/papers_files/Interactive_Water_Surfaces.pdf
- Khronos, KHR_materials_volume:
  https://github.com/KhronosGroup/glTF/blob/main/extensions/2.0/Khronos/KHR_materials_volume/README.md
- Epic, Single Layer Water shading model:
  https://dev.epicgames.com/documentation/en-us/unreal-engine/single-layer-water-shading-model-in-unreal-engine
- Schmidt and Budge, Simple Nested Dielectrics in Ray Traced Images, 2002:
  https://escholarship.org/content/qt9h83s3m0/qt9h83s3m0_noSplash_693fb7df0433335e068723e435d6a371.pdf
- Falcor path tracer (nested dielectrics, StandardBSDF):
  https://github.com/NVIDIAGameWorks/Falcor/blob/master/docs/usage/path-tracer.md
- Arnold, transmission and nested dielectrics:
  https://help.autodesk.com/cloudhelp/ENU/AR-Core/files/ac-shading/ac-surface-shaders/ac-standard-surface/ac-transmission/arnold_user_guide_ac_transmission_ac_nested_dielectrics_html.html
- Walter, Marschner, Li, Torrance, Microfacet Models for Refraction through Rough Surfaces, 2007.
- Pope and Fry, Absorption spectrum of pure water, Applied Optics, 1997.
- Yang and Ouyang, Real-Time Ray Traced Caustics, Ray Tracing Gems II ch. 30, 2021:
  https://developer.download.nvidia.com/ray-tracing-gems/rtg2-chapter30-preprint.pdf
- Gruen, Ray-Guided Volumetric Water Caustics in Single Scattering Media with DXR, Ray Tracing Gems
  ch. 14, 2019.
- Hanika, Droske and Fascione, Manifold Next Event Estimation, EGSR 2015.
- Blender manual, Cycles shadow caustics (MNEE): https://docs.blender.org/manual/en/latest/render/cycles/object_settings/object_data.html#caustics
- Jensen, Realistic Image Synthesis Using Photon Mapping, 2001.
