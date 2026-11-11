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
| Ocean | FFT cascades, fading in shallow water. One FFT domain already in the PhysX module as `PhysXWaterSurface3D` (step 3 research); more cascades to hide tiling are still to add. | Open world |
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

### Caustic cascades (10 Nov)

The caustics map now uses cascades, as shadow maps do, and as Yang and Ouyang (Ray Tracing Gems II
ch. 30) extend it to oceans. The setting is `rendering/pathtracing/caustics_cascades`, default 3.

- **Coverage.** There are three squares around the same centre: `caustics_area`, then 4 and 16
  times it. With the default area that is 16, 64 and 256 m.
- **Cascade 0 is unchanged.** It is 512 texels and takes one photon from every pixel, so the near
  lines are exactly as sharp and as noisy as before.
- **The outer two cascades** are 256 texels each (25 cm and 1 m texels). Each takes one extra photon
  from a quarter of the pixels; which quarter rotates every frame on a 2 x 2 pattern. That keeps
  about eight photons a texel in every cascade.
  - Cost: half again the photon rays, which is a small share of a frame. The GPU timing goes on the
    PC queue.
  - Memory: 9 MiB becomes 13.5 MiB.
- **Lookup.** A point reads the finest cascade that covers it. Across a cascade's outer 8% it blends
  into the next one out instead of into the plain sun. The outermost still fades to the plain sun.
  - A texel with no straight photon, or with photons landing at another depth, falls through to the
    coarser cascade.
  - The coarse texels cannot hold small ripples, so far out the caustics are the swell's broad
    bands. That matches what the eye resolves at that distance.
- **Packing.** The cascade count rides in the existing caustics state parameter (bits 3 and 4), so
  no new `rt_params` slot is used.
- **Check.** On the cloud bench's default view (lavapipe, PT), 1 cascade against 3 differ only past
  the 16 m square, where the swell's bands now show on the sand. Inside it the image differs only
  by noise.

## Step 3 research: flow, rivers and buoyancy (9 Nov)

### What the fork already has (PhysX module, #113)

Missed in the first pass of this note: `modules/godot_physx/water/` already holds most of an
ocean (one FFT domain, no extra cascades yet), so the ocean is not a new build.

- `PhysXWaterSurface3D`: Tessendorf FFT ocean (fetch-limited JONSWAP, choppiness, whitecap foam
  that stays with the water), plus a local ripple layer (a damped shallow-water wave equation that
  bodies disturb), shore swash, wet sand, `recenter()` for an endless sea, and a raster caustics map.
  `sample_height()` gives the same surface on the CPU.
- `PhysXBuoyancy3D`, `PhysXBoat3D`, `PhysXWaterWake3D`, `PhysXWaterSpray3D`: probe buoyancy
  (density x g x area x depth per sample point), drag, outboard thrust, a sliding wake grid, spray.

Gaps:
1. **No flow anywhere.** No river, no current: the surface, foam and buoyancy drag all assume still
   water.
2. **The path tracer sees it as alpha glass.** The surface material is `unshaded` with `blend_mix`
   alpha, so path tracing peels it and gets none of steps 1 to 2 (bending, depth tint, murk,
   caustics). Its vertex displacement already reaches the path tracer (vertex capture).
3. **No river geometry.** Nothing builds a river mesh from a path.

### How engines and games do flow

| Who | How flow is defined | Storage | Surface | Notes |
|---|---|---|---|---|
| Valve, Left 4 Dead 2 / Portal 2 (Vlachos 2010, Grimes 2011) | Houdini: field from plane normals, attractors for upwellings, curl for vortices; painted direction masks | 256² DXT5, RG = flow (length = speed), A = colour mask; about 4 texels per metre | Normal map scrolled along flow in two layers half a phase apart, noise offsets the phase to hide the pulse; normal strength scaled down with speed; debris uses offset interval so it is undistorted at the peak; foam lookup distorted again by the flowed normal | The basis of everything below. 2 extra fetches, 21 ALU |
| Unreal Water plugin | River spline, per point `RiverWidth`, `Depth`, `WaterVelocityScalar` | All water bodies rendered into one world "water info texture" (velocity packed as magnitude + angle, `r.Water.MaxFlowVelocity` 1024) | Flow-mapped normals and foam | One texture for rivers, lakes and ocean is how bodies meet |
| Unity HDRP water | Painted current map | RG direction, B influence | Current translates the FFT swell/agitation bands; foam follows with "current influence" | Current moves the simulated waves, not just a scrolled normal |
| Crest (Unity) | Flow inputs: area, spline (per-point speed), paint, texture (RG velocity) | Flow is a simulation layer in its camera-centred LOD textures | Flow transports waves, foam, normals and caustics; streams can bake a shallow-water sim to a texture | Closest to what the fork needs |
| Waterways (Godot addon, Arnklit) | Bezier river mesh; bake raycasts the river for obstacles, dilates to a distance field, takes its gradient turned 90 degrees as flow (round obstacles), "pressure" = blocked share across the width | RG flow + B foam, R distance + G pressure, in the river's UV2 | Catlike two-phase with jump offsets and two scales (2x octave near the camera); speed = base + steepness + distance-from-bank + pressure | Godot 3 only (forks: TokisanGames, .NET port for 4.2) |
| Horizon Zero Dawn / Forbidden West | Houdini river tool: spline rolled down terrain, never uphill; width/depth ramps; Forbidden West bakes local sims to deformation flipbooks | Vertex colour flow and foam | Flowed normals, baked deformations for standing waves and impacts | Artist control preferred over runtime sim |
| Sea of Thieves (SIGGRAPH 2018) | FFT ocean; shallow water GPU sim (Mei 2007) for streams and waterfalls | Sim textures | Foam from FFT choppiness and depth intersection, blurred with feedback | |
| Proland (Yu, Neyret, Bruneton, Holzschuch 2009) | Velocity from a stream function stored on the banks; interpolated by distance to nearby banks | Procedural | Wave sprites carried by particles, faded in and out (no pulsing, no contrast loss) | Stateful particles; best quality, costs a particle system |
| BeamNG, CryEngine, Unigine | Spline rivers with a scalar speed (BeamNG); flowmap painting tools (Unigine) | | | Nothing new |

### Techniques for the flowing surface

- **Two-phase flow maps** (Vlachos; formulas from Flick's Catlike Coding article): per phase
  `progress = fract(t + phase)`, `uv = uv0 - flow * (progress + offset)`, weight `1 - |1 - 2 progress|`,
  phase B shifted half a cycle and half a tile; noise added to `t` hides the pulse; a "jump"
  `(t - progress) * jump` (about 0.24, 0.21) shifts the texture each cycle so it does not repeat.
  Stateless, cheap, works in any shader, including a ray-tracing hit shader.
- **Its weaknesses and fixes:**
  - Contrast loss at the crossover (two half-weight copies): Mikkelsen's hex-tiling (JCGT 2022)
    sharpens blend weights with an exponent and renormalises; for normals, blend in surface-gradient
    form (sum the height derivatives, not the unit normals), as Flick's derivative maps do. Variance
    restoration (Burley 2019, Heitz and Neyret 2018) assumes independent samples, which two phases of
    one texture are not, so it would over-correct.
  - Stretching: the cycle bounds it; speed scales distortion, not time.
  - One scale looks fake: two octaves at different tilings and speeds (Waterways).
- **Lagrangian advection** (Yu et al. 2009/2011; Bousseau et al. 2007 bidirectional): textures or
  sprites carried by particles, faded in and out. Best spectrum, but needs per-frame state, so it does
  not run in a hit shader. Not now.
- **Water surface wavelets** (Jeschke et al. 2018): wave amplitudes advected on a grid, reflections
  and diffraction round obstacles. Strong for interactive open water, heavy (a 4096² amplitude grid);
  research-grade, not shipped. Not now.

### Generating the flow

- From a **river path**: direction = path tangent, speed from per-point velocity, width and depth
  (Unreal, BeamNG, Waterways, Horizon).
- Round **obstacles**: Waterways' distance-field heuristic works but is not mass-conserving. Better:
  solve a **stream function** across the river (Laplace on a grid, each bank held at a constant, the
  difference = discharge, rocks held at their bank's value): depth-averaged velocity
  `u = (dpsi/dy, -dpsi/dx) / depth` goes round rocks and speeds up through narrows. This is the bank
  stream function Proland interpolates, solved properly once at bake time. A few hundred Jacobi or
  multigrid sweeps on the GPU, done in the editor.
- From a **simulation**: shallow-water bake (Crest streams, Fluid Flux, Sea of Thieves). The ripple
  layer here is a wave equation without advection, so it cannot give currents; a full SWE solver is
  the later upgrade for water that must react.

### What PhysX and NVIDIA's own research bring

The PhysX team published the method for exactly this kind of water, built for games on the GPU:

- **Chentanez and Müller 2010**, "Real-time Simulation of Large Bodies of Water with Small Scale
  Details" (SCA): a shallow-water heightfield solved on the GPU (semi-Lagrangian advection of
  velocity, height update from the divergence, stabilised against overshoot at steep fronts), with
  particles spawned where the surface breaks or overturns for spray and splashes, and two-way
  coupling with rigid bodies (bodies displace the water and feel its drag and buoyancy). Müller's
  GDC 2008 talk "Fast Water Simulation for Games Using Height Fields" is the simpler wave-equation
  form our ripple layer already is.
- **Chentanez, Müller and Kim 2014**, "Coupling 3D Eulerian, Heightfield and Particle Methods"
  (SCA): the same heightfield where water is shallow and calm, a 3D grid only where it gets violent
  (waterfalls, rapids), particles for spray. The heightfield part is what fits an open world.
- **Solenthaler, Bucher, Chentanez, Müller and Gross 2011**, "SPH Based Shallow Water Simulation"
  (VRIPHYS): the particle version, for water that runs over arbitrary terrain.

What the module already has that this builds on: the ripple layer is a heightfield on a sliding grid
around the player (Müller 2008's wave equation), bodies already push it (`submit_sphere()`), and
`PhysXBuoyancy3D` already reads its height. Turning that grid into Chentanez and Müller's
shallow-water solve adds advection, so it carries a current, and wakes and eddies form behind rocks
and boats; outside the grid the baked stream-function flow takes over, the same way the FFT ocean
takes over past the ripple square today. `PhysXParticleFluid3D` (PhysX 5 PBD particles) or
`PhysXWaterSpray3D` gives the spray where the solve marks breaking water, and NVIDIA Flow
(`PhysXFlow3D`) the mist over rapids and falls. Unreal's Fluid Flux plugin runs the same kind of
shallow-water solve over the whole river, which is the heavy end.

### Buoyancy

- Probe buoyancy, as `PhysXBuoyancy3D` and every engine read (Crest, godot4-oceanfft, Habrador)
  do: per probe `F = rho g area depth`, applied at the probe, so it rights itself. Triangle clipping at
  the waterline (Kerner, Just Cause 3; Arc Blanc 2025) is for hero boats only (about 1 ms per hull).
- **What is missing is the current.** Drag must use the velocity relative to the water,
  `v_rel = v_body - v_flow` (Kerner; Arc Blanc: `F_d = -1/2 Cd rho A |v_rel| v_rel`), so debris
  drifts downstream, spins in eddies and piles up at rocks.
- Water height and flow reach physics from a CPU copy: `sample_height()` already does this for the
  ocean; the flow bake is static, so a CPU copy of it is free.

### Plan

One world-space water info layer, as Unreal and Crest do, so rivers, lakes and the sea meet in one
place and every consumer (surface shader, path tracer, buoyancy, foam) reads the same flow:

1. **Path-traced water surface.** `PhysXWaterSurface3D`'s material writes `TRANSMISSION`, `IOR` and
   `ABSORPTION` under path tracing, so the ocean and lakes get bending, depth tint, murk and
   caustics from steps 1 to 2. Also what the engine-rendered logo needs.
2. **Flow.** A `flow` input on `PhysXWaterSurface3D`: a flow texture (RG velocity in m/s over the
   surface's extent) or constant current. The surface shader runs two-phase flow with noise, jumps,
   two octaves and surface-gradient blending; foam is carried by the same flow. The FFT ocean's
   waves are carried along too (HDRP, Crest).
3. **Rivers.** A `PhysXRiver3D` (Path3D with per-point width, depth, speed) builds its mesh and bakes
   its flow with the stream-function solve above (rocks from collision), and joins a lake or the sea
   by writing into the same water info layer.
4. **Buoyancy in the current.** `PhysXBuoyancy3D` drags against `v_body - v_flow`.

## Step 3a as built: path-traced water surface (9 Nov)

`PhysXWaterSurface3D`'s shader now has a path-traced copy (`#if defined(RT)`, compiled only for
the path tracer). It drops `unshaded` and the blend, and writes `TRANSMISSION` (1 minus foam),
`IOR` 1.33, `ABSORPTION` (`pathtraced_absorption`, default 0.45/0.09/0.05 per meter) and the
per-pixel wave normal, with `ALPHA` scissor for the swash edge. Both faces bound the medium, so
the path tracer enters the water at the top and leaves at the underside, and steps 1 to 2 apply
as they are: bending, total internal reflection, Beer-Lambert depth tint, and sun caustics on the
sand. Foam stays an opaque rough white layer. The raster shader is unchanged.

Two engine fixes it needed:

1. **Render modes of the path-traced copy.** The path tracer took `unshaded` and `fog_disabled`
   from the raster shader, so the water came out flat white. They now come from the shader's own
   `#if defined(RT)` variant (`RTClassification`), like its outputs already did.
2. **Varyings at a hit.** For captured (vertex-displaced) geometry the path tracer re-runs
   `vertex()` at the hit point to get the varyings, but that point is already moved. The water's
   `vertex()` added its mesh offset and read its rest position there, so every shore lookup
   (depth, swash, shore foam) landed several meters off and foam covered the shallows. The
   path-traced copy of `vertex()` now inverts the displacement instead: four fixed-point steps of
   `rest = hit - offset(rest)`, which converge while the waves don't fold (the usual way to look up a
   choppy FFT surface by world position). Any shader that both displaces and reads its pre-displacement
   position in a varying meets the same thing; storing the rest position at capture time would fix
   it for all of them (issue #169). Done: the capture now keeps the rest pose and hits re-run
   `vertex()` on it, so the water's inversion is gone ([displaced-hit-varyings.md](displaced-hit-varyings.md)).

Bench: `docs/bench/water_surface` (sloping sand, rocks, three floating boxes; `--view=beach|under`,
`--raster`). At 100% crop the sand checker shows through the path-traced water, bent by the waves
and crossed by caustics; with caustics off it matches the raster pattern. Deep water reads dark
teal rather than blue, since a shader can't yet give the water a scattering colour (the
Transmission section's scatter is material-only); a shader `SCATTER` output is the follow-up.

## Step 3a follow-up: scattering from shaders, caustics edge (9 Nov)

1. **SCATTERING and SCATTERING_ANISOTROPY shader outputs.** A shader medium only absorbed, so deep
   water went black under path tracing: its blue body colour is light scattered back up. Unreal's
   Single Layer Water material takes the same three inputs (absorption, scattering coefficients,
   phase g). The medium code a hit hands raygen carries the extinction (absorption plus scattering);
   the single-scatter albedo and g, which don't fit in the payload, go into a one-uint-a-pixel image
   (`rt_medium_image`, binding 50) written by the hit that entered the medium and read by raygen
   right after, flagged by `RT_MEDIUM_CODE_SCATTER`. Shadow rays leaving the medium attenuate by the
   extinction. `PhysXWaterSurface3D` writes `pathtraced_scattering` (default 0.05, 0.08, 0.12 per
   metre: clear coastal water) with g 0.85 (Petzold's ocean measurements are about 0.9).
2. **Caustics map edge.** Near the border of the caustics square, photons bent out of it are lost
   while none bend in from outside, so the bent/straight ratio dipped into a dark line on the sand.
   The ratio now fades to the plain sun over the outer 8% of the map.

## Step 3b as built: baked flow (9 Nov)

Paulo picked baked flow everywhere plus a live shallow-water solve near the player; this is the baked
half.

- **`current`** (m/s) on `PhysXWaterSurface3D`. With a measured seabed (`seabed_from_floor`, or a
  `surface_mesh` footprint) the flow over the simulated square is solved once, on the CPU, when the
  seabed or the current changes: the depth-weighted stream function above, `div(grad(psi) / h) = 0`
  by SOR, dry cells as walls (each dry patch one psi value: banks take theirs from the square's edge,
  islands the value that sends no net flow round them, updated Gauss-Seidel after each sweep or the
  two fight and diverge), the square's edge integrating the current's flux with inflow and outflow
  scaled to match. About 400 sweeps on a 160 x 160 grid, about 90 ms. Velocity `(dpsi/dz, -dpsi/dx) / h`, depth
  floored at 25 cm and speed capped at 4x the current. Past the square, and without a seabed, the
  flow is the current.
- **The surface is carried along it.** Every ocean texture read (height, choppy displacement,
  slopes, foam) goes through two-phase flow: two copies offset along the local flow for one
  `flow_period` (2 s) each and restarted half a period apart, weights `1 - |1 - 2p|`, the phase
  shifted by low-frequency noise so the restarts don't pulse in step. The foam lace is carried with
  the same phases. The CPU `sample_height()` does the same, so floaters ride the drawn waves.
- **Buoyancy in the current.** `PhysXBuoyancy3D` drags against `v_body - sample_flow()`, so floaters
  drift downstream and turn round rocks with the flow. On the bench (`--view=river --current=1,0`) the
  boxes drift about 3 m in the first 10 s, accelerating towards the current.
- Path tracing reads the same shader, so the moving surface is the same there.

Not yet: wakes and eddies behind rocks (potential flow has none; the live solve, step 3c, brings
them), foam spawned where the flow speeds up.

## Step 3c as built: live flow near the player (9 Nov)

The other half of Paulo's pick: the simulated square (the ripple grid) now solves the shallow-water
equations, so the flow there is live.

**Method.** Chentanez and Müller 2010's heightfield solver on the existing ripple grid, which
replaces the iWave-style wave equation (for small waves on still water the two are the same; the
shallow-water equations linearise to the wave equation with `c^2 = g h`):
- Staggered grid: surface height at cell centres, velocity on faces (each cell stores its +x and +z
  face).
- Velocity is the baked flow plus a live part. The baked flow goes in as face fluxes taken from the
  stream function at cell corners, so every cell's inflow and outflow cancel exactly: with nothing
  disturbing it the water stays flat, and a cleared or re-centred grid restarts from the baked flow.
  The square's edge passes the baked flux (still water in, the cell's height out).
- Two compute passes a tick. Height from the face fluxes with the upwind face depth (their eq. 4-6),
  the live part's depth capped at `0.45 dx^2 / (g dt^2)` for stability (their `h_avgmax` plays the
  same role). Then face velocities: semi-Lagrangian self-advection, gravity from the new heights
  (their eq. 8), the `|v| <= 0.5 dx / dt` clamp.
- Advection uses Catmull-Rom interpolation clamped to the four nearest values (the monotone cubic
  of Fedkiw, Stam and Jensen 2001) instead of their modified MacCormack, which needs one more pass;
  both keep eddies from smearing out the way plain bilinear does.
- **Wall friction.** Faces running along dry ground are slowed (`wall_friction`, 1.5/s). Without
  it the walls are slippery and potential flow has no reason to separate; with it the flow leaves
  rocks in shear layers that roll up into a wake. On the river bench the wake behind the rock
  starts as a recirculation bubble and after about 30 s meanders like the start of a vortex street.
- Damping (`damping`) acts on the live part only, so the baked current never decays.
- No moving wet/dry fronts yet (the swash pass still draws run-up), no overshoot fix (no breaking
  waves at these speeds).

**Where it shows.** The surface's two-phase flow reads the live velocity texture instead of the
baked one when it runs, so waves and foam swirl round rocks; `sample_flow()` adds the read-back
live velocity, so floaters and `PhysXBuoyancy3D` feel the eddies; the path tracer uses the same
shader and heights. `live_flow` off is the baked flow of step 3b.

**Bench.** `--view=wake --current=1,0 --tracers=4000` drops flecks along lanes upstream of the rock
and carries them with `sample_flow()`: off, the streaks part round the rock and close behind it;
on, they roll up in its wake. `--node=live_flow:false` for the comparison, `--pt-from=N` to settle at
raster speed and path trace the last frames.

**Cost.** Two passes over the grid a tick (160 x 160: 25,600 cells, a 4 x 4 cubic per face) plus
one readback of the state for the CPU while a current runs; a still lake costs the same as the old
wave equation's one pass did, twice.

## Step 3d as built: white water from the flow (9 Nov)

**How others do it.** Offline and in Houdini, white water is spawned from Ihmsen, Akinci, Akinci
and Teschner's whitewater potentials (Unified Spray, Foam and Bubbles for Particle-Based Fluids,
2012): air is trapped where neighbouring water moves past each other, at wave crests, and only
where the water has the energy to churn. Chentanez and Müller 2010 spawn foam particles on their
heightfield at steep, rising crests. Shallow-water river tools (Fluid Flux) keep a foam map that
the simulation fills where flow is fast or turbulent and that advects with the water. Hydraulics
gives the other trigger: water foams through chutes and hydraulic jumps, where the flow is
supercritical (Froude number `|v| / sqrt(g h)` above about 1; Chanson's measurements of air
entrainment in jumps).

**What was built.** A foam amount per cell of the live grid (the state's spare channel), in the
height pass:
- carried with the water (semi-Lagrangian, the cell-centre velocity), fading over
  `flow_foam_persistence` (8 s);
- made at `flow_foam` times `energy * max(trapped_air, supercritical)`: `trapped_air` from the
  mean change of velocity to the four neighbours over 25 cm (0.15 to 0.6 m/s), `supercritical`
  from the Froude number (0.8 to 1.6), `energy` from speed (0.5 to 1.5 m/s), so still and slow
  water never foams;
- written to the live velocity texture's blue channel; the surface draws it as foam with the same
  lace breakup as whitecaps, faded out over the square's last 6 %. Path tracing reads the same.

On the river bench at 1 m/s the rock's two shear layers trail foam lines a few metres downstream
that wander with the wake. Off: `flow_foam` 0.

## The full live version (documented, not built)

Paulo asked for this to be written down: the whole river, lake and flooded street as a live
shallow-water simulation, the way Fluid Flux does it in Unreal, rather than only a window near the
player.

**What it would do that the chosen design can't:** water that changes volume and goes where the
ground lets it. A river rising in a storm and spilling over its banks, a dam breaking, rain filling
a street and draining into gutters, a tide coming in over flats, a ship's bow wave reaching both
banks, debris piling up and backing the water up behind it.

**Method.** Chentanez and Müller 2010 on the GPU: water height and velocity per cell on a grid over
the terrain heightfield; semi-Lagrangian advection of velocity, height from the divergence of the
flux, their overshoot fix at steep fronts, wet/dry fronts that move. Two-way coupling with PhysX
bodies (bodies displace water and feel its drag and buoyancy, as `submit_sphere()` and
`PhysXBuoyancy3D` already do), particles spawned where the surface breaks (PhysX PBD particles or
`PhysXWaterSpray3D`), NVIDIA Flow mist over falls. Where a 3D effect is needed (a waterfall's
plunge) Chentanez, Müller and Kim 2014 add a local 3D grid.

**Making it fit an open world.**
- *Sparse tiles:* only tiles with water, or next to it, exist and step; a river 5 km long and 30 m
  wide at 0.5 m cells is about 600,000 cells, a few milliseconds a step on a desktop GPU, far less
  than a dense grid over the map.
- *Level of detail:* fine cells near the player, coarser further out, and tiles past a few hundred
  metres stepped less often (or frozen to their baked state) since nobody sees the difference.
- *Streaming:* tiles saved with the world chunk and restored on load; a river's sources keep
  feeding water in at fixed rates so it settles back to its baked state when nothing disturbs it.
- *Rendering and physics:* the surface reads tile height and velocity instead of the baked flow;
  the path tracer reads it through the same shader; `sample_height()` / `sample_flow()` read a CPU
  copy of the tiles near bodies.

**Costs and risks.** GPU time every frame for water nobody is looking at, memory for every wet tile,
stability at high speeds (the CFL limit makes fast rapids need small steps), and save-game size. It
also makes water nondeterministic across machines unless stepped in fixed point, which matters for
multiplayer.

**Path from the chosen design.** The live window near the player (next step) is this same solver on
one tile set; turning it into the full version is mostly the tile manager (allocation, LOD,
streaming) around it, plus sources and sinks. Hand-placed live areas (a flood scene, a hero river)
are the step between: the same tiles, only where a level designer marks them.

## Step 4 research: rivers from a path (9 Nov)

Goal: draw a path, get a river. Its surface runs downhill, it is as wide and deep as the path
says, it flows along the path and round rocks, it foams where it is steep, it carries what floats
in it, and it joins a lake or the sea. Everything built in steps 1 to 3 (path-traced medium,
caustics, baked flow, live flow, white water, buoyancy against the current) should work on it
unchanged.

### How engines and tools build rivers

| Who | Authoring | Surface along the river | Ground | Flow | Physics | Joins |
|---|---|---|---|---|---|---|
| Unreal Water plugin (`WaterBodyRiver`) | Spline; per point `Depth`, `River Width`, `Velocity` (sign = direction), audio intensity; optional curl noise | Points may have different heights (lakes may not); no waves on rivers | Carves the Landscape through an edit-layer brush: elevation curve (default S-curve) times per-point depth, channel edge offset, ramp width, falloff by angle or width, blend modes (alpha, min, max), blur, terracing | Velocity written to a flow map along the spline | Depth and flow are queryable for gameplay and buoyancy | Rivers are meant to connect lakes and the sea; "River to Lake" and "River to Ocean" transition materials; all bodies share one water mesh and one water info texture |
| Crest (Unity) | Spline at the water surface; per-point flow speed | `RegisterHeightInput` sets the water level from the spline | None | `RegisterFlowInput` moves the water along the spline; FFT waves can run along it | Shared with the ocean | Lakes are spline-shaped height inputs over the global sea; example scene joins two lakes with a river |
| Waterways (Godot 3 addon) | Bezier `Curve3D` with a width per point, step count | Follows the curve | None; the river sits where it is drawn | Bakes a flow map in the ribbon's UV2: raycasts the collision for obstacles, distance field, gradient turned 90 degrees, a "pressure" pass for narrows, a foam pass | None built in | None |
| Horizon Zero Dawn (Houdini tool, 2017 talk) | A curve plus the terrain | Curve snapped to terrain, then a running minimum along it: "it only goes down; rather than going over the hill, it cuts the hill" | Edges projected back to the terrain; width from a ramp, narrower where steep (canyons); bed let "slide down" the terrain | Vertex colour flow | | Reservoirs and cascades where it would otherwise climb |
| Horizon Forbidden West (Malan 2022) | As above | Local Houdini sims baked to flipbook deformation volumes (128 layers) for standing waves round rocks, eddies, ripples, placed procedurally and reviewed by artists | | | | Waterfalls: separate system, not covered |
| BeamNG, Torque 3D | 3D spline; width per node (BeamNG depth is global; Torque per node) | Flat by default; nodes rotated to slope it (Torque) | None; artist hides the edges under terrain by hand | One scalar `FlowMagnitude` force for the whole river | Density and viscosity for buoyancy and drag | None |
| Proland (Yu et al. 2009) | A graph of bank curves with a stream-function value per bank, plus an axis curve | Terrain mesh | Elevation layer under the river | Velocity from the stream function interpolated between the nearest banks, evaluated per point, no global solve; wave sprites carried by screen-space particles | | |

What they agree on: the river is a spline with width, depth and speed per point; its surface is
not flat; flow follows the spline; the river owns its own surface mesh. Where they differ is the
ground: Unreal carves its own landscape, Horizon edits the terrain in Houdini, everyone else sits on
whatever ground is there.

### What papers add

- **Peytavie et al. 2019, Procedural Riverscapes.** River types after Rosgen (A, B, C, D, E, F, G).
  Cross-sections are unit-area templates scaled per point by discharge over speed (continuity). Bends
  get an asymmetric section (deep on the outside, a shallow bar on the inside), straights a
  symmetric one, braided reaches several channels. The surface is checked to fall monotonically
  along the river; where the bed is steeper than a threshold they level it and insert a cascade with
  a plunge pool. Rapids: turbulence on steep straights; vortices behind rocks.
- **Génevaux et al. 2013, Terrain Generation Using Procedural Models Based on Hydrology.** River
  primitives blended into the terrain: the river's height is its axis height plus a cross profile of
  the distance to the axis, blended with the ground by a compact weight `(1 - d^2/r^2)^k`. Also the
  discharge law `Q = 0.42 A^0.69` (drainage area A, from Dunne and Leopold).
- **Kipfer and Westermann 2006, Realistic and Interactive Simulation of Rivers.** SPH particles
  over terrain with a surface built from them, 3,000 to 20,000 particles at 12 to 68 fps. Shows a
  whole river can be simulated, at a particle budget that would not cover a game river; the
  heightfield route (Chentanez and Müller, step 3c) is what scales.
- **Yu, Neyret, Bruneton and Holzschuch 2009.** The bank stream function (step 3 above): the same
  quantity the fork already solves at bake time.

### What hydraulics adds

Rivers obey a few rules that make the authoring easier and the result believable:

- **Continuity.** Without tributaries the discharge `Q = width x depth x speed` is the same all
  along the river. So narrows are fast, pools are slow, and the author only has to give the flow
  once: set `discharge`, and each point's width and depth decide its speed. (A tributary is a second
  river whose mouth adds to it.)
- **Manning's equation** (uniform open-channel flow): `v = R^(2/3) S^(1/2) / n`, with `R` the
  hydraulic radius (area over wetted perimeter, about the depth for a wide river), `S` the surface
  slope and `n` the roughness (about 0.03 for a clean natural channel, 0.05 for a weedy or stony one,
  0.013 for concrete). With `Q` and the width known it gives the depth for each slope; or with the
  depth known, the speed. Example: 6 m wide, 0.8 m deep, `n = 0.035`: slope 0.2% gives 0.9 m/s
  (Froude 0.3, calm); slope 3% gives 3.6 m/s (Froude 1.3, supercritical). The white water from step
  3d already keys on Froude above 0.8, so steep reaches foam by themselves.
- **Across the channel** (divided-channel method, Chow 1959): with the same slope everywhere across,
  each strip's discharge goes as `d^(5/3)`, so the flow is fastest over the deep middle and slows to
  nothing at the banks. Integrated across, that is the stream function's profile from bank to bank:
  `psi(n) = Q * int_0^n d^(5/3) / int_0^W d^(5/3)`.
- **Leopold hydraulic geometry** (Leopold and Maddock 1953): downstream, width grows as `Q^0.5`, depth
  as `Q^0.4`, speed as `Q^0.1`. Gives sensible defaults when the author only sets the discharge.
- **Slope limits.** Gravel rivers run about 0.1% to 2%; above about 4% to 6% a river becomes steps
  and pools (Rosgen A); where the ground drops more steeply than that, the surface should step: a
  cascade or a fall, as Peytavie and Horizon do.

### Design for the fork

**A `PhysXRiver3D` node built on the parts we have.** The key move, from Waterways: run everything in
the river's own *strip* coordinates. Unroll the river along its arc length `s` and across its width
`n`; the strip is a rectangle. In the strip:

- the **baked flow** is the stream-function solve already in `_bake_flow()`, with the left bank held
  at 0 and the right bank at `Q`, the cross profile from the `d^(5/3)` rule as the start, rocks found
  by raycast as before;
- the **live flow** is the same shallow-water solver (step 3c) on a window of the strip around the
  player; heights are measured from the river's rest surface, which slopes, so the solver itself does
  not need to know about slope (gravity and bed friction balance in the base flow; the solver works
  on the disturbance, exactly as on the flat surface today). Bends add metric terms of order
  `width / radius`, small for any river that is not a hairpin; ignored at first;
- **white water**, flow foam and the two-phase flowed normals read the strip textures through the
  ribbon's UV, so the shader code is shared with `PhysXWaterSurface3D`;
- the **path tracer** sees an ordinary mesh with the water medium (steps 1 to 3a), so bending, depth
  tint, murk and caustics come for free. The surface is real geometry, so a slope or a step is no
  problem for it.

**Surface height.** Each path point's height is snapped to the ground (or taken from the path), then
a running minimum downstream, so it never climbs (Horizon, Peytavie). Where the drop between points
is steeper than `max_slope` the surface steps: a cascade, with a plunge pool and white water at its
foot. Between points the surface follows Manning's slope.

**Authoring.** `Curve3D` points carry `width`, `depth` and optionally `speed`; one `discharge` for the
river; `roughness` (Manning `n`). Two of width, depth and speed decide the third by continuity. Points
are spaced by curvature for the ribbon (Waterways' step count), about 4 flow texels per metre (Valve's
density).

**Buoyancy and physics.** `PhysXBuoyancy3D` today points at one `PhysXWaterSurface3D`. The river
offers the same `sample_height()` and `sample_flow()` (height from the strip, flow from the strip
textures turned back into world space), and buoyancy accepts either body, so floaters drift
downstream, catch on rocks and spin in eddies. The river's CPU copy is the same strip grid.

**Joins.** The river's last point snaps to the lake's or sea's `water_level`; its ribbon fades into
that surface over a short band (Unreal's transition materials do the same); its outflow later feeds
the lake's current as an inflow jet. Tributaries: a river whose mouth lies on another river adds its
discharge downstream.

**Level of detail.** The ribbon is cut into chunks along `s` with visibility ranges; flow textures
are per chunk; the live window and the CPU copy only exist near the player. A 5 km river at 4 texels
per metre and 30 m wide is about 2.4 M texels of baked flow (about 19 MB at RGBA16F), streamed by
chunk.

### The open choice: what does the river do to the ground?

1. **Sit on the ground.** The river raycasts the existing collision for its bed and banks, as the
   seabed and rocks do today. Works on any terrain, mesh or city canal, needs no terrain system; the
   ground must already have a channel, or the water clips into it. Edges where the surface meets the
   ground get the shore fade and foam that lakes already have.
2. **Also build a channel.** As 1, plus the node can generate its own bed and bank mesh with
   collision from the cross-section (trapezoid, or asymmetric at bends after Peytavie), skirted down
   into the ground. Good for flat ground, blockouts and city canals.
3. **Carve the terrain.** Unreal's way: the river writes its channel into a terrain heightmap. Godot
   has no terrain in core; it would need an adapter per terrain addon (Terrain3D, HTerrain) or an
   exported height image to import. Most work, and the one an open world wants long term.

Recommendation: 1 and 2 now (2 as an option on the same node); 3 later as an adapter, since its
input is the same cross-section that 2 builds.

### Plan

1. `PhysXRiver3D`: path, per-point width/depth/speed, discharge, roughness; surface by running
   minimum and Manning; ribbon mesh with strip UVs; shared water material (path traced).
2. Baked flow in the strip: `d^(5/3)` cross profile, stream-function solve round rocks; flowed
   normals and foam.
3. Cascades where the drop is too steep; white water from Froude at their foot.
4. Buoyancy reads rivers; `sample_height()` / `sample_flow()` on the river.
5. Live flow window in the strip (solver reused); joins to a lake or the sea.
6. Option: the generated channel (choice 2). Later: terrain carving adapter (choice 3).

## Step 4a as built: the river node (9 Nov)

`PhysXRiver3D` (a `Path3D`): curve from source to mouth, `widths` and optional `depths` per curve
point, one `discharge`, Manning `roughness`, `max_slope`, `snap_to_ground`.

- **Ground.** With `snap_to_ground` the river raycasts the static ground once per cross-section
  sample (every `step_length` along, `cross_segments` across), the same way the ocean measures its
  seabed. It sits on whatever ground is there: terrain, a mesh, a city canal.
- **Surface.** Where no depth is given, the surface is the height at which the measured
  cross-section carries the discharge: Manning's equation over the divided channel,
  `Q = sum(d^(5/3) dn) S^(1/2) / n`, solved by bisection per section. `S` is the bed slope averaged
  over about 10 m, leaving out steps steeper than `max_slope`; counting a fall in the average
  drained the water either side of it to a film. Then a running minimum downstream, so it never
  climbs, and drops steeper than `max_slope` are flagged as cascades: the ribbon itself becomes the
  falling sheet.
- **Flow.** Continuity across: each sample carries `Q d^(5/3) / sum(d^(5/3) dn)`, so the speed is
  `Q d^(2/3) / sum(...)`: fastest over the deep middle, zero at the waterline, faster in narrows. In
  the bench (6 m3/s, parabolic channel 6 m wide, 0.5% slope) that is about 1.8 m/s mean; floating
  boxes drift downstream along the core.
- **White water.** Froude number from the mean speed and mean depth,
  `smoothstep(0.8, 1.6, Fr)` as in step 3d, and 1 at every cascade; carried downstream, fading as
  `exp(-ds / (v persistence))`, so the foam trails 10 to 20 m below the fall. Across, it gathers in
  the fast core; at a cascade it spans the river.
- **Mesh and shader.** One ribbon, built on the CPU: UV = (arc length, distance across), UV2 = flow
  (m/s), COLOR = depth, foam, fade past the banks. All three reach the path tracer's hit shader, so
  the river needs no textures and is not affected by the displaced-shader issue. The shader runs
  two-phase flowed ripples (value-noise slopes, three octaves, blended as surface gradients and
  renormalised) that get rougher with speed, and the same flowed foam lace as the ocean. Under path
  tracing it writes the same water medium as `PhysXWaterSurface3D` (IOR 1.33, absorption,
  scattering), a little murkier by default.
- **Buoyancy.** `PhysXBuoyancy3D` accepts a river as its water: height and flow come from
  `sample_height()` / `sample_flow()`, which find the section from the curve's closest offset and
  interpolate across.

Not yet: the stream-function solve round rocks inside the river (a rock now only makes a dry patch
in the cross-section), the live window, joins to lakes and the sea, the optional channel mesh, and
carving terrain.

## Step 4b as built: the river as a carve brush (9 Nov)

Paulo's choice (18:28): ground shaping goes through a Berlenga deformation layer (terrain note,
section A+) that keeps the uncarved heights, stacks brushes and writes to Terrain3D (by name) or a
mesh ground. The river is its first brush; it never edits terrain itself.

- `carve` on: the channel floor is `carve_depth` under the uncarved ground at the centre, with a
  running minimum downstream (Horizon's "it cuts the hill"). The section is a trapezoid, the floor
  40% of the width, banks smoothstepped up to the ground; past the banks the brush rises at
  `carve_bank_slope` and fades over `carve_falloff` (Génevaux et al.'s compact blend weight, here a
  smoothstep). At bends the floor shifts towards the outer bank by `curvature x width^2 / 4`, at most
  a quarter of the half width (Peytavie et al.'s asymmetric section for type C reaches); curvature is
  averaged over about 6 m.
- Brushes: one per 64 m world tile, `rect`, `cell_size`, `size`, `heights`, `mask`, `mode = "lower"`
  (the ground goes down to the height, blended by the mask, never up), re-sent on `carve_changed`.
- `base_height_source`: a node answering `base_height_at(world_xz)` with the uncarved ground. Without
  it the river would measure its own carved channel on the next rebuild and cut deeper each time.
- The water uses the carved profile for its Manning depth, so its surface and flow match the channel
  the deformer will leave. In the bench (flat valley floor, `--carve`) the bench script plays the
  deformer: it lowers its mesh ground to the brushes and rebuilds the collision.

## Step 4c: flow round rocks (9 Nov)

Until now each cross-section shared the discharge out by itself (`d^(5/3)`), so a rock only made a
dry patch: the water beside it ran straight on, and nothing foamed at it.

**What others do.** Waterways (Godot 3) bakes a flow map round obstacles from a distance field
turned 90 degrees, plus a "pressure" pass for narrows and a foam pass. Proland (Yu et al. 2009)
interpolates a stream function between the banks. Peytavie et al. 2019 put vortices behind rocks;
Horizon Forbidden West places baked Houdini flipbooks for standing waves and eddies round rocks.
Flood models (LISFLOOD-FP, Bates and De Roo 2000; Hunter et al. 2005) drop inertia from the shallow
water equations where friction dominates, which is the river's normal state: the *diffusive wave*.

**Built: a diffusive-wave stream function in the river's strip.** With friction in charge, the unit
discharge follows the surface gradient, `q = -K grad(eta)`, `K = d^(5/3) / (n sqrt(S))` (Manning).
Continuity makes `q` the curl of a stream function `psi`; that `q / K` is a gradient gives
`div(grad(psi) / K) = 0`. This is the ocean's flow bake (step 3, weight `1 / d`) with Manning's
weight `1 / d^(5/3)` instead, and the difference matters: in a straight reach its solution is
exactly the divided channel's `d^(5/3)` profile (fast middle, still banks), where the `1 / d`
weight would flatten the speed across the river. Round a rock the same equation parts the water,
speeds it up past the rock's sides and slows it in front and behind.

- Solved on the ribbon's own grid (`step_length` along, `cross_segments` across) in strip
  coordinates, with the bend's metric `h = 1 - curvature n`: along-faces weigh `1 / (h K ds^2)`,
  across-faces `h / (K dn^2)`. `psi` is 0 on the right bank and the discharge on the left; both
  ends keep their own section's profile; a rock is an island whose single `psi` is chosen so no
  net flow goes round it (as the ocean bake). SOR from the section profiles, so a river with no
  rocks needs no iterations at all.
- Velocity: along `= dpsi/dn / d`, across `= -dpsi/ds / (h d)`. The mesh's UV2 and
  `sample_flow()` now carry both, so ripples, foam and floating bodies swing round rocks.
- White water per sample instead of per section: the local Froude number, every cascade, and the
  water touching a rock (its upstream pillow and the shear lines past its sides, more the faster
  it runs). It is carried downstream along the local flow and fades at the mean current's pace,
  so the two trails from each rock close in behind it, as on a real river.
- Rocks are found by the ground rays. A carving river measures the uncarved terrain (which has no
  rocks), so `obstacle_collision_mask` adds rays against rocks on their own physics layers.

**Not in it.** The diffusive wave has no inertia, so no eddies shed behind a rock, no standing
wave over a submerged boulder and no hydraulic jump. Those need the shallow water equations with
their advection term: the live window (next).

## Step 4d: the live window on a river (9 Nov)

The design above put the live flow in the river's strip coordinates. Built instead on a world
square, because the solver, its textures, readback, foam and body coupling already exist for
`PhysXWaterSurface3D` (step 3c) and work unchanged:

- **The same solver.** `PhysXRiver3D` owns a `WaterSolver` (Chentanez and Müller 2010's
  heightfield shallow water, water_ripple.glsl) on a square of `live_domain_size` (48 m) and
  `live_grid_resolution` (192) round the point of the river the camera looks at, or round
  `live_follow`. Its smallest ocean layer runs with zero amplitude (a river has no wind sea).
- **Fed from the river.** Each cell's still depth is the river's depth there (cells off the
  river are walls); the baked face fluxes are differences of the river's stream function (step
  4c) at cell corners, so the baked flow enters the live solve divergence-free and, left alone,
  stays put. The river's psi grows towards its left bank, the ocean's the other way, so it goes in
  negated. Moving a quarter of the square re-centres it and restarts it from the baked flow.
- **On a sloping surface.** The solver carries only the disturbance on the river's own surface:
  gravity and bed friction balance in the baked flow, so the live part needs neither. What it
  adds by itself is the velocity head, `v^2 / 2g`, that Manning's normal depth leaves out: a
  tilt of 10 to 25 cm across the window where the river speeds up. Kept, that would step the
  surface at the window's edge and slide with it, so the window's mean plane (least squares over
  its wet cells, refitted at each readback) is taken out and only local motion stays.
- **What it gives.** Wall friction along rocks separates the flow into shear layers that roll
  up: behind each rock a recirculation bubble, then within 10 to 20 s a meandering wake (the start
  of a von Kármán street), drawn by the live flow foam (`flow_foam`, 0.25 by default on a river,
  since a river runs fast everywhere). The shader reads the live height (vertex rise and per-pixel
  slope), the total velocity (ripples and foam ride it) and the live foam; `sample_height()` and
  `sample_flow()` add the read-back live parts, so floaters swirl in the eddies, and floating
  bodies push the live water as on the sea.
- **Limits.** A river's source or mouth inside the window has no flux in or out there (water
  piles at the end); joins to lakes and the sea come next and give the mouth somewhere to go.
  Past the square's edge the baked flow carries on.

## Step 4e: joins to lakes, the sea and other rivers (9 Nov)

**What others do.** Unreal's water bodies are meant to connect: a river's last spline point sits
on the lake or ocean, and "River to Lake" / "River to Ocean" transition materials blend the
river's surface into the other body's over the overlap; all bodies share one water mesh. Crest
joins lakes with a river by height inputs over a global sea. Hydraulics adds three things:

- **Level pool / backwater.** A river can't run below the water it flows into: near the mouth the
  water ponds back until its own normal depth rises above that level (the M1 backwater curve of
  gradually varied flow, Chow 1959; the level pool is its usual first approximation).
- **The plume.** Outflow into still water is a turbulent jet (Rajaratnam 1976, Turbulent Jets): a
  potential core a few widths long at the mouth speed, then a centreline speed falling as
  `x^(-1/2)` while the half-width grows by about 0.1 per metre, a Gaussian profile across; in
  shallow water bed friction wears it down over about `2h / c_f` (Giger, Dracos and Jirka 1991,
  shallow jets). River plumes at sea add buoyancy (Horner-Devine, Hetland and MacDonald 2015);
  not modelled.
- **Confluences.** Below a junction the discharges add (continuity); Best 1987's confluence zones
  (stagnation at the corner, flow deflection, a separation zone below the side bank, a shear layer
  between the two streams) are inertial, so they belong to the live window.

**Built.**
- `mouth_target` on `PhysXRiver3D`: a `PhysXWaterSurface3D` (lake or sea) or another
  `PhysXRiver3D`. The mouth is the first section over that water whose own surface has come down
  to its level; the river's surface never falls below that level (level pool), and past the mouth
  the ribbon fades out over `mouth_blend` (6 m) into the other water, foam with it.
- Into a lake or the sea: the river registers its mouth (`set_inflow()`: point, heading, width,
  discharge). The water surface bakes each inflow as Rajaratnam's plane jet over its simulated
  square into a plume texture added to its flow: waves and foam ride it, `sample_flow()` includes
  it, so floaters are carried out from the mouth. The live water does not simulate the plume.
- Into another river: the main river adds the tributary's discharge from the join down
  (`get_discharge_at()`), so it runs deeper or faster below it (Manning with the new discharge),
  and its stream function steps by that discharge on the bank the tributary joins from. The
  tributary takes the main river's surface at the join as its level; when the main river is
  rebuilt it re-reads it (only a change over 1 cm wakes it, so the two settle).
- Bench: `docs/bench/river --joins` (a lake basin at the bottom of the valley and a stony creek
  joining below the step), `--view=joins|mouth`. On it the main river carries 6 m3/s down to the
  junction and 8 below it, ponds to the lake level about 2 m before the shore, and its plume still
  runs at about 1.6 m/s 20 m out into the lake.

**Not yet.** A lake's outlet (a river whose source is a lake's shore) works by drawing the river from
there, but the lake does not drain towards it. Lakes have no level budget (inflow does not raise
them).

## Step 4f: waterfalls (9 Nov)

Until now a fall was a cascade: the ribbon draped down the cliff at whatever
slope the ground had. A real fall leaves the lip and drops free.

### What the hydraulics says

- **Brink depth.** At a free overfall the flow passes through critical depth
  a few critical depths upstream, and at the lip the depth is
  `y_b = 0.715 y_c`, with `y_c = (q^2/g)^(1/3)` (Rouse 1936; Chanson,
  *The Hydraulics of Open Channel Flow*). So the surface draws down over about
  `4 y_c` before the lip. The water leaves the lip at `v0 = q / y_b`.
- **The nappe.** Past the lip the water falls freely, so its path is a
  projectile: `x = v0 t`, `y = -g t^2 / 2`. Its thickness thins as it speeds
  up: `t = q / |v|`, `|v| = sqrt(v0^2 + (g t)^2)`.
- **Break-up.** A falling sheet takes in air at its surfaces, spreads by a few
  per cent of the fall, and breaks into clumps and droplets once the
  disturbances reach its core (Ervine and Falvey 1987; Ervine, Falvey and
  Withers 1997). For a nappe the break-up length is about
  `L_b = 6 q^0.32` m, with `q` in m^2/s (Horeni 1956, as used by Castillo
  2006 and Carrillo 2020). Below `L_b` the fall arrives as white streaks,
  not a sheet.
- **Plunge pool.** The jet enters at `v = sqrt(v0^2 + 2 g H)`, carries air
  down, and the pool boils white over a few jet thicknesses to a few metres.
  Spray is thrown up at a good fraction of the impact speed. Mist is the
  finest drops, whose fall speed is a few cm/s (Stokes), carried out by the
  air the fall drags down with it, the "waterfall wind".

### How others do it

- **Games.** Most waterfalls are hand-placed meshes with scrolling textures,
  plus particles at the foot (Unreal Water tutorials, community UE5 setups).
  Horizon Forbidden West's water talk (Malan, SIGGRAPH 2022) leaves
  waterfalls out except for baked 2D impact effects on the pool.
- **Particle fluids.** Bagar, Scherzer and Wimmer (EGSR 2010) render an SPH
  waterfall and class each particle as water or foam by its Weber number
  `We = rho v^2 l / sigma`. That is too costly for a river system, but it
  confirms the idea that speed decides whiteness.

### Design for the fork

1. Find falls in the river: a run of cascade sections that drops more than
   `fall_min_height` (default 1.5 m), and only where the free jet stays clear
   of the ground (a steep chute stays a cascade).
2. Find the lip by bisecting ground raycasts between the last high section
   and the next one. Draw the surface down to `0.715 y_c` over `4 y_c`.
3. Build a curtain mesh, a thin slab with two faces `q/|v|` apart, so the
   path tracer sees a real sheet of water. One column per wet cross sample
   follows the projectile from that sample's own `q`. UV carries the travel
   time, so the streaks move with the water and stretch as it speeds up.
   Aeration grows to white over `L_b`, and past it the sheet breaks up
   (alpha holes from noise).
4. Hide the ribbon under the jet. White water starts where the jet lands.
5. Add spray (GPUParticles3D, launched from the impact line at a share of the
   impact speed) and optional mist (a PhysXFlowEmitter3D at the foot, fed
   into the scene's PhysXFlow3D, so it is traced under path tracing). Both
   scale with the fall's power `rho g Q H`.
6. `get_waterfalls()` reports each fall: lip, height, impact point and speed,
   and break-up length.

### As built

`fall_min_height`, `fall_spray`, `fall_mist`, `fall_material`, `get_waterfalls()`.
Bench: `docs/bench/river --falls` (views `fall`, `fallfront`), a 6 m cliff with
6 m^3/s on a 5 m river. It reports q = 1.2 m^2/s, lip speed 3.2 m/s,
impact 10.6 m/s, throw 3.3 m and break-up length 6.4 m, so this sheet arrives
just whole. Under path tracing the sheet refracts and keeps the raster streaks
at 100% crop. Mist needs a build with NVIDIA Flow and a PhysXFlow3D in the
scene, so it is not shown in the cloud shots. Limits: the lip is taken as
level across the river, each column's jet is independent (no contraction of
the sheet), and the live window does not see the fall.

## Step 5a: ocean wave cascades and the TMA spectrum (9 Nov)

### Problem

The ocean was one FFT tile (`ocean_domain_size`, 40 m by default). A tile holds
no wave longer than itself, and its longest waves repeat visibly. A strong wind
over open water should raise long, tall swell. At 14 m/s a fully developed sea
peaks near 165 m wavelength and has `Hs = 0.22 U^2 / g`, about 4.4 m
(Pierson-Moskowitz). The tile gave 1.1 m.

### How others do it

- **Several tiles of growing size, each with its own band of the spectrum.**
  Unity HDRP Water has three bands: swell, agitation and ripples. Crest sums
  wave cascades per LOD. Sea of Thieves (SIGGRAPH 2018) and the Godot FFT
  ocean write-ups by Jump Trajectory and others use 3 to 4 cascades, cutting
  each tile's lowest few modes and handing them to the next, larger tile. That
  hides the repeat, because no tile's own fundamental shows.
- **Spectrum in finite depth: TMA** (Bouws et al. 1985). JONSWAP times the
  Kitaigorodskii depth factor `Phi(w_h)`, `w_h = w sqrt(d/g)`. Horvath
  ("Empirical directional wave spectra for computer graphics", DigiPro 2015)
  gives the piecewise form used here, and recommends TMA as the base spectrum
  for shallow seas.

### As built

- `wave_cascades` (1 to 4, default 1, unchanged) and `wave_cascade_ratio`
  (default 6): cascade `i` is a tile `ocean_domain_size * ratio^i`. Each cascade
  keeps waves shorter than a quarter of its tile (`k >= 8 pi / L_i`) and hands
  longer ones to the next. For N >= 32 and ratio <= 8, the next tile's Nyquist
  limit stays above the split.
- Each extra cascade is an ocean-only `WaterSolver` (a resting 8x8 ripple grid)
  with a `k_min` / `k_max` band in the spectrum init. Its height, displacement,
  derivative and foam textures are summed in the surface shader (foam takes the
  most of any), and its readbacks are summed in `sample_height()` (the choppy
  displacement inversion included) and `sample_vertical_speed()`.
- The spectrum init multiplies by TMA's `Phi`. Shallow `depth` (3 m by default)
  now takes energy out of waves longer than about 7 s. For open water, set
  `depth` to the real depth.
- Bench: `docs/bench/water_surface --view=sea` prints `Hs` from 40 s of
  `sample_height()` at one point. At 14 m/s and depth 60 m: 1 cascade 1.09 m,
  3 cascades 3.55 m (fetch-limited, near the shallows), against 4.4 m for a
  fully developed sea.
- Limits: shore foam and swash still see cascade 0 only (the breaking-waves
  step will use all of them); caustics are from cascade 0; whitecaps are per
  cascade, not from the summed Jacobian.

## Step 5b: breaking shore waves (9 Nov)

### What the sea had

The FFT ocean fades out in the shallows (`shallow_fade_depth`), and the shore
had swash, wet sand and shore foam driven by the ocean's height at the
waterline. No wave turned toward the beach, grew, or broke.

### How others do it

- **Horizon Forbidden West** (Malan, SIGGRAPH 2022): artists draw shape, guide
  and animation curves per beach. A Coons patch interpolates the wavefronts,
  and one hand-animated breaking cross-section (a 2D texture, rising at 0.27,
  steepening at 0.4, curling into foam at 0.5) is played along them. It looks
  superb, but it is authored per beach.
- **Procedural shore waves** (Sea of Thieves, Crest, Unreal community tools):
  a distance field to the shore steers Gerstner-like trains toward it, with
  heights shaped by depth and foam where they break.
- **Physics.** Linear wave theory gives everything needed:
  - Dispersion `w^2 = g k tanh(k h)`, using the explicit inverse of Fenton and McKee 1990.
  - Ray theory: the phase obeys the eikonal `|grad S| = k(h)`, so crests turn to follow the depth contours.
  - Shoaling from energy flux: `H = H0 sqrt(cg0 / cg)`.
  - Depth-limited breaking at `H / h = 0.78` (McCowan 1894). Inside the surf zone the height stays near that limit, as a bore.

### As built

- `shore_waves`, `shore_wave_height` (0.8 m), `shore_wave_period` (8 s) and
  `shore_wave_direction` (the wind's by default). There are two trains: the
  second is 1.13 times the period, 0.6 times the height, and 8 degrees round,
  so the waves come in sets.
- A CPU bake over the ripple grid runs whenever the seabed is measured:
  - The phase is solved by fast sweeping (Zhao 2005) from the edges the waves come in through, and dry cells are filled from their wet neighbours.
  - The gradient gives `k` and the direction of travel. The height is the shoaled height, capped at `0.78 h`, and fades out deeper than a quarter of the deep-water wavelength, where the FFT ocean carries the sea.
  - The results go into two RGBA float textures.
- Shader: each train is a trochoid riding `S - w t`. Where `H` reaches the
  breaker limit, the profile is skewed shoreward (`cos(th - a cos th)`, which
  steepens the front), and the crest and a trailing band behind it turn white.
  Heights vary slowly along the crests. Normals use the analytic slope.
  `shore_signal()` (the moving waterline) includes them.
- `sample_height()` and `sample_vertical_speed()` include them, at the resting
  point, without the horizontal sway.
- Bench: `docs/bench/water_surface --view=surf --node=shore_waves:true`.
  `--profile` prints the surface along the beach: troughs of -0.6 m over about
  2 m of water, crests of +0.2 m over the last half metre, travelling in.
- Limits: there is no plunging curl (the profile only steepens). The GPU
  swash's run-up still reads the FFT ocean, while the visible edge follows
  the shore waves through `shore_signal()`. There is no reflection off steep
  shores, and no wave-current interaction.

### The living waterline (9 Nov)

Paulo asked for a waterline that is not a ruler-straight line. What each
water already had, and what this step adds:

- **Sea and lakes** already break the line: `shore_signal()` moves it with the
  waves (now including the shore waves), swash runs up and back, the wet sand
  band darkens behind it, and the shore foam lace is broken by noise lobes.
- **Rivers** had none of that: the edge was where the flat surface met the
  bank. The river shader now:
  - laps the edge with noise that rides the flow phases (`edge_lap_height`,
    `edge_lap_scale`), plus a slow breathing term, so the line creeps up and
    back along the bank;
  - adds a static relief (`edge_relief`, three octaves) for small stones and
    hollows, so the line is ragged even where the bank is steep;
  - draws a thin foam band in the last few centimetres (`edge_foam`).
- These are shading effects on the depth the river already stores, so they
  cost a few noise lookups per pixel and work the same raster and path traced.

## Step 5c: run-up and curling breakers (9 Nov)

### Run-up

The swash used to read only the FFT ocean, so the breaking shore waves reached
the waterline but the film on the sand ignored them. It now adds the shore
waves' surface rise at the waterline (the same phases, heights and clock as
the material), and caps the reach with the empirical 2% run-up of Stockdon et
al. (2006):

`R2 = 1.1 (0.35 b sqrt(H0 L0) + 0.5 sqrt(H0 L0 (0.563 b^2 + 0.004)))`

with the deep-water height H0, wavelength L0 = g T^2 / 2 pi and the local
beach slope b. For 0.8 m, 8 s swell on a 1:11 beach that is about 0.6 m
vertical, roughly 7 m up the sand.

### Curling breakers: how others do it

- **Fournier and Reeves (1986)**: particle orbits that tilt and flatten in
  shallow water, plus a phase term that speeds up the top of the orbit, so the
  crest overtakes the face and overhangs.
- **Horizon Forbidden West (Malan, SIGGRAPH 2022)**: one hand-animated breaking
  cross-section stored in a texture (rows are frames: rising at 0.27,
  steepening at 0.4, curling into foam at 0.5) and played along authored
  wavefronts on a finely tessellated strip. Overhangs come straight from the
  displacement.
- **Thurey et al. (2007)**: steep fronts of a shallow-water solver are tracked
  as lines that spawn particle sheets for the falling lip, then drops and foam.
- **Which breakers plunge**: the surf similarity (Iribarren) number
  `xi = tan b / sqrt(H0 / L0)` (Battjes 1974): spilling below about 0.5,
  plunging from 0.5 to 3.3, surging above.

### As built

A procedural cross-section in the spirit of Malan's, driven by physics instead
of authored curves:
- The bake stores, per cell and train, how far the wave is through breaking
  (its unbroken shoaled height over the breaker height 0.78 h: 1 at the break
  point, rising shoreward) and the surf similarity number from the local
  slope.
- As a wave breaks its crest sharpens and its trough flattens, as in cnoidal
  waves: `((1 + cos) / 2)^q` with q from 1 to 2.5, kept zero-mean.
- On plunging beaches the crest is thrown shoreward by up to 0.7 / k (a
  fraction of the wavelength) over the steepening face, so the surface folds
  over; then the lip falls by up to 1.6 times the amplitude onto the water
  ahead and turns white, over one stage of breaking, and the wave runs on as a
  bore.
- Normals come from differences of the full displacement, so the underside of
  the lip faces down. The raster pass draws both sides of the surface inside
  the plunge; the path tracer sees the folded geometry as it is (the folded
  layer's winding is reversed, which is the right way round for the water
  under the lip).
- `shore_wave_curl` scales it; `mesh_detail` (1-4) refines the mesh. The fold
  needs cells of about a tenth of the wave height: 0.1 m cells for a 1 m
  breaker.
- A profile check of the cross-section through one period (0.8 m, 8 s swell,
  1:11 beach) shows the crest overtaking the face and the lip hooking down for
  about a second before the bore.

Limits: no air tube is resolved in the raster pass (the water is drawn without
depth, so the lip and face blend), no spray sheet from the lip (both added in
Step 5e, which replaced this fold with separate lip geometry), and the stage is a
function of depth, so every crest plunges at the same place along a ray.

Path tracing check: the first build drew fans of long triangles over the surf
under path tracing only. The lip and drop weights were written as
`exp(-pow(x, 2.0))`, and GLSL leaves `pow` undefined for a negative base: on
lavapipe it gives NaN for half of every period, which raster drops silently
and the BLAS built from the captured vertices turns into fans. They are now
`exp(-x * x)`. Raster and path traced frames of the curl view then match (100%
crop), and the whitecaps at sea land in the same places under both.

## Step 5d: whitecaps across cascades, and a performance pass (9 Nov)

### Whitecaps where the cascades add up

Each cascade's foam pass sees only its own band, so the chop riding a long
swell crest, where the waves really break, never folded in any one of them.
The material now also takes the Jacobian of the summed displacement
(Tessendorf's folding test on all cascades together) and whitens the crests
where it drops below `foam_threshold`. The persistent foam stays per cascade
(the cascades' tiles differ, so one persistent map cannot follow all of them).
At 14 m/s with three cascades the whitecap cover in the bench's sea view went
from 0.01% to 0.18% of the water. (Monahan's fit gives about 3% at that wind;
`foam_threshold` raises it.)

Shore foam, swash and caustics still read cascade 0 only, on purpose: near the
shore the long swell is carried by the shore waves (adding the long FFT
cascades as well would count it twice), and caustics come from the curvature
of the short waves.

### Performance pass

Measured in the cloud (software Vulkan, so GPU numbers are only relative), bench
`docs/bench/water_surface --view=beach --perf --node=wave_cascades:3
--node=shore_waves:true`, with `--gpu-profile` (the water passes are now
labelled: Water ripples, FFT ocean, blits, foam, shore foam, swash, caustics).

- CPU: the node's physics tick takes 0.22 ms with three cascades and shore
  waves; `sample_height()` costs 1.2-1.3 us a call. The shore wave bake runs
  once per seabed measure.
- GPU, share of the frame: the water material's transparent pass is the
  largest (about 90 ms of 244 ms in software), then the FFT (three cascades,
  three times one), then the ripple grid.
- Done: the fragment reads the cascades' derivatives and displacement once
  and shares them between the normal and the foam (they were read twice for
  the whitecaps), and skips the shore waves' extra normal samples where no
  shore wave reaches. The transparent pass went from 91 ms to 69 ms (-25%).
- Fixed: with `step_mode` per frame, the swash never got the shore waves'
  clock.
- Not done: running the larger cascades on 64x64 grids would quarter their FFT
  work, but their shortest waves (a quarter of the first tile) would get only
  2.7 samples per wavelength and lose about 30% of their height in the
  interpolation. On a real GPU three 128x128 FFTs cost a fraction of a
  millisecond, so the quality stays.
- For the PC: `--gpu-profile` on the RTX 4090 for real timings, with
  `mesh_detail` 1 and 3, raster and path traced (the BLAS refit grows with the
  mesh).

## Step 5e: plunging lips and spray (10 Nov)

### Why a rebuild

Step 5c folded the surface itself: the crest was pushed shoreward over the
face. A heightfield mesh can only fold as finely as its cells, so the lip was
a smear at `mesh_detail` 1 and needed 0.1 m cells (detail 3 to 4) to read,
with no air tube and no spray.

### How others do it

- **Thurey, Muller-Fischer, Schirm and Gross (2007)**: find the steep fronts of
  the heightfield as lines, and at each point along them emit a thin sheet
  (cross-sections joined into a ribbon) that follows a ballistic path off the
  crest and lands on the water ahead. The surface stays a heightfield; the lip
  is separate geometry.
- **Ihmsen, Akinci, Akinci and Teschner (2012)**: spray is born where three
  potentials meet: kinetic energy, wave-crest curvature (thin, fast edges)
  and trapped air (impacts). Each is clamped to a range,
  `phi(I) = (min(I, max) - min(I, min)) / (max - min)`, and the product
  sets the droplet rate.
- **Mead and Black (2001)**: the tubes measured at surf breaks are about twice
  as long as they are high when fully curled. That bounds the throw.
- Houdini's FLIP whitewater and Unreal's Water plugin both split it the same
  way: a surface, plus particles where the surface would have to break.

### As built

- Crest lines: marching squares over the ripple grid at the phase where each
  train's crest sits. Each piece runs the same way along its crest.
- Every 0.2 m along a crest line, a cross-section ring of 18 vertices: an outer
  sheet and an inner sheet 9 points along a ballistic path, closed into a tube
  of water. The throw is `w min(0.5 / k, 2 Hc)`, the drop the water height at
  the landing point, so the flight time is `sqrt(2 drop / g)` and the jet
  speed the throw over it. The lip grows over the first 45% of breaking and
  sinks into the bore it lands in.
- The material's face is warped into a bore-like front (the crest at the front
  of the period, the front width `3 amp k / 2 pi` of it), so the lip leaves a
  steep face, and the lip rides 3 cm clear of that face so the two water
  surfaces never cross.
- Spray: the kinetic, crest and trapped-air potentials from Ihmsen, 60 and 220
  droplets per second per metre at the lip edge and the landing, drawn by one
  GPUParticles3D with up to 512 emission points (systematic resampling by
  rate). `shore_spray` and `shore_spray_amount` control it.
- The lip is the built-in water shader compiled with `BREAKER_LIP`: the same
  colours, foam and path traced water medium, white toward the edge.
  `get_breaker_stats()` returns crest points, vertices, triangles, spray rate
  and CPU time.

### Cost

Cloud CPU, lavapipe, the `--view=lip` bench:

| Setting | Frame | Lips CPU |
| --- | --- | --- |
| `mesh_detail` 1 | 175 ms | none |
| `mesh_detail` 1 + lips | 191 ms | 2.4 to 3.9 ms (about 1,100 crest points, 20k vertices) |
| `mesh_detail` 2 | 201 ms | none |
| `mesh_detail` 3 | 230 ms | none |

Lips with the coarse mesh cost less than either finer mesh and show the tube,
which the finer meshes never did. GPU numbers wait for the desktop.

### Path tracing check

The first lips drew dark squares under path tracing only. A front/back face
debug view showed them as back-face hits: where the lip thinned towards
nothing at its ends, the inner sheet met the outer one and a ray picked either.
The sheet now keeps at least 2 cm, which removed them. While chasing it the lip
also lost its alpha test (an alpha-tested material is cut again in the
any-hit pass, which does not see the lip's varyings), got tangents, a
rebuilt (not refitted) BLAS since its topology changes every tick, and
per-triangle winding checked against its normals.

Limits: the lip is one sheet per crest line (no splash-up jet after it
lands) and spray droplets are lit spheres without motion blur.

### Lip build on worker threads (10 Nov)

The audit proposed moving the ring build to a compute pass. Profiling it
first (lavapipe cloud, 4 cores, `--view=lip`, about 1,100 crest points and
20k vertices) showed where the tick went: about 2.2 ms building rings, 1 ms
in the marching squares that find the crest lines, and 0.6 to 1 ms in the
mesh upload. Each ring reads the surface height twice (`sample_height`:
ripple readback, every ocean cascade with its choppy inversion and flow
phases, wakes, shore waves). A compute port would have to duplicate all of
that in GLSL and keep it in step with the CPU path the buoyancy and the
material share, or read the mesh back, so it was not taken.

Instead the marching pass now only lists the live crest pieces and their
slots in the mesh arrays (a prefix sum of their ring and index counts), and
each piece builds its rings straight into its slot on the WorkerThreadPool.
The spray emitters are joined in piece order after. Height samples read the
node's height once on the main thread (`_sample_height_at`), so workers
never touch the scene tree. The output is bit-identical: hashes of the
vertices, normals, tangents, colours, indices and spray totals match the
single-thread build tick for tick.

| Build | Lips CPU per tick (mean) |
|---|---|
| Before (one thread) | 4.6 ms |
| Worker threads | 3.0 to 3.3 ms |

The ring build alone goes from about 2.4 ms to 0.6 to 1.2 ms on 4 cores
that lavapipe also uses; a desktop CPU with more free cores gains more.
What is left is the marching squares and the mesh upload.

### Each wave breaks at its own place (10 Nov)

**Before.** Each shore train was one regular wave: every crest had the same
height, so it shoaled the same way and broke where it reached 0.78 of the
depth. Along any line toward the beach every crest plunged at the same spot.
Only the beat between the two trains varied the surf.

**Published methods.** A real sea's individual wave heights follow a Rayleigh
distribution (Longuet-Higgins 1952), and they come in groups, because a narrow
spectrum is a carrier wave under a slowly varying envelope (Longuet-Higgins
1984). The random-wave surf models are built on that:

- Battjes and Janssen (1978) take the fraction of waves already broken at
  each depth from a Rayleigh distribution clipped at the breaker height.
- Thornton and Guza (1983) weight the Rayleigh distribution by the chance
  that each wave breaks, which matches field data.
- Wave-by-wave models (Mase and Iwagaki 1982) carry each wave on its own and
  break it where its own height reaches the breaker index.

That last approach is the one that fits here, because the surface is drawn
crest by crest.

**What the fork does now.**

- Each train's crests carry a height factor *r* from the envelope of five
  close frequencies around the carrier (spread 0.15 cycles per wave), taken
  as a function of the wave count `theta / 2 pi`. That count travels with the
  crests, so a wave keeps its own height all the way in.
- The envelope's mean square is 1. The individual heights are therefore
  Rayleigh, with `shore_wave_height` as their rms, so the wave energy is the
  same as before. They come in sets: about five bigger-than-rms waves every twelve or so (checked over 20,000 waves: mean r 0.90, 90th percentile 1.51, both as Rayleigh).
- The bake is unchanged. A wave of *r* times the baked height shoals to *r*
  times its unbroken height, so its stage (how far past breaking it is) is
  *r* times the baked stage. Once broken, it is held at the breaker height,
  as Thornton and Guza's saturated surf zone does.
- A bigger wave is steeper offshore, so its surf similarity number is
  divided by sqrt(*r*): big sets spill on the outer bars, and small waves run
  in and plunge closer to the sand.
- The material, `sample_height` (so buoyancy follows) and the lips all
  apply the same function.
- `shore_wave_groups` blends to regular waves. At 0 the surf is the same as
  before.

The run-up is left as it was. At the waterline every wave is already held at
the breaker height, and the swash already runs the crests above the usual
ones further up.

## The camera under water: research (9 Nov)

### What the fork has today

- **Path tracing:** the water is a real medium (refraction, Beer-Lambert, scattering, total internal
  reflection in the dielectric lobe), but raygen starts every camera ray outside any medium. A camera
  under the surface traces its first segment as if in air, then treats the underside as the way
  *into* the water: no fog between the eye and the first surface, and the world above comes out
  dark (absorbed on a path that never happened). A ray that misses everything inside clear water
  is not absorbed either.
- **Raster:** the PhysX water shader is unshaded and alpha blended. From below it draws the
  underside with the same tint as the top. Nothing fogs what is under the water, and the image does
  not split where the surface crosses the lens.

### How others do it

- **Crest (Unity, MIT, code read).** `UnderwaterRenderer` draws the water surface into a mask
  (front faces above, back faces below) plus a horizon line from the camera's far plane against the
  sea level, so each pixel knows if it is under water. A full screen pass fogs those pixels:
  `lerp(scene, scatter_colour, 1 - exp(-density * distance))` with a density per channel; the scatter
  colour comes from the sun and ambient light at the camera's depth. The surface seen from below is
  shaded by the ocean shader itself: Fresnel worked out on the air side through Snell's law, so it
  reaches 1 past the critical angle (total internal reflection), and the reflected part is the
  scatter colour. A meniscus darkens three pixels each side of the mask's edge across the lens.
- **Unity HDRP water.** A full screen pass with an absorption distance and a scattering colour; if
  volumetric fog is on the water goes into the volumetric buffer and gets light shafts from shadows.
  The water line across the lens is drawn from a per pixel "is underwater" buffer that custom passes
  can read. Docs do not mention total internal reflection.
- **Unreal Water.** Each water body has an underwater post process material, switched on when the
  camera is inside the body (the water height under the camera). It fogs with the Single Layer Water
  material's absorption and scattering. Splitting the image at the water line is left to the
  material.
- **Godot add-ons (code read).** tessarakkt's godot4-oceanfft checks the camera against the wave
  height and swaps in a volumetric fog (one grey density, so no colour shift with distance) and a
  full screen tint; no water line. Boujie Water Shader uses a FogVolume. Ben K's waterline shader
  (0BSD) is a full screen quad that compares each pixel's near plane point with the water height,
  and for a displaced surface rebuilds the exact triangle under the point, since a bilinear read of
  the height map misses the creases of the mesh.
- **Offline path tracers.** PBRT and Mitsuba make the camera's medium a scene setting. Cycles
  finds it: before the camera rays it casts a probe ray and keeps a volume stack from the faces it
  crosses (front face in, back face out), so a camera that dips into a volume needs no setup.

### Plan

- **Path tracing, Cycles' way.** Each pixel probes straight up from its point on the near plane
  (the same plane raster clips at, so the water line splits the lens in the same place). The first
  medium surface it meets decides: its back face means the pixel starts inside that medium, with
  that surface's extinction and scattering; a front face or nothing means air. Other surfaces (a
  pier, a boat's hull) are passed through. Camera rays then start at the near plane. Misses inside
  clear water are absorbed. One probe a pixel a frame, only with refraction on.
- **Raster, Crest's way, with the path tracer's numbers.** The fog and the underside use the
  water's own `pathtraced_absorption` and `pathtraced_scattering`, so both modes agree:
  - A full screen pass under the water fogs what is behind each pixel: transmittance
    `exp(-sigma_t d)` per channel, plus single scattering of the sun (Henyey-Greenstein phase with the
    water's anisotropy, the sun refracted at the surface) and the sky, each dimmed by the depth it
    crossed, in closed form along the ray. Surfaces under water lose the light the water took on
    the way down.
  - Per pixel near plane test against the displaced surface (the same heights the mesh uses, with
    the choppy offset inverted), so the image splits at the water line, with a thin meniscus.
  - The underside: refraction of the screen up through the surface (Snell's window), exact
    dielectric Fresnel from the water side reaching 1 at 48.6 degrees, the reflected part the
    water's own in-scattered light, and the fog of the distance to it.
  - Only when the camera is near or under the water, so above water costs nothing.
- **Reuse:** the surface heights, normals and foam from `PhysXWaterSurface3D`'s shader, its
  medium parameters, and the path tracer's medium tracking from step 1.
- **Not in this step:** raster light shafts (the path tracer has them), raster caustics on the
  floor, fogging transparent objects under water in raster (Crest needs a per-material fog for that
  too), underwater bubbles and particles.

## The camera under water: as built (9 Nov)

- **Path tracing.** Raygen starts each pixel's camera ray on the near plane (where raster clips) and
  first calls `rt_camera_medium()`: a probe straight up through the caustic photon mode of the hit
  shader, which now reports a medium surface's back face with its medium code (sign bit set, so
  photons still read it as not entering) and leaves its scattering in `rt_medium_image`. The first
  medium surface met decides; other surfaces are passed through (up to 4). The probe starts 1 cm
  below the point, so a surface within the trace's 1 mm minimum distance is not skipped (it left a
  band of unfogged floor along the water line). The path then starts in
  that medium, so fog, scattering, the surface from below (refraction, Fresnel, total internal
  reflection) and shadows out of the water all come from the existing medium tracking. A camera ray
  that misses everything inside clear water is now absorbed (black) instead of seeing the sky. Only
  with `rendering/pathtracing/refraction` on.
- **Raster.** `PhysXWaterSurface3D.underwater_enabled` (on) and `underwater_meniscus` (0.35). While
  the camera is within reach of the waves the node shows two more materials built from the water
  shader's own source (`#define UNDERWATER_FOG`, `UNDERWATER_SURFACE`), copying its parameters each
  frame:
  - *Fog pass:* a full screen quad (2 mm mesh, so the path tracer has nothing in its way), drawn
    first among transparents. Per pixel: the near plane point against the displaced surface
    (`surface_height_world()`, choppy offset inverted) splits the image; the meniscus darkens a two
    pixel band there; just above the line, looking down, the part of the ray under the water is
    fogged too (the mesh's straight triangles and the filtered heights disagree by millimetres
    there, Ben K's crease problem). Under the water: `scene x light_at_depth x exp(-sigma_t d) + in-scatter`, d
    clipped where the ray reaches the surface. In-scatter is single scattering in closed form along
    the ray: the sun refracted into the water (Snell, IOR 1.33, Fresnel transmission, the first
    visible DirectionalLight3D) with the Henyey-Greenstein phase of `pathtraced_scattering_anisotropy`,
    dimmed by `exp(-sigma_t z / mu)`; the sky (the environment's irradiance from above, baked with
    `sky_bake_panorama` every few seconds) isotropic, dimmed by `exp(-1.2 sigma_t z)`.
  - *Underside pass* (the water material's next pass): the screen read along the refracted ray
    (Snell's window), exact Fresnel from the water side (1 past 48.6 degrees), the mirror below as a
    16 step screen-space march (fogged by its own distance), falling back to the water's in-scatter;
    foam as a lit, rough layer; then the fog of the distance to the camera.
  - The plain surface pass skips its underside while the camera is under (`underwater_camera_below`
    from `sample_height()` at the camera).
- **Bench.** `docs/bench/water_surface --view=submerged|waterline|up` (slope 12 degrees by default
  there; waterline with 0.3 m waves), `--cam-dy=M`, and `--dive=M --seq=N` to lower the camera through
  the surface over a sequence.
- **Compared (960x540, 16 spp, 16 to 32 frames):** water column and rock within about 10% between
  raster and path tracing. Raster's floor is darker and has no caustics; its mirror past the
  critical angle only shows what is on screen. Shots: /mnt/project-files/fork/water/underwater/.
- **Not built:** raster light shafts, raster caustics, fog on transparent objects under water in
  raster, bubbles. With several cameras the node follows the active one.

## Step 5f: directional spreading and a shared-memory FFT (10 Nov, audit item 5)

### Problem

- The spectrum spread its energy about the wind as `cos^2 theta` (and a 4% back lobe), the same at every
  wavelength. Measured seas are narrowest at the spectrum's peak and wider above and below it, so the
  dominant waves run with the wind while short waves cross at wide angles. At 14 m/s our sea's slopes were
  2.7 times steeper along the wind than across it; Cox and Munk (1954, sun glitter) measured about 1.5.
- No way to ask for swell: long, nearly parallel crests from a storm far away.
- Each cascade's FFT ran one dispatch per butterfly stage for each of its four transforms: 8 log2(N)
  dispatches a step, through global memory each time (48 at N = 64, 64 at N = 256), times the cascades.

### Published methods

- **Horvath, "Empirical directional wave spectra for computer graphics" (DigiPro 2015), section 6.**
  Compares cos^2, Mitsuyasu, Hasselmann (both `Q(s) |cos(theta/2)|^2s` with a measured `s(w)`) and
  Donelan-Banner (`b / (2 tanh(b pi)) sech^2(b theta)`, `b(w / wp)` piecewise; he uses the first branch
  below 0.56 wp too). His default: TMA spectrum and Donelan-Banner spreading. Swell (section 6.1): the
  spreading times `Q(s) |cos(theta/2)|^2s` with `s = 16 tanh(wp / w) swell^2`, normalised again (a
  quadrature for Donelan-Banner; for Hasselmann the exponents add, closed form).
- **FFT on the GPU:** a line's whole radix-2 Stockham FFT in one workgroup's shared memory, log2(N)
  stages with barriers, several transforms batched in one dispatch (Flügge 2017, "Realtime GPGPU FFT ocean
  water simulation"; GodotOceanWaves; Crest and Atlas batch all cascades in a texture array).

### As built

- `PhysXWaterSurface3D.wave_spreading`: Donelan-Banner (default), Hasselmann, or Cos2 (the old one,
  kept as the opt-out). `swell` 0..1 for the first two. The spectrum init evaluates them per mode, the
  swell product normalised by a 64-point quadrature; regenerated in place like a wind change.
- `water_fft_rows.glsl`: rows then columns, all four transforms of a step along z: 2 dispatches a step
  per cascade. Same butterflies as `water_fft.glsl`, so the field is the same: the height map and the
  rendered frame are bit-identical to the per-stage passes. Up to N = 1024 (shared memory); larger grids,
  and the project setting `physics/physx_3d/water/shared_memory_fft` off (the default until a GPU timing), take the per-stage passes.

### Results (lavapipe, `docs/bench/water_surface --view=sea_top --height-out`, 3 cascades, 14 m/s)

| Spreading | Hs | slope variance z / x (wind mostly along z) |
|---|---|---|
| Cos2 (before) | 1.13 m | 2.72 |
| Donelan-Banner | 1.09 m | 1.41 |
| Hasselmann | 1.12 m | 1.17 |
| Donelan-Banner, swell 0.8 | 1.09 m | 2.55 |

Cox and Munk's upwind / crosswind slope variance at 14 m/s is 1.48: Donelan-Banner matches it. The
wave height is unchanged (the spreading only moves energy between directions).

FFT cost: lavapipe runs workgroups on CPU threads, where 256-thread barriers are costly: 247 against
210 ms a frame at N = 256 with 3 cascades. That says nothing about a GPU, where this layout is the
standard fast one (64 dispatches a step become 2). So the setting ships OFF, and goes on by default once
the PC times it faster (PC queue).

## Step 5g: whitecap cover from the wind (10 Nov, audit item 6)

### Problem

Whitecaps came from a fixed Jacobian threshold (`foam_threshold`, 0.7). How much of the sea that whitens
depends on the spectrum, the choppiness, the cascades, the fetch and now the spreading, not on the wind
alone, so the same threshold gave 0.18% cover at 14 m/s with three cascades (step 5d) where the sea shows
about 3%, and changing any wave setting changed the foam with it.

### Published methods

- **Monahan and O'Muircheartaigh, "Optimal power-law description of oceanic whitecap coverage dependence
  on wind speed" (J. Phys. Oceanogr. 10, 1980).** From open-ocean photographs: cover
  `W = 3.84e-6 U10^3.41` (fraction): 0.1% at 6 m/s, 1% at 10 m/s, 3% at 14 m/s, 8% at 20 m/s. Later
  surveys (Anguelova and Webster 2006, Callaghan et al. 2008) keep the cubic law with a spread of about a
  factor of 3 at one wind (sea temperature, a rising or falling sea), and a threshold near 3-4 m/s.
- **Dupuy and Bruneton, "Real-time animation and rendering of ocean whitecaps" (SIGGRAPH Asia 2012).**
  Whitecaps where the Jacobian of the choppy displacement falls below a threshold, which they set from
  the Jacobian's statistics so the cover matches the measured coverage at that wind.
- Engines: Crest and UE water take a fixed foam threshold or coverage slider; HDRP's water reads the
  simulation's foam with a wind-speed-dependent amount curve, artist set.

### As built

- The foam pass sums its foam over the grid (one atomic per workgroup), read back each step with the
  height. The cascades draw their foam over each other (max), so the share left clear is the product of
  each one's: `cover = 1 - prod(1 - c_i)`.
- `PhysXWaterSurface3D.whitecap_auto` (on by default) steers the threshold each step, an integral
  controller in log cover (the cover spans decades with the wind, and a threshold step changes it by a
  factor): `threshold -= 0.08 ln(cover / W) dt`. The same threshold goes to every cascade's foam pass and
  the material's summed-crest test. It holds Monahan's cover whatever the waves' settings, and follows
  the wind as the weather changes it. Below about 3 m/s the target is under 0.01%, and the threshold
  runs down until the sea is clear.
- `whitecap_cover_scale` (1) scales the target, inside the measured spread. `whitecap_auto` off keeps the
  fixed `foam_threshold` (the opt-out); with it on, `foam_threshold` is the starting point.
- `get_whitecap_cover()`, `get_whitecap_target_cover()`, `get_effective_foam_threshold()` for a bench.

### Results (lavapipe, `docs/bench/water_surface --view=sea_top --whitecaps=4,...,20`, 15 s a wind, mean of the last 7.5 s)

Share of the sea under foam (mean foam on the grids; the material's lace erosion keeps the mean):

| U (m/s) | Monahan | fixed 0.7, 3 cascades (before) | auto, 3 cascades | auto, choppiness 1.5 | fixed 0.7, 1 cascade |
|---|---|---|---|---|---|
| 4 | 0.04% | 0.10% | 0.05% | 0.01% | 0.31% |
| 6 | 0.17% | 0.30% | 0.18% | 0.18% | 0.68% |
| 8 | 0.46% | 0.56% | 0.49% | 0.47% | 1.37% |
| 10 | 0.99% | 0.96% | 0.99% | 0.98% | 1.99% |
| 12 | 1.84% | 1.29% | 1.85% | 1.83% | 2.63% |
| 14 | 3.11% | 1.67% | 3.12% | 3.11% | 3.77% |
| 17 | 6.03% | 2.36% | 6.05% | 5.98% | 5.20% |
| 20 | 10.5% | 3.03% | 10.4% | 10.3% | 6.21% |

The fixed threshold grew the cover 30 times from 4 to 20 m/s where the sea grows it 240 times, and
moved with the cascades and the choppiness; steered, the cover is on Monahan's curve within 1-8% from
6 m/s up (4 m/s still settling from a far start at choppiness 1.5). The threshold stays between 0.68 and
0.77 with three cascades (0.54-0.64 at choppiness 1.5). Cost: one atomic per 64 texels in the foam
pass and a 4-byte readback with the existing ones.

## Step 5h: dispersive boat wakes with the Kelvin pattern (11 Nov, audit item 9)

### Problem

The wake grid that travels with a boat (`PhysXWaterWake3D`, `water_wake.glsl`) runs the plain wave
equation at one speed (`wave_speed`, 2.2 m/s). Every wavelength travels at that speed, so a moving hull
makes a Mach cone: a V whose angle changes with the boat's speed, with no transverse waves behind the
stern and no feathered diverging waves on its arms. Real deep water is dispersive (`omega^2 = g k`: long
waves are faster), and that alone makes the Kelvin pattern: a wedge of half-angle 19.47 degrees at any
speed (Thomson 1887), transverse waves of wavelength `2 pi U^2 / g` inside it and short diverging waves
on its edges, narrowing at high hull Froude numbers (Rabaud and Moisy 2013).

### Published methods

- **Water Surface Wavelets (Jeschke et al., SIGGRAPH 2018)**, the audit's suggestion. Wave amplitude on
  a 4D grid (position x direction x wavenumber), advected at the group speed, the height rebuilt from a
  precomputed profile buffer. It carries waves over kilometres at about 8 ms for 4096^2 x 16 on a
  laptop GPU. Its phases are random per direction, so it cannot build patterns made by interference:
  the paper says wake patterns that depend on "constructive interference between coherent phases" are
  impractical with it. The Kelvin pattern is one, so it does not give what the audit promised. Not used.
- **Water Wave Packets (Jeschke and Wojtan, SIGGRAPH 2017).** Lagrangian packets with exact
  dispersion: Kelvin wakes come out right, but a boat wake takes millions of packets (0.5-2 fps in the
  paper). Too slow for a game.
- **iWave (Tessendorf 2004)** and **dispersion kernels (Canabal et al., SIGGRAPH Asia 2016).** A height
  grid with the vertical-derivative operator `sqrt(-laplacian)` as a convolution kernel, so the grid
  has deep-water dispersion and moving sources leave Kelvin wakes. A short kernel gets long waves wrong
  (its transform is smooth at k = 0 where `|k|` has a cusp), which is what Canabal et al. fix with
  fitted kernels.
- **eWave (Tessendorf 2014)**, the same equation stepped in Fourier space with an exponential
  integrator: every mode is an exact oscillator at `omega = sqrt(g k)`, so dispersion is exact at every
  wavelength the grid holds and the step is unconditionally stable. It costs two FFTs a step. A moving
  pressure patch on it is the classic Havelock problem, whose linear answer is the Kelvin wake.
- Engines: UE water, Crest and HDRP draw boat wakes as foam trails, decals or a non-dispersive ripple
  sim; Crest's dynamic waves are the wave equation at one speed, like ours was.

### Plan

eWave on the wake grid, keeping everything else the grid does (it slides with the boat, foam, the hull
bands, propeller wash, breaking):

1. The hull is a pressure head `d(x)` (its dip, as before). The surface oscillates about `-d`:
   `h_tt = -g sqrt(-laplacian) (h + d)`. A hull at rest still sits in its dip; a moving one radiates the
   Kelvin pattern. No more pulling the height toward the dip.
2. Each step: a spatial pass shifts the state as the grid slides and packs `z = (h + d) + i v`; one
   complex FFT (rows, then columns, the shared-memory shader the ocean uses); a mode pass splits `H`
   and `V` by Hermitian symmetry and turns each mode by `omega dt`; the inverse FFT; a spatial pass
   takes `h = Re - d`, damps a border band (the grid is periodic in Fourier space; the band keeps waves
   from wrapping), and does the foam and breaking as before.
3. `dispersive` (on by default) is the opt-out; off keeps the wave equation. With it on the grid side
   is a power of two (192 cells rounds up to 256, the size growing to keep the cell size), and
   `wave_speed` is unused.
4. Bench: a boat crossing calm sea at 4, 6 and 10 m/s, the wake heights read back from above; measure
   the wedge angle against 19.47 degrees and the transverse wavelength against `2 pi U^2 / g`.

### As built

As planned, with three things learned on the bench:

- The exact step loses no energy on its own, so the wave equation's damping (0.25/s) took the transverse
  waves out 30 m behind the boat. With `dispersive` an eighth of `damping` applies.
- Dispersive waves near the bow are steep (slope 0.3-0.4) and the old foam threshold (slope 0.08) turned
  the whole wedge white. `breaking_slope` (0.3, where whitecapping starts; Stokes' limit is about 0.44)
  replaces `foam_slope` with it on: the bow waves break, the Kelvin waves stay glassy.
- The grid is periodic in Fourier space; the border band is 15% of the side (10% before) so long waves
  are gone before they wrap.

The step is pass 1 (slide, foam, the dip, pack) + row/column FFT + mode pass + row/column FFT + pass 2,
each its own compute list (a barrier inside one list re-pushes the last push constant onto the FFT
pipeline, which has none). 256^2 by default.

### Results (lavapipe, `docs/bench/water_surface --boat=U --wake-out`, an 8 m x 2.4 m hull, 15 s in)

Wedge half-angle: the outermost offset where the wave envelope still reaches a quarter of its peak,
median over 16-48 m behind the bow. Transverse wavelength: zero up-crossings along the track.

| U (m/s) | hull Froude | wedge, wave equation (before) | wedge, dispersive | transverse wavelength, dispersive (theory) |
|---|---|---|---|---|
| 4 | 0.45 | 32.2 | 21.8 | 9.0 m (10.2) |
| 6 | 0.68 | 21.4 | 19.5 | 23.8 m (23.1) |
| 10 | 1.13 | 12.9 | 12.3 | longer than the grid (64.0) |

Before, the wedge was a Mach cone (`asin(2.2 / U)`: 33 degrees at 4 m/s, 13 at 10), filled with a flat
raised plateau and no transverse waves. Now it holds near Kelvin's 19.5 degrees up to a hull Froude
number of about 0.7, with transverse waves of the right length, then narrows as ships' wakes do at
high Froude numbers (Rabaud and Moisy 2013: the angle falls about as 1 / Froude past 0.6). The path traced frame
shows the waves in the sun's glitter path (no denoiser here). The raster frame does not show a glitter
path on this bench's sea at all, wake or not: a raster sun-specular question for later, not the wake.

Cost: two 256^2 complex FFTs and three 256^2 passes a step, about what one ocean cascade's FFT costs
(the GPU timing goes on the PC queue). The CPU side is unchanged.


## Step 5i: raster reflections from above (11 Nov, bug)

The raster surface seen from above was `unshaded`: a tint shaped by a fixed pseudo-light, with no sky and
no sun in it (an old fix for blown-out blending, when the surface was lit as if opaque and then blended
over the lit floor). So a raster sea showed no glitter path and no sky at grazing angles, where the path
tracer shows both, and boat wakes were nearly invisible in raster.

Now, still unshaded and still blended (so the old blow-out cannot come back), the surface adds what it
reflects and lets through only the rest: `out = old (1 - F) + F sky(r) + sun`, written for blend_mix.

- `F`: the exact unpolarized Fresnel term, air into water (2% looking down, most of the light at grazing).
- Sky: a 128 x 64 radiance panorama of the environment's sky (`sky_bake_panorama`), baked again every
  four seconds, read along the reflected ray (turned up to the horizon where a steep wave points it down).
- Sun: a GGX lobe at `pathtraced_roughness` (Bruneton, Neyret and Holzschuch 2010 light the sea with a
  microfacet sun the same way), its roughness widened by how fast the normal turns across the pixel
  (geometric specular antialiasing, Tokuyoshi and Kaplanyan 2019): close up the glints are sharp, far
  out they merge into a glitter path instead of flickering.
- None on foam or dry sand. `PhysXWaterSurface3D.raster_reflections` (on) is the opt-out.

Rivers (`PhysXRiver3D`) had the same unlit raster look and now share the code
(`water_raster_reflections.h`: the GLSL and the sun and sky lookup), with their own `raster_reflections`.
Waterfall sheets keep the tint.

Unresolved: raster cloud shadows do not dim the glitter (an unshaded material does not read the light's
shadow); the path tracer's do.

## Step 5j: multigrid flow bakes (11 Nov, audit item 10)

The flow bakes solve a stream function: `div(grad(psi) / h) = 0` over the sea or lake's simulated square
(the current round islands and along shores) and `div(grad(psi) / K)` with the bend metric over a river
(round rocks). Both used SOR, which needs a few hundred to a few thousand sweeps and grows with the grid
squared. Now both call `FlowPoisson::solve` (`water/flow_poisson.h`): conjugate gradients in double,
preconditioned by one symmetric multigrid V-cycle (McAdams, Sifakis and Teran 2010; the cycle after
Briggs, Henson and McCormick, A Multigrid Tutorial).

- Levels: 2 x 2 aggregates with the Galerkin coarse operator, which on this stencil is just "add up the
  conductances crossing between aggregates, drop those inside one from the diagonal". Banks, fixed edges
  and dry ground fall out on their own (no conductance), so no geometric special cases.
- Unsmoothed aggregation undershoots the smooth error, so the coarse correction is scaled by 1.8 (the
  over-correction of Braess 1995): CG iterations 30 -> 14 at 320 x 320. One forward Gauss-Seidel sweep
  before, one backward after, so the preconditioner stays symmetric. Levels in float, CG in double.
- Islands (a rock, an islet: one unknown psi set so no net flow goes round it) are extra unknowns
  coupled to their wet neighbours, preconditioned with Jacobi beside the V-cycle.
- Rivers: the strip equation is now in its symmetric finite-volume form (along a face K_f dn_f / (h_f
  ds), across K_f h_f ds / dn, the metric h at the face rather than at the cell), which CG needs. It
  shifts the flow round rocks on a bend by up to 0.07 m/s of 3.8 (mean 0.01); the old form was not
  conservative there.
- `physics/physx_3d/water/multigrid_flow` (on) is the opt-out, back to SOR.

Timings (cloud CPU, `docs/bench/water_surface --current=0.6,0.2 --bake-time=N`, whole bake):

| Grid | SOR | Multigrid |
|---|---|---|
| 160 x 160 | 62 ms | 10 ms |
| 320 x 320 | 515 ms | 50 ms |
| River bench, 25 x 201 (`--bake-time`) | solve about 6 ms, rebuild 12 ms | solve 1.5 ms, rebuild 7.5 ms |

The sea's flow is the same to 0.4 mm/s (fastest 2.1 m/s), since the equation did not change there.

## Step 5k: waterfall mist on every build (11 Nov, audit item 11)

Mist only existed with NVIDIA Flow: each fall fed a `PhysXFlowEmitter3D`, which does nothing without a
`PhysXFlow3D` in the scene, so the bench, the showcase and every build without Flow had spray and no mist.

Now `PhysXWaterMist3D`: the finest drops (radius about 20 um) as a small grid of liquid water content
(g/m^3) over a world-aligned box, stepped on the CPU at 15 Hz.

- Physics per drop size: Stokes settling, 2/9 rho_w g r^2 / mu (4.8 cm/s at 20 um); evaporation time
  r^2 / (2 G (1 - RH)) from the diffusional growth law r dr/dt = G (S - 1), G about 1e-10 m^2/s (Rogers
  and Yau, A Short Course in Cloud Physics, ch. 7): 13 s at 85%, a minute at 97%, humidity from the
  WeatherMap3D; extinction 3 Q LWC / (4 rho_w r) with Q about 2 (0.075 /m per g/m^3 at 20 um).
- Air: the spray wind a fall drags down leaves the impact as a wall jet (Glauert 1956: thickening about
  0.1 r, slowing as it spreads), at about 0.3 of the impact speed, never back into the cliff, with some of
  it rising near the impact (the billow up the fall's face); plus the WeatherMap3D's wind.
- Steps: semi-Lagrangian advection (Stam, Stable Fluids, 1999), explicit turbulent diffusion (0.5 m^2/s),
  evaporation, the source. Parallel over z slabs.
- Source: a share of the fall's water that grows with the square of the impact speed (atomisation by the
  jet), `1e-5 (v / 10 m/s)^2` of the mass flow, `fall_mist_amount` to scale it.
- Raster: a FogVolume over the box with the grid as its density texture (needs the Environment's
  volumetric fog; density 0 and ambient inject 1 light it with the sky and add no fog of its own).
- Path tracing: RTVolumes gained dense boxes (up to five, one atlas, density in r; `emission.w` holds the
  count and switches `rt_volume_fetch` to them), traced by the same delta tracking and ratio tracking as
  Flow's gas (Woodcock; Novak et al. 2014), albedo 0.999, Henyey-Greenstein g 0.85 for drops this size.
  A frame where a PhysXFlow3D publishes its gas traces that instead (one medium a frame).
- A scene with a PhysXFlow3D keeps the Flow emitter (a full fluid solve) instead of the grid.

River bench `--falls --view=fall --mist` (6 m fall, 40 x 20 x 40 cells): step 0.46 ms + texture 0.17 ms
of CPU, 15 times a second (about 0.3 ms a frame at 30 fps). Near the foot about 2.7 g/m^3, 0.7 five
metres up. Raster and path tracing agree to about a third on how much the mist brightens the far bank
(+9 and +14 grey levels): Godot's froxel fog is coarser. GPU cost of the traced mist: PC queue.

Not done: rainbows (a Mie lobe at 138 degrees needs a spectral phase function; HG has none).

## Sources

- Stockdon, Holman, Howd and Sallenger, Empirical parameterization of setup, swash, and runup, Coastal Engineering 53 (2006).
- Battjes, Surf similarity, Coastal Engineering Proceedings (1974).
- Fournier and Reeves, A simple model of ocean waves, SIGGRAPH 1986.
- Malan, Water in Horizon Forbidden West, SIGGRAPH Advances 2022.
- Thurey, Muller-Fischer, Schirm and Gross, Real-time breaking waves for shallow water simulations, Pacific Graphics 2007.
- Mead and Black, Predicting the breaking intensity of surfing waves, Journal of Coastal Research SI 29 (2001).

- Chentanez and Müller, Real-time Simulation of Large Bodies of Water with Small Scale Details, SCA
  2010: https://matthias-research.github.io/pages/publications/hfFluid.pdf
- Chentanez, Müller and Kim, Coupling 3D Eulerian, Heightfield and Particle Methods for Interactive
  Simulation of Large Scale Liquid Phenomena, SCA 2014:
  https://matthias-research.github.io/pages/publications/hybridsim_preprinted.pdf
- Solenthaler et al., SPH Based Shallow Water Simulation, VRIPHYS 2011:
  https://matthias-research.github.io/pages/publications/SPHShallow.pdf
- Ihmsen, Akinci, Akinci and Teschner, Unified Spray, Foam and Bubbles for Particle-Based Fluids,
  The Visual Computer 28 (2012)
- Chanson, air entrainment in hydraulic jumps (University of Queensland reports):
  https://core.ac.uk/download/15116299.pdf
- Fedkiw, Stam and Jensen, Visual Simulation of Smoke, SIGGRAPH 2001 (monotone cubic interpolation
  in semi-Lagrangian advection): https://web.stanford.edu/class/cs237d/smoke.pdf
- Fluid Flux (Unreal shallow-water rivers): https://80.lv/articles/fluid-flux-a-cool-water-simulation-system-for-unreal-engine/

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
- Vlachos, Water Flow in Portal 2, SIGGRAPH 2010:
  https://www.advances.realtimerendering.com/s2010/Vlachos-Waterflow(SIGGRAPH%202010%20Advanced%20RealTime%20Rendering%20Course).pdf
- Grimes, Making and Using Non-Standard Textures, GDC 2011:
  https://www.hlportal.de/files/GDC2011_BronwenGrimes_NonstandardTextures.pdf
- Flick, Texture Distortion (Catlike Coding): https://catlikecoding.com/unity/tutorials/flow/texture-distortion/
- Mikkelsen, Practical Real-Time Hex-Tiling, JCGT 2022: https://jcgt.org/published/0011/03/05/paper.pdf
- Burley, On Histogram-preserving Blending for Randomized Texture Tiling, JCGT 2019:
  https://www.jcgt.org/published/0008/04/02/paper.pdf
- Yu, Neyret, Bruneton, Holzschuch, Scalable Real-Time Animation of Rivers, 2009:
  https://artis.inrialpes.fr/Publications/2009/YNBH09/ ; Proland river plugin:
  https://proland.inrialpes.fr/doc/proland-4.0/river/html/index.html
- Bousseau et al., Video Watercolorization using Bidirectional Texture Advection, 2007:
  https://core.ac.uk/download/50362841.pdf
- Jeschke et al., Water Surface Wavelets, 2018:
  https://pub.ista.ac.at/group_wojtan/projects/2018_Jeschke_WaterSurfaceWavelets/WaterSurfaceWavelets.pdf
- Unreal water spline metadata:
  https://docs.unrealengine.com/4.27/API/Plugins/Water/UWaterSplineMetadata/index.html ;
  r.Water.MaxFlowVelocity: https://indxzero.github.io/ue544cvarwiki/articles/r.water.maxflowvelocity
- Unity HDRP current map:
  https://docs.unity3d.com/Packages/com.unity.render-pipelines.high-definition@17.2/manual/water-create-a-current-in-the-water-system
- Crest flow: https://docs.crest.waveharmonic.com/Manual/Simulation/Flow.html
- Arnklit, Waterways (Godot): https://github.com/Arnklit/Waterways
- tessarakkt, godot4-oceanfft (probe buoyancy): https://github.com/tessarakkt/godot4-oceanfft
- Simon Schreibt, River 2k19 (Horizon-style Houdini river): https://simonschreibt.de/gat/river ;
  80.lv, Horizon Zero Dawn procedural rivers: https://80.lv/articles/horizon-zero-dawn-procedural-rivers-wires/
- Ang, The Technical Art of Sea of Thieves, SIGGRAPH 2018:
  https://history.siggraph.org/wp-content/uploads/2022/09/2018-Talks-Ang_The-Technical-Art-of-Sea-of-Thieves.pdf
- Kerner, Water interaction model for boats in video games (Just Cause 3):
  https://gamedeveloper.com/programming/water-interaction-model-for-boats-in-video-games
- Arc Blanc, real-time ocean and solid coupling, 2025: https://www.arxiv.org/pdf/2503.03326
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
- Epic, Water body actors (river spline, landscape carving):
  https://dev.epicgames.com/documentation/unreal-engine/water-body-actors-in-unreal-engine
- Crest, water bodies and water inputs: https://crest.readthedocs.io/en/4.20/user/water-bodies.html ;
  https://crest.readthedocs.io/en/4.20/user/water-inputs.html
- BeamNG river editor: https://documentation.beamng.com/world_editor/tools/river_editor/
- Torque 3D river editor:
  https://docs.torque3d.org/for-designers/editors/world-editor/spline-based-tools/river-editor
- Peytavie, Dupont, Guérin, Cortial, Benes, Gain, Galin, Procedural Riverscapes, CGF 2019:
  https://www.cs.purdue.edu/homes/bbenes/papers/Peytavie19CGF.pdf
- Génevaux, Galin, Guérin, Peytavie, Benes, Terrain Generation Using Procedural Models Based on
  Hydrology, ACM ToG 2013: https://www.cs.purdue.edu/homes/bbenes/papers/Genevaux13ToG.pdf
- Kipfer and Westermann, Realistic and Interactive Simulation of Rivers, Graphics Interface 2006.
- Chow, Open-Channel Hydraulics, McGraw-Hill 1959 (Manning's equation, divided-channel method).
- Leopold and Maddock, The Hydraulic Geometry of Stream Channels, USGS Professional Paper 252, 1953.
- Rosgen, A Classification of Natural Rivers, Catena 22, 1994.
- Dunne and Leopold, Water in Environmental Planning, 1978.
- Crest underwater (MIT): https://github.com/wave-harmonic/crest (UnderwaterRenderer,
  UnderwaterEffectShared.hlsl, OceanReflection.hlsl ApplyReflectionUnderwater)
- Unity HDRP, underwater view:
  https://docs.unity3d.com/Packages/com.unity.render-pipelines.high-definition@17.0/manual/water-underwater-view.html
- Epic, FUnderwaterPostProcessSettings:
  https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Plugins/Water/FUnderwaterPostProcessSettings
- tessarakkt, godot4-oceanfft OceanEnvironment.gd: https://github.com/tessarakkt/godot4-oceanfft
- Boujie Water Shader: https://github.com/Chrisknyfe/boujie_water_shader
- Ben K, Godot waterline shader: https://mcspider.codeberg.page/godot-waterline-shader.html
- Blender Cycles, volume stack initialisation from the camera (kernel/integrator/volume_stack.h).
- Pharr, Jakob, Humphreys, PBRT 4th ed., 11.3 Media (camera medium).
- Rouse, Discharge characteristics of the free overfall, Civil Engineering 6, 1936; Chanson, The Hydraulics
  of Open Channel Flow, 2nd ed., 2004 (free overfall, nappe flow).
- Ervine and Falvey, Behaviour of turbulent water jets in the atmosphere and in plunge pools, Proc. ICE 83,
  1987; Ervine, Falvey and Withers, Pressure fluctuations on plunge pool floors, J. Hydraulic Research 35, 1997.
- Castillo, Turbulence and break-up length of rectangular falling jets (Horeni 1956 `L_b = 6 q^0.32`),
  IAHR 2006; Carrillo, Castillo et al., Water 13(11) 1593, 2021: https://www.mdpi.com/2073-4441/13/11/1593
- Bagar, Scherzer and Wimmer, A Layered Particle-Based Fluid Model for Real-Time Rendering of Water, EGSR 2010:
  https://www.cg.tuwien.ac.at/research/publications/2010/bagar2010/bagar2010-draft.pdf
- Malan, Real-time water in Horizon Forbidden West, SIGGRAPH 2022 Advances:
  https://advances.realtimerendering.com/s2022/SIGGRAPH2022-Advances-Water-Malan.pdf
- Cox and Munk, Measurement of the roughness of the sea surface from photographs of the sun's glitter, JOSA 44, 1954.
- Flügge, Realtime GPGPU FFT ocean water simulation, TU Hamburg-Harburg, 2017.
- Donelan, Hamilton and Hui, Directional spectra of wind-generated waves, Phil. Trans. R. Soc. A 315, 1985.
- Hasselmann, Dunckel and Ewing, Directional wave spectra observed during JONSWAP 1973, J. Phys. Oceanogr. 10, 1980.
- Horvath, Empirical directional wave spectra for computer graphics, DigiPro 2015:
  https://dl.acm.org/doi/10.1145/2791261.2791267
- Bouws, Guenther, Rosenthal and Vincent, Similarity of the wind wave spectrum in finite depth water (TMA),
  J. Geophys. Res. 90, 1985.
- Unity HDRP Water System, simulation bands: https://docs.unity3d.com/Packages/com.unity.render-pipelines.high-definition@17.0/manual/water-water-system-simulation.html
- Ang, Catling et al., The Technical Art of Sea of Thieves, SIGGRAPH 2018 Talks.
- Fenton and McKee, On calculating the lengths of water waves, Coastal Engineering 14, 1990.
- Zhao, A fast sweeping method for eikonal equations, Math. Comp. 74, 2005.
- McCowan, On the highest wave of permanent type, Phil. Mag. 38, 1894; Dean and Dalrymple, Water Wave Mechanics
  for Engineers and Scientists (shoaling, refraction, breaking).
