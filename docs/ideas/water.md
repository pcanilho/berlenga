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

**Raster lighting (11 Nov).** The raster sheet and the river's white water
were drawn unshaded with a fixed light direction, so a cascade or a fall in a
shaded valley glowed flat white (pixels about 194 on a dark hillside) while the
path tracer lit it properly. Foam is a rough diffuse scatterer, so both now
take the scene's lights, shadows and sky: the fall sheet is a lit material,
and the river keeps its open-water look (own colour, sky and sun reflections)
as EMISSION while its white water goes to a lit ALBEDO. The reflection term is
linear in the colour, so the split is exact and open water is unchanged.
PhysXWaterSurface3D's raster foam (whitecaps, shore foam, the rim) and wet
sand take the same split: open water stays as it was, foam and wet sand are lit.
In the water bench with the sun turned down (`--sun-energy=0.1`) the surf no
longer glows brighter than the sand beside it; in full sun the frame barely
changes.

**Open water's body light (11 Nov).** The open water's own colour (deep and
shallow tint) was still emission at a fixed brightness, so with the sky clock
the sea glowed blue at dusk and at night. It is now that colour times the light
a level white diffuse surface would return under the sun and the baked sky
(irradiance over pi): the sun from the light the reflections already track, the
sky's irradiance summed from the same small panorama, cos-weighted. With the
reflections off (no baked sky), the colour is drawn as set, as before.

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

## Lakes lay flat (11 Nov, bug)

A lake's fetch is its own size, so at 4 m/s on a 200 m lake the JONSWAP peak is about 0.5 m long (Hs about 3 cm).
The FFT tile is 40 m with 64 or 128 points, so the peak fell below two cells and only the spectrum's tail under it
was kept: the bench measured Hs 0.00 m and a low camera saw a flat sheet. The ocean grid is now raised until the
peak spans four cells, up to 256 (Hs 0.02 m, ripples and glitter visible). Open sea is unchanged.

## Research: the scene mirrored in raster water (11 Nov, not built)

The showcase found that a calm raster lake reads as flat blue from a low camera. Raster water (Step 5i) reflects
only the sky panorama and the sun, so mountains and trees never mirror in it. Godot's SSR only runs for opaque
surfaces, and the water is blended. Path tracing already mirrors the scene.

How others do it:
- Unreal 5: water uses the Single Layer Water shading model, drawn in its own pass after the opaques. Screen-space
  reflections (or Lumen) trace against the scene's depth, and the sky and reflection captures fill the gaps.
- Unity HDRP water: screen-space reflections on the water surface, with reflection probes and the sky behind.
- Crest (Unity): planar reflections (a second camera mirrored in the water plane) in its built-in pipeline
  version, and the render pipeline's SSR in newer versions.
- Sea of Thieves and many open-world games: SSR on water, with a cubemap or sky fallback.

The two methods:
1. **SSR marched inside the water shader.** A transparent Godot shader can read the opaque scene
   (`hint_screen_texture`) and its depth (`hint_depth_texture`). March the reflected ray in screen space with a
   perspective-correct DDA (McGuire and Mara, *Efficient GPU Screen-Space Ray Tracing*, JCGT 2014), refine the hit
   by bisection, and fade near the screen edges and by thickness. Where the ray misses, keep today's sky and sun.
   For a lake seen from a low camera, the mountains above it are on screen, which is the best case for SSR.
   It misses whatever is off screen, and objects drawn after the water (other transparents).
   Cost: one march per water pixel, about 24 to 32 depth reads. Estimated 0.2 to 0.5 ms at 1440p with half the
   screen as water on a high-end GPU, to be measured on the PC.
2. **Planar reflections.** Draw the scene again from a camera mirrored in the water plane, into a texture the water
   samples, distorted by the wave normal. This is exact for flat water and also shows off-screen objects, but it
   costs a second scene pass (roughly 30 to 100% of the frame's opaque cost per reflecting plane). It needs one
   pass per water height, so every lake at a different level costs its own pass.

Recommendation: method 1, with `raster_scene_reflections` as an opt-out and the sky panorama as the fallback.
Planar stays a possible later option for a single hero plane. Not built until Paulo picks it.

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
- Raster: a FogVolume over the box with the grid as its density texture. FogVolumes need the
  Environment's volumetric fog: while a mist is live and it is off, the mist turns it on at density 0
  (no fog of its own), ambient inject 1 (lit by the sky) and 200 m reach, and puts it back after (as
  WeatherMap3D's haze layers do, so it works with or without a weather node).
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

## Step 6a: contact falls (11 Nov, first of three waterfall stages)

Gap analysis : most falls the terrain generator makes are not free
drops. The water stays on a steep face (horsetail or slide), and the river drew those as a plain ribbon.

### What a contact fall shows

- **White from part-way down.** On a steep chute the turbulent boundary layer grows from the lip; where it
  reaches the surface, air is drawn in and the flow turns white (the inception point). Upstream of it the
  water is clear and glassy. Smooth chutes (Wood, Ackers and Loveless 1983; Chanson, "Self-aerated flows on
  chutes and spillways", J. Hydraulic Eng. 119(2), 1993): `L_i / k_s = 13.6 (sin a)^0.0796 F*^0.713`, with
  `F* = q / sqrt(g sin a k_s^3)` and `k_s` the rock roughness. Below it the mean air concentration grows to
  an equilibrium `C_e = 0.9 sin a` (Chanson 1993, up to about 75 degrees). On rock with `k_s` 0.1 to 0.3 m
  and q of 0.1 to 0.7 m^2/s, `L_i` is 2 to 8 m: a tall face is white for nearly all of its height.
- **Bulking.** Aerated water swells: depth `d = d_w / (1 - C)`, so the white sheet is two to ten times
  thicker than clear water at the same discharge.
- **Ledges throw spray.** Where the face steps out (a rock bed), the sheet hits the step at near the speed
  it has gathered and splashes; each ledge is a small plunge.
- **Mist off the face.** Fine drops leave the aerated surface the whole way down, so a tall contact fall
  carries a thin veil, densest at the ledges and the foot.
- **Speed.** Steep thin films reach their friction-limited speed within metres; they do not accelerate as
  a free jet. Uniform equilibrium flow on chutes: `V = (8 g q sin a / f)^(1/3)` (Darcy-Weisbach friction on the
  depth `q / V`; Chanson 1993), with `f` about 0.1 to 0.3 on rough or stepped rock.

### How others do it

Shipped games use hand-placed meshes with panning white textures and particles at the foot (UE: Niagara plus
panning meshes; Unity HDRP's Glacier sample: deformer plus animated decals; Horizon: impact effects only).
None tie whiteness to the flow. Hydraulics gives the inception point and the air content directly, so the
fork can.

### Design

1. **Classify.** A cascade run taller than `fall_min_height` whose free jet would hit the face is a contact
   fall (today it is dropped). When the terrain generator's per-fall record is available (`type`,
   `face_profiles`, `ledges`), use it instead of the mesh.
2. **Follow the face.** The ribbon keeps draping down the face, resampled with a centripetal Catmull-Rom
   spline where it is steep (no polyline corners).
3. **Aeration.** Per section: friction-limited speed and clear-water depth from Manning on the bed slope;
   distance from the lip; `C = 0` above the inception point, rising to `C_e = 0.9 sin a` over a few `L_i`
   below it. Whiteness and opacity from `C`; thickness `d_w / (1 - C)`. Streak texture scrolled at the
   section's speed.
4. **Ledges.** Where the bed slope drops sharply going down (convex-concave step), an impact: spray from
   the existing GPU spray at a share of the speed there, scaled by the local flow power.
5. **Mist.** The existing mist grid takes a line source along the face (strength from speed squared and
   air content), not only a point at the foot.
6. **Report.** `get_waterfalls()` lists contact falls with `type = "contact"`, `inception_length`,
   `air_concentration`.

### As built

- `PhysXWaterfalls3D` (modules/godot_physx/water) reads `get_waterfalls()` from its `source` (TerrainGenerator3D)
  and drapes a sheet down every fall that is not a free plunge (`type` cascade, horsetail or tiered).
- The sheet covers the wet part of the lip only: the brink points no higher than 1.5 critical depths over the
  lowest one (a gorge's lip line runs up its walls). Each column follows the ground under it, sampled every
  0.25 m with the source's `get_height()`; without one, it falls back to the record's three face profiles,
  smoothed with a centripetal Catmull-Rom spline. A first build that only used the profiles left 30% of the
  sheet under the ground on the valley layout (gorge walls); following the ground leaves 3%.
- Per row: speed from `dv/ds = (g sin a - f v^3 / 8q) / v` (`friction_factor` 0.2), so the sheet speeds up on
  the steep part and slows on the foot; air 0 above the inception point and towards `0.9 sin a` below it over
  about 1.5 m (the surface whitens right at the inception point; a first build with 5 m left a 16 m main-river
  cascade with its inception at 11 m clear all the way down); it leaves over 6 m, since bubbles rise out more slowly
  than they are drawn in, so the water stays white across a ledge; thickness `q / v / (1 - C)`. Rows every 0.5 m, each
  vertex kept above the rock under it (a ledge's brink between rows 1 m apart cut through the sheet).
- Width and shape across (look fix, 11 Nov; the first build drew a flat strip the width of the wetted lip with
  saw-tooth edges, because its outer columns were 1 to 2 m apart and the river fall shader cut holes along the
  edge with a hard threshold): the water runs `4 sqrt(Q)` m wide (hydraulic geometry of steep channels,
  width about `a Q^0.5`, Leopold and Maddock 1953), at most the wetted lip, centred where the lip shares put it.
  Across it the flow per metre is a parabola, `1.5 q (1 - (2u - 1)^2)`, so the middle is thick and white and the
  sides thin out. Columns are 0.15 to 0.4 m apart, each following the ground, and run `wet_margin` (0.6 m) past
  the water on both sides.
- Own shader (`get_contact_shader_code()`): each vertex carries its distance inside the water's edge. The edge
  wanders with the face (two octaves of noise fixed to the rock, down the face, a share of the half-width), so
  the sheet narrows and widens as it goes down; the water fades in over 15 cm, so no step shows. Past the edge
  the rock is wet in patches: darker and glossy (wet surfaces lose albedo and roughness, Lagarde 2013), drawn as
  a dark, low-roughness veil. White water has fine lumps and thin spots riding the flow; near the margins the
  film is thinner, less white and more see-through. Path tracing uses the same edge as a cut-out and the wet veil
  as partial coverage.
- Spray (`GPUParticles3D`) at each ledge of the record and at the foot, scaled by `0.5 rho Q v^2`: drops, and soft
  camera-facing puffs that rise, spread and fade (the drops alone read as dots from a few metres); a
  `PhysXWaterMist3D` box over the face with a line source along it, only for falls carrying at least
  `mist_min_discharge` (0.2 m3/s): a trickle wets the rock but throws no measurable mist.
- Rivers draw their own falls from their beds. `PhysXRiver3D.fall_exclusions` takes the lips drawn here, so a
  contact fall is not also drawn as a free jet leaving the face.
- `get_contact_falls()` reports per fall `inception_length`, `air_at_foot`, `air_peak`, `impact_speed`, ledges
  and rows. Opt-out: `contact_falls = false`.

## Step 6b: tall plunge falls, the broken column and its veil (12 Nov, second waterfall stage)

Gap: the valley's 338 m fall (Q 3.9 m3/s, 14 m lip) is drawn by its river as the short-fall curtain: a thin,
hard-edged white slab, whole from top to bottom. The scene's mist at its foot is a block with a hard outline.

### What a tall free fall shows

- **The sheet breaks up within metres.** A nappe disintegrates once its surface disturbances reach the core:
  `L_b = 6 q^0.32` m (Horeni 1956, as used by Castillo et al. 2015 and Carrillo 2020). At q = 0.28 m2/s that is
  4 m. Below it the fall is no longer a sheet but clumps and streaks ("comets": a dense head with a trail of
  spray) inside a growing cloud of drops.
- **Clumps slow to their terminal speed.** Air drag `F = 0.5 rho_a Cd A v^2` caps each piece at
  `v_t = sqrt(8 rho_w g r / (3 rho_a Cd))`: about 9 m/s for 2.5 mm raindrop-sized drops (Gunn and Kinzer 1949),
  20 to 30 m/s for decimetre clumps. Speed down the column follows
  `v^2 = v_t^2 + (v_0^2 - v_t^2) exp(-2 g z / v_t^2)`: a 338 m fall lands near 25 m/s, not the free-fall 81 m/s.
  The generator's record already says so (impact 9 m/s for the finest share).
- **The column spreads.** A turbulent jet spreads at about its turbulence intensity, a few per cent of its
  length (Ervine and Falvey 1987), and drag on the smaller drops widens it further; the slowest drops drift with
  the wind by `wind * fall time`. Tall falls read as a widening, wind-bent veil (Yosemite, Angel Falls), whiter
  at the core.
- **Spray and mist.** The impact throws spray at a share of the landing speed; the finest drops stay airborne
  and rise as a cloud (the "waterfall wind" the column drags down spreads along the pool). Mist also leaves the
  column all the way down.

### How others do it

- Film: layered sheet, instanced streak particles and volumes for spray and mist (DreamWorks, "How to Train
  Your Dragon: The Hidden World", 2019 talk on its procedural waterfalls).
- Games: panning meshes for the top, particle streaks (ribbons or stretched billboards) for the broken water,
  sprite or volumetric mist at the foot (UE Niagara setups; realtimevfx "Season" waterfall breakdown).
- No engine ties the break-up height or the column's speed to the flow; hydraulics gives both.

### Design

1. `PhysXWaterfalls3D` takes free falls too (`type` plunge, or `free` true). Rivers drop their own curtain for
   them through `fall_exclusions`, as for contact falls.
2. **Jet.** Columns across the lip follow the projectile from the lip speed. The same sheet mesh and shader as
   the contact falls; air rises to white over `L_b`, and past `L_b` the sheet loses coverage (noise riding the
   flow) until it is gone by about `2 L_b`.
3. **Broken column.** GPU particles launched across the lip width at the break-up point with the jet's speed and
   direction, each with its own terminal speed from a size spread, quadratic drag, a lateral kick of a few per
   cent of its speed, and the weather's wind. Drawn as velocity-stretched soft streaks (white, lit, partly
   see-through), so the column reads as falling water and widens and bends on its own.
4. **Foot.** Spray and puffs at the landing (stage 1's), at the landing speed the drag gives.
5. **Veil.** A `PhysXWaterMist3D` the height of the column with a line source along it plus the foot source,
   so mist hangs along the fall and drifts with the wind. Its box gets soft walls (the density fades to zero
   over the last cells), which also fixes the hard-edged block.
6. **Report.** `get_plunge_falls()`: break-up length, landing speed, column width at the foot, drift.

### Built (12 Nov)

- `PhysXWaterfalls3D.plunge_falls` (on by default) takes the `plunge` records. The record's `top` is the bed cell
  above the drop, which on the valley sits 44 m back from the edge, so the brink is found by walking downstream
  until the ground falls away. The jet leaves at the brink depth 0.715 y_c; its sheet is the contact sheet's mesh
  and shader with `COLOR.a` carrying the unbroken share (1 to `L_b`, 0 at `2 L_b`), torn by noise riding the flow,
  and no wet rock beside it.
- The column is one `GPUParticles3D` with its own particle shader: clumps launched over the break-up zone
  (0.75 to 1.8 `L_b`), terminal speeds 9 to 26 m/s with size, drag `a = g - g |u| u / v_t^2` on the speed
  relative to the air, a 5 % kick, a per-clump eddy (`column_turbulence`) and the WeatherMap3D wind times
  `wind_exposure` (0.3: the column is in its cliff's lee and drags its own air down; at full exposure the
  valley's wind carried it 160 m back into the mountain). The cliff under the brink is sampled at 16 heights,
  so clumps blown against it run down its face instead of through the rock.
- Streaks: a camera-facing quad turned round the velocity, a dense head and a tail of spray. Never thinner than
  1.5 pixels, trading width for opacity (Persson's phone-wire AA), so a far column is a veil, not speckle. Traced:
  stochastic transparency (Enderton et al. 2010), because thousands of faint streak edges used up the four traced
  transparency layers and the column all but vanished under PT.
- A CPU ensemble of 192 clumps runs the same physics for the report and the spray and veil placement.
- Valley fall r1_19m: lip 7.9 m of water at 2.4 m/s, `L_b` 4.8 m, lands at 16.9 m/s mean (free fall 81 m/s),
  slowest clumps 37 s, column 23 m wide at the foot, 9 m of wind drift, 40,000 streaks.
- Veil: `PhysXWaterMist3D.column_height` / `column_share` feed a share of the mist evenly up the column; its rate
  grows with the fall's height (drops stripped all the way down). Every mist box now fades to nothing over its
  last three cells at the sides and top, which removes the hard-edged block.
- Open: GPU cost on the PC (40k quads, alpha blended); the stage 3 work (mist wetting rock, sound).

### Faces that lean into the jet (12 Nov, bug)

The glacial valley's tall fall (338 m) left gaps: a dry stretch under the brink, the column hidden in places, a
stub at the foot. The ground profile down the fall's centre line showed why: the face is not a cliff but a slope of
about 70 degrees (3.5 m down for each metre out), so the free jet (2.5 m/s off the lip, 2.5 m out after a 5 m drop)
is inside the rock within a few metres of the lip, and the column's clumps were pushed out of the face all the way
down, riding just over a heightfield whose drawn mesh can stand in front of them. Real water there keeps contact
with the rock: a horsetail fall, not a plunge. The face search for the column also stopped at 80 m out, so a face
running further out than that read as no face at all.

- At 96 heights down the drop the face's run out from the lip is set against the free jet's,
  `v0 sqrt(2 d / g)` (drag only shortens it). The jet meets the face where the rock stands beyond it at four heights
  in a row, so a rounded brink the water clears does not count.
- Meeting within `2 L_b` or 15 % of the drop: the fall is built as a contact fall from the brink. Further down: the
  column falls free to the meeting height and lands on the rock there, and a contact sheet starts at that point and
  runs to the foot.
- A handed-over sheet fans out down the face at the jet's turbulence rate (3 % of its travel, as the plunge sheet
  spreads), thinner as it widens; the generator's own contact falls keep their width.
- The column's face search reaches the whole run of the fall plus 40 m.
- Valley fall r1_19m: now a 338 m contact fall from the brink, unbroken to the foot.

### Brink, ledges and veil (13 Nov)

- White water stays white: a sheet fed by a run steeper than a river cascade (0.08 over the last 8 m before the
  lip) starts at the equilibrium air for that run and has no clear stretch. Bubbles rise out over metres, not in
  the second the water takes to reach the face; the clear film had drawn a dark gap under the white rapid.
- PhysXRiver3D draws its surface down to brink depth (0.715 y_c, Rouse 1936) before a lip another node draws, over
  4 y_c and at least one section, as at its own falls; it stood at full depth up to the hidden stretch.
- `PhysXWaterfalls3D.surface` (a Landscape3D): with `cliff_relief` the drawn rock stands off the heightfield, so
  each sheet point moves by `get_cliff_point(base) - base` and runs over the ledges; the heightfield clamp is skipped
  where it moved.
- A tall drop handed from a plunge to the rock throws the plunge's foot veil (impact speed, the fanned width, the
  height factor); the face's line source had spread its mist over a 380 m box to almost nothing.
- Hanging valley (0f47d01d): its 389 m fall becomes a contact fall from the brink (face 116 m out over the drop);
  at 0.03 m3/s off a 0.39 km2 catchment it is a thread with no mist, which is right for that flow.

## Sources

- Wood, Ackers and Loveless, General method for critical point on spillways, J. Hydraulic Eng. 109(2), 1983;
  Chanson, Self-aerated flows on chutes and spillways, J. Hydraulic Eng. 119(2), 1993.
- Leopold and Maddock, The hydraulic geometry of stream channels, USGS Professional Paper 252, 1953.
- Lagarde, Water drop 2b: dynamic rain and its effects (wet surfaces), 2013:
  https://seblagarde.wordpress.com/2013/04/14/water-drop-2b-dynamic-rain-and-its-effects/
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

## Raster seabed fade with depth (12 Nov)

The showcase valley's sea is a shelf that deepens from the beach to about 17 m at the end of the measured seabed
(the generator's world rectangle). In raster the sea surface had a fixed alpha (`water_color.a`), so the shelf
showed through it at every depth: a pale, glint-free band along the coast that stopped in a straight line where the
seabed ends and open sea begins. The path tracer already absorbs light along the path through the water (its
`pathtraced_absorption` medium).

Fix (raster only): the surface's opacity follows Beer-Lambert transmittance of light from the bed, down and back
up along the view, `T = exp(-sigma d (1 + 1 / mu))`, with `sigma` the mean of `pathtraced_absorption` (so raster
and path tracing fade the bed alike), `d` the still-water depth and `mu` the cosine of the view to the vertical.
Alpha goes from the water's own value over the shallows to opaque where the bed is gone. The swash film and the
waterline's cut-out are unchanged. Same technique as depth-based water opacity in UE's single-layer water and
HDRP's water (absorption distance).

## Every water body takes shadows (15 Nov, bug)

Paulo saw the river under the big fall stay bright in the cliff's shadow. In raster the open water of PhysXRiver3D
and PhysXWaterSurface3D (lakes, the sea) is drawn as emission, its own bounded look: lit as a whole PBR surface
and blended over the lit floor it had blown out. But the sun's share of that emission (the glitter's GGX lobe and
the sun's light on the body colour) came from uniforms, so no shadow map, terrain shadow or cloud projector ever
reached it. Foam, wet sand, the contact sheets, the plunge columns and spray were already lit as surfaces and took
shadows; the path tracer traces its own shadows for all of them.

- How engines do it: water shaders take the main light's shadow attenuation for the sun specular and the
  sunlit part of the scattering, and keep the sky reflection unshadowed (Unity HDRP water samples the directional
  shadow for its specular and its scattering; UE's single-layer water applies the directional light's shadow to
  its direct lighting while reflections come from captures and SSR).
- Built: the sun's two terms move to light(). The fragment hands the body colour's weight, the glitter's normal,
  weight and roughness (widened by the screen derivatives there, since light() has none) to light() through
  varyings; light() adds them for the directional light under `ATTENUATION` and `LIGHT_COLOR`, which carry the
  shadow maps, the light's exposure normalisation and the cloud projector. The sky's share of the body light and
  the mirrored sky stay in the emission, since water in shadow still mirrors the sky. Foam keeps a Lambert term in
  the same light().
- Under 0.6 cloud cover the sea now darkens under the cloud shadows like the beach beside it.

## Shadows in the water (15 Nov, bug)

With the camera under the water, the raster light scattered in the water toward the eye was the sun's
everywhere: a bay under a cliff, or under a cloud shadow, glowed as if lit. The surface and the bed took their
shadows (#286 and the scene's own lights); the medium between them did not, while the path tracer lit it only
where the sun reached.

How others do it: Crest reads the shadow maps into its own data before the transparent pass and uses a soft
term for the scattering in the volume; Unreal's Single Layer Water leaves its medium unshadowed (its docs say
moving under the water projects no shadowing) and relies on Volumetric Fog for shafts; froxel fog (Wronski
2014, Hillaire 2015) is the general answer, but Godot's fog density and absorption are grey, and water's
absorption is strongly per channel, so the medium stays in our own pass.

What we build: a spatial stage function, `sun_visibility(world_position)` (vec3), the first directional
light's shadow (one hardware PCF tap from the cascade over the point) times its projector (the cloud shadow),
at any point; Forward+ and Mobile read the shadow atlas, Compatibility returns 1. The underwater pass marches
the sun's share over the near water (24 steps, about five mean free paths), each step lit as far as the
light reached where its refracted ray entered the water (the shadow map holds the shadow above the surface;
beneath it the refracted ray bends away from the map's), integrated in closed form within the step, and
jittered per pixel and frame (interleaved gradient noise, Jimenez 2014). The camera-to-surface stretch seen
from below and the foam's sun from below take the same light.

Left for later: the sky's share is still unoccluded (a cliff hides part of the sky too; the terrain's sky
map could be read here), and an underwater rock's own shadow in the medium (the march reads the light at
the surface only).

## Clear water you can see into, shadowed (15 Nov, research for Paulo)

Paulo: water should be partly transparent and shadow itself, for instance on a beach (examples he pointed
at: Derelicts, an indie survival game on UE5, and FluidNinja LIVE 2 for UE5).

Where we are. The path tracer already does this physically: the surface refracts, the water absorbs per
channel, scatters, and every ray to the sun is a shadow ray, so a beach shows the sand through the
shallows, a cliff's shadow darkens the water body and the bed under it, and caustics form. The raster
surface is an alpha-blended tint over the bed (an early version that read the screen texture blew out, and
alpha blending replaced it), so on the water bench the raster shallows read milky where the path tracer
shows clear water over sand, and a shadow across the water only darkens the surface's sun terms (#286),
not the water below it. The underwater passes since read the screen texture without trouble.

What others do:
- Unreal, Single Layer Water: a pass after the lighting reads the scene colour and depth behind the
  surface and applies a homogeneous medium (absorption and scattering per channel in 1/m, Schlick phase).
  Its docs say the medium projects no shadowing; shafts come from Volumetric Fog. Studios on it (Photon
  Water, LightSpeed) add their own underwater volumetric lighting.
- Crest (Unity): refraction off the camera colour texture with per-channel depth fog, subsurface terms for
  the sun, and the shadow maps read into its own data before the transparent pass: a hard term that blocks
  the specular and a soft term for the scattering in the volume, tinted by a shadow colour. Caustics from a
  texture with a focal depth.
- HDRP water: refraction with a maximum distance, absorption distance, scattering from ambient, height,
  displacement and the direct light; caustics that take the directional light's shadow.
- Fluid Flux 2 (UE5, the beach demos): shallow-water simulation blended with wave profiles near the shore,
  listed features include wave shadow.
- Real-Time Underwater Spectral Rendering (Monzón et al., Eurographics 2024): spectral attenuation and
  scattering for underwater, the reference for per-channel colour.

Proposal (raster; the path tracer needs nothing):
1. See-through water: the surface reads the scene behind it (colour and depth), offset by the refracted
   ray, and dims it per channel by Beer-Lambert over the real path through the water (down to the bed and
   back along the view), adding the scattered light; the Fresnel reflection on top. Shallows show the sand,
   deep water turns to the water's own colour, both matching the path tracer.
2. Shadows in the water from above: the scattered light marched along that refracted path with
   `sun_visibility` (as under water, #287), so a cliff's or a cloud's shadow darkens the water body itself.
3. Waves shadowing: the displaced surface casts the sun's shadow onto the troughs and the bed (and the
   caustics take the shadow maps, as HDRP's do).

Step 1 built (15 Nov): `clear_water` (shader parameter, on). The open surface seen from above reads the screen
behind it at the refracted point (the bed's depth from the solver's seabed, else from the screen; a point
showing something in front of the water halves the offset twice, then takes the straight pixel) and passes
it through the medium: the bed's light dimmed on its way down (as the underwater pass does) and along the
refracted path, plus the sun and sky scattered along it, the sun's as far as `sun_visibility` lets it
reach the surface. The path length follows the solver's seabed, not the screen's depth, which jumped from
pixel to pixel where the waves bend the offset (a stepped edge across the shallows). The open water's own
sun body term (#286) is off in this mode, its sun being inside the medium's light; glitter, reflections,
foam and wet sand are as before. The light in the water is now set every frame (also in the editor), not
only near the camera. Rivers follow.

Step 1b (15 Nov): rivers too. The clear-water code moved into water_raster_reflections.h
(`water_clear_through`, with `water_light_in_water` deriving the refracted sun and the sky's radiance from the
raster reflection uniforms both shaders already share), and PhysXRiver3D's shader uses it with the ribbon's
own depth for the bed (`clear_water`, on). The waterline's foam lace (edge_foam) now shows as the thin line it
was meant to be; under the old tint's low alpha it was barely visible.

Step 2 (15 Nov): shadows in the water from above. `water_clear_through` marches the sun's scattered light
along the refracted path (12 steps over about four mean free paths, jittered per pixel and frame), each
step lit as far as its refracted sun ray entered the water (`sun_visibility`), as the underwater pass does.
A shadow across deep water now darkens the water body itself; on the bench's open sea the raster colour
now matches the path tracer's (median 58/109/107 against 45/112/116). Shallow water shows little change:
there the bed's own light, already shadowed, dominates.

Step 3 (15 Nov): waves shadowing themselves. `wave_shadows` (on): from each surface point the sun's ray is
marched over the resting heights (6 jittered steps, far enough to climb 2.5 m); the length it spends inside
crests in front dims the sun on foam and wet sand (Beer-Lambert) and removes the glitter on the lee sides
(heightfield self-shadowing, as relief mapping's: Policarpo and Oliveira 2006, Tatarchuk 2006). Subtle on
the bench's chop; it matters with swell at a low sun. Cost: six height lookups per pixel of water.
Raster caustics are not part of this: the caustic map (water_caustics_map.glsl) stores a tone-mapped
focusing excess, not an energy-conserving ratio, and nothing draws it in raster today; making it physical
first is its own piece of work.

Found on the way (bug): the surface lies on the ground as a swash film wherever the ground stands above
it, so over a block standing out of the water the mesh's triangles were drawn up its faces from the
waterline to its top (dark teeth, raster and path traced). The film now lies only on ground less steep
than about 35-50 degrees (the slope of the seabed texture over 0.7 m).

## Storm seas (15 Nov, research for Paulo)

Paulo asked for bigger open-ocean waves, as in Sea of Thieves and Black Flag, and then for parity with
FluidNinja LIVE 2's beach and landscape water. Order picked: storm seas, raster caustics, live shallow water
on terrain, then a world wet map and bodies pushing water by their shape.

**What those games do.**
- Sea of Thieves (Ang et al., "The Technical Art of Sea of Thieves", SIGGRAPH 2018 talks): a Tessendorf FFT
  ocean. Crest colour from a wave peak mask taken from the choppy displacement (thin peaks have shorter
  light paths), mixed by view angle and sun direction. Foam at peaks and around objects (depth buffer),
  blurred over frames with feedback; more foam in storms. Shallow-water splashes from a GPU shallow-water
  solver (Mei et al. 2007).
- Assassin's Creed III and IV (fxguide 2012 and 2013; Ricour, Game Developer 2013): Tessendorf FFT tiles
  projected from the camera. Sea states follow the Beaufort scale 0-12: each level sets the waves' surface
  scale, height scale, choppiness and foam decay (foam keyed at 0, 3, 7 and 12). Crests lit by a cheap test
  of view, sun and slope so back-lit crests look thinner and brighter. Spray from depth probes along the hull.
  No separate rogue-wave model: storms are the higher Beaufort levels.
- FluidNinja LIVE 2 (manual 2.06): a 2D fluid solver painted by moving objects (bones, mesh distance fields)
  that drives the water mesh, a wet map with fade-out, and caustics. Water follows the landscape's height
  gradient, fills pits and follows river splines. No breaking surf, foam particles or open-ocean storms.

**What we have.** Four FFT cascades of a fetch-limited JONSWAP/TMA spectrum, directional spreading with
swell, choppiness, whitecap cover steered to Monahan's fit, shore breakers, spray off lips, the weather's
wind (`follow_weather`).

**Finding: storm height.** At 22 m/s over 500 km of fetch the bench sea measured Hs 2.8 m, against about
8 m from JONSWAP. The cause is `depth`, 3 m by default: the spectrum's TMA factor treats the whole sea as
3 m deep and holds the long waves down. With `depth` 80 the same storm measures Hs 7.8 m. Open sea needs
its real depth; the bench's new `--view=storm` (and `storm_sun`) sets a deep gale with no floor.

**Built (15 Nov).**
- Foam streaks: from Beaufort 7 to 9 (14 to 22 m/s) fading foam breaks up along the wind (lace stretched
  ten to one) and trails downwind of its patches, each third-of-a-metre lane by its own distance so the
  trails read as lines (the finest cascade's foam only: the long cascades' metre-wide cells smear).
  Shader uniforms `foam_streaks` and `foam_streak_dir`, set from the wind.
- Crest spray (spume): `crest_spray` (on), `crest_spray_amount` (6000). Every fourth physics tick, 384
  resting points within 40 m of the camera (none nearer than 8 m) are tested with the summed cascades'
  Jacobian on the CPU readback, as the shader's whitecaps; folding ones become emission points for droplets
  thrown downwind at a quarter to half the wind speed, rising 0 to 1 at Monahan et al.'s (1986) and Andreas's
  (1998) spume onset, 9 m/s, full by 20 m/s. Drawn with the surf spray's droplet. Cost on the cloud CPU:
  0.14 ms a physics frame.
- Whitecap steering: the threshold starts from a fit of the bench's converged thresholds against the wind
  (6 m/s 0.729, 9 0.747, 12 0.761, 15 0.779, 18 0.797, 22 0.824: 0.694 + 0.0059 U) and follows wind changes
  by it; the measured cover (persistent foam, filling as 1 - exp(-t / persistence) after a start) is divided
  by that fill before steering. Before, a storm's cover overshot to 21% against a 14.5% target for seconds.
- Ocean height, displacement and derivative textures are created cleared (were undefined until a cascade's
  first step).
- Second pass (docs declined the first pair: at slider size the two halves looked the same; the streaks
  were only the fading whitecap foam drawn out, and 2 to 4 cm droplets vanish past a few metres).
  Spume puffs: the crest emitters also feed soft camera-facing blots 1.2 to 3.5 m across with ragged
  noise edges, an eighth as many as the droplets, thrown nearly flat downwind at 0.2 to 0.4 U, held low
  (gravity 2.5 m/s^2, damping 1 to 2), growing and thinning over 2 s, lit from behind; the falls' spray
  puffs' technique. Windrows: value noise stretched about fifty to one along the wind (filaments a
  third of a metre across, tens of metres long, wandering by a slow noise), cut high and gathered into
  bands, over the whole sea at foam_streaks; past where a filament is under a pixel (2 mrad footprint
  from the grid centre, since the vertex stage calls foam_at_with and has no derivatives) it fades to
  its mean cover. Bench `--view=storm_low`: 3.5 m up looking downwind at 20 m/s. The puffs face the
  camera by `render_mode billboard_particles`, which the path tracer also honours (a vertex() billboard is
  not run for traced particles: they showed as flat bright rectangles; the falls' puffs had the same bug).
- Third pass (docs: the windrows ran as smooth unbroken ribbons in one half of the frame, cut off along a
  hard line by the banding noise, and the whitecaps looked smaller with streaks on). Windrows now: three
  octaves stretched about thirty to one (filaments about a quarter metre across), broken along their
  length every few metres by a second noise, and only gently denser in places (0.6 to 1, no band that
  ends). The whitecaps' lace is no longer stretched along the wind (that thinned them in a gale); the
  streaks carry their own structure. `foam_streaks` is a material uniform the node sets from
  `wind_speed` (smoothstep 14 to 22 m/s), not a property.

**Tried and dropped.** The sun's path in the water traced to the real surface toward the sun (a crest with
the sun behind it lit through its thin top, the effect Sea of Thieves and Black Flag fake with a peak
mask): no visible change on the clear open sea in raster (mean difference 0.05/255) and none to match in
the path tracer either; the water is too clear for a few metres of path to show. Not worth four extra
height lookups a pixel.

**Fixed (15 Nov).** In a storm's first second wide white patches showed and cleared within about two
seconds. Not the persistent foam (each cascade's cover stayed under 1% from the start) but the material's
crest foam, the Jacobian of the summed cascades: the cascades' Gaussian draws were seeded by mode index
alone, and every extra cascade carries the same index band (4 to 24) on a tile `wave_cascade_ratio` times
larger, so the cascades were scaled copies of one field. At the start their slopes added in phase and the
sum read as folded; the long waves drifted apart over a couple of seconds. The banded cascades now salt the
seed with their tile size (independent draws, as the random-phase model asks; Tessendorf 2001). Bench
storm view, mean brightness of the sea at frame 8: 120 before, 36 after (90 frames: 88 either way). The
first cascade keeps its draws, so a one-cascade sea is unchanged; multi-cascade seas show different (equally
likely) waves.
Bigger, steeper storm waves than the spectrum gives are an art choice: `wave_amplitude` 1.6 and `choppiness`
1.4 give the Black Flag look on the bench.

## Raster caustics (15 Nov, FluidNinja parity step 2)

The light-space caustics map (water_caustics_map.glsl) has been drawn since the first water pass but stored
a tone-mapped "excess light" in 8 bits and had no reader; the bed under clear raster water was lit evenly.

**Established technique.** Wallace's WebGL Water (2011) and Unity HDRP's water system (2022.2) both rasterise
the surface's grid refracted onto a plane a fixed distance under it (HDRP's "virtual plane distance") and
read the ratio of a triangle's area before and after as the light's concentration there, summed with
additive blending; HDRP then scales the lit underwater scene by it. Caustics mapping (Shah, Konttinen and
Pattanaik 2007) is the same splat taken from the light's view onto real receivers. Crest scrolls a
painted caustic texture by a focal depth (not physical). Our map is already the first kind, with an
orthographic light-space projection like a shadow map, so any receiver can look it up.

**Built.**
- The map stores the physical ratio: R = irradiance at the reference depth over a flat surface's (1 even,
  0 none, up to 64 at a focus) in RGBA16F, G the water mask. Receivers read R + (1 - G), so dry texels
  read 1.
- The ocean's heights in the map are read at world positions (they were read relative to the map's centre,
  so the pattern was out of phase with the drawn waves wherever the centre was not a whole tile away).
- The map follows the scene's sun (the light the water is drawn with); `caustics_sun_direction` only
  when no sun is up.
- Clear water reads it: the bed point seen through the surface goes back up its refracted sun ray and down
  to the reference depth, and the map's ratio scales only the sun's share of that bed's light (the sun
  over sun plus sky after both have crossed the water down to it), times sun_visibility there, so shaded
  beds and the sky's light stay as they were. `caustics_reference_depth` sets where the pattern is sharpest.
- The map's ocean heights are read with a cubic B-spline (Sigg and Hadwiger, GPU Gems 2 ch. 20, four
  bilinear taps): bilinear heights have one slope per texel and focused the light onto the texel edges as
  a square grid.
- Only the first ocean cascade and the ripples shape the map (longer cascades barely focus at a few metres).

## Live water on terrain (15 Nov, FluidNinja parity step 3, research)

Paulo's ask: water that flows, fills and floods over the ground as FluidNinja LIVE 2 and Fluid Flux
show it (a stream finding its way down a slope, a hollow filling into a pond, rain running off and
pooling, a dam breaking). What exists: the live window (step 3c, Chentanez and Müller 2010 on the
ripple grid) solves shallow water inside a water body's square, but has no moving wet/dry fronts and
never leaves its body. "The full live version" above is the plan this starts.

**How others do it.**
- Fluid Flux (Unreal): a 2D shallow-water simulation on render targets over the landscape, with
  sources, slopes and falls, drawn as mesh tiles; its coastline and waterfall showcases are the
  reference look (80.lv articles, ImaginaryBlend forum thread).
- Virtual pipes (O'Brien and Hodgins 1995; Mei, Decaudin and Hu 2007 on the GPU; Štava et al. 2008
  for interactive terrain): per cell a water height and four outflow fluxes, accelerated by the
  height difference to each neighbour and scaled down when they would drain the cell below zero. It
  conserves volume exactly, never goes negative, wets and dries without special cases, and stays
  stable on cliffs, which is why city and terrain games use it for floods. Its weakness is inertia:
  waves are damped and eddies do not form.
- Chentanez and Müller 2010 (our live window): semi-Lagrangian velocities, better waves and eddies,
  needs extra care at wet/dry fronts and steep slopes (their overshoot fix, depth clamps).

**Proposal.** A new node, a hand-placed live area over any ground (`PhysXLiveWater3D`): a square of
up to about 256 m at 0.25 to 1 m cells. It reads the ground's heights once (physics ray casts, so
terrain, rocks and buildings all count) and keeps them as a texture. Water comes from sources (point
inflows in m^3/s, area springs), from rain (the weather's rain rate) and from the edges of water
bodies it touches, and leaves by sinks and soaking in. The solver is the pipe model, with velocity
from the fluxes for flow-aligned foam and drag. The surface is a displaced grid using the shared
water shader (clear water, shadows, caustics, foam where it runs fast or falls), traced by the path
tracer through vertex capture. A CPU readback gives `sample_height()` and `sample_flow()` for
buoyancy. Later, the same tiles stream over a whole world (tile manager, LOD, sleeping tiles) as
"The full live version" lays out.

### Built (15 Nov): `PhysXLiveWater3D`, placed live areas

- A Node3D holding a `size` grid of `cell_size` cells (default 64 x 64 at 0.5 m). Ground comes from
  physics ray casts against static bodies on `ground_collision_mask`; cells with no ground drain.
  `update_ground()` re-reads it and keeps the water, so a wall that breaks lets the water through.
- Solver: the virtual pipe model on the CPU (Mei, Decaudin and Hu 2007), `friction` damping the
  fluxes, CFL substeps (at most 16 a tick), `open_edges` letting water leave at the border.
  Volume is conserved exactly: a 1.5 m^3/s spring matches the stored plus lost volume to the litre.
- Water in: `fill_rect` + `fill_height` at start, `sources` (x, y, z, m^3/s), `add_water()`, rain in
  mm/h (or the WeatherMap3D rate with `rain_from_weather`), `infiltration_rate` soaking it away.
- Surface: a grid mesh with the river shader (shadows, clear water, reflections), depth and foam
  per vertex, velocity in UV2 so foam and normals follow the flow. Foam appears where the water runs
  faster than `foam_speed` and supercritically (Froude above 1.5). Dry vertices sit just under the
  ground, so wet/dry edges move with the water.
- Queries for gameplay and buoyancy: `sample_depth()`, `sample_height()`, `sample_flow()`,
  `get_total_volume()`, `get_volume_out()`, `get_cost_ms()`.
- Cost: about 3 ms a tick for 160 x 160 cells on the cloud CPU (peaks near 11 ms in a dam break).
- Bench: `docs/bench/live_water` with `--scene=spring|dam|rain`, `--view=overview|low`, `--break=s`.
- Not yet: GPU solver, depth over 3 m shading (the river depth encoding), muddy water colour,
  streaming over a whole world (step c, only if wanted).

### Built (15 Nov): wet and dry fronts in the live window (step b)

The live window (rivers, lakes and sea round the camera) now lets its water run up onto dry ground
and drain back, after Chentanez and Müller 2010 section 5:
- Ground up to 1 m above the still water (`RUNUP_BAND` in water_ripple.glsl) is simulated too; a dry
  cell's height is its ground. Before, only cells under still water were simulated and a cell never
  emptied below 2 cm, so a wake or splash stopped dead at the still waterline.
- A face carries water only out of a cell that holds some (upwind depth), at most a quarter of it
  per tick, so no cell goes below its ground; a face between a dry cell and water lower than its
  ground carries no velocity.
- Dry cells render as before (the neighbouring water continued under the ground), so static
  shorelines look the same. River bank vertices now take the live height (the ribbon's margin), and
  the river hands the window its signed depth (banks and rocks keep their real height; zero made
  them flood as flat white blocks).
- Bench: water_surface `--view=runup --splash=6,4 --node=wind_speed:1` (mounds dropped near the
  shore every 2 s run up the sand and drain back within about 2 s).

## Wet where water has been (15 Nov, FluidNinja parity step 4a, research)

Paulo's ask (FluidNinja parity): ground, rocks and props stay dark and glossy where water ran over
them, then dry out: a wave's reach on the sand, a flood's high-water line, splashes on a rock, a
character's footprints out of a stream. What exists: `WeatherMap3D` wetness (rain film and puddles,
material-wetness.md) in 32 m cells over 4 km, read by BaseMaterial3D Wetness, terrain, grass, plants
and hair, raster and path traced. The sea draws its own wet sand behind the swash. Nothing records
contact with water.

**How others do it.** The film-industry "wet map" (Houdini wetmap, Thinkbox Stoke's wetmap field,
RealFlow collider wet maps): per texel, wetness = max(wetness, contact) each step, then an
exponential decay toward dry. FluidNinja LIVE 2 does the same on a render target round the player,
written by its fluid's density, faded over time, read by the landscape and props. Fluid Flux reads
its simulation depth for the shore's wetness. Drying speed in reality depends on air temperature,
humidity and wind (evaporation); sand dries in minutes, rock in tens of minutes.

**Plan.**
- `WeatherMap3D` gains a contact wetness layer: a square of fine texels (default 512 over 64 m,
  12.5 cm) that follows the camera, published as global `weather_contact_map` + `weather_contact_rect`.
  Texel = max(texel, contact), decaying with a drying time from the weather (slower while it rains or
  in cold, still air; `contact_dry_time` at 20 C as the base).
- Writers: `add_contact_wetness(pos, radius, amount)` for scripts and bodies, `PhysXLiveWater3D`
  (its wet cells each tick), the live window of seas, lakes and rivers (wet cells, done on the GPU
  by the ripple pass), and spray/splash particles landing.
- Readers: the existing `weather_wetness()` helpers take max(weather, contact): BaseMaterial3D,
  TerrainMaterial3D, grass, plants; the path tracer the same through three new rt_params.
- Cost: one more texture read where wetness is already read; a 512^2 R8 texture update a frame.

### Built (15 Nov): contact wetness

- `WeatherMap3D` `contact_wetness_*` (on by default; 512 texels over 64 m round the camera, R8),
  globals `weather_contact_map` / `weather_contact_rect`, `add_contact_wetness()`,
  `add_contact_wetness_grid()`, `get_contact_wetness_at()`, `get_contact_drying_time()`.
- Drying: `contact_wetness_dry_time` (600 s) at 20 C, half humidity, 2 m/s, scaled by the vapour
  deficit (Magnus) and Penman's wind function; none while it rains. Grids stamp bilinearly, so
  0.5 m cells leave soft edges.
- Readers: BaseMaterial3D and TerrainMaterial3D (shared `weather_wetness_at`), GrassField3D, the
  path tracer (rt_params 64 to 67, RT_PARAM_COUNT 68). Plants and hair still read the rain only.
- Writer: `PhysXLiveWater3D` stamps its wet cells each tick. Sea, lake and river live windows not yet.
- Bench: live_water `--wet --scene=spring --stop=300 --frames=720` (the gully stays dark after the
  spring stops and the water soaks away; `--dry=8` shows it dry). Raster and path traced match.

## Live water over the whole world (15 Nov, FluidNinja parity step 3c)

Paulo picked "both, streaming first". The plan in "The full live version" (sparse tiles, level of
detail, streaming) asks for a tile manager round the solver. How others do it:
- Fluid Flux (Unreal) runs its shallow-water simulation in a "simulation domain" that follows the
  player, with the state outside it baked or frozen; its "coastline" and "river" setups keep a
  static base state the live domain starts from.
- Clipmaps for terrain and the toroidal update of snow and wetness maps (as our WeatherMap3D cells
  and SnowCover3D do): a camera-centred grid shifted in whole cells, keeping what overlaps.
- Sparse tiles with sleep/wake (Bridson and Müller-Fischer's sparse grids; Chentanez and Müller
  2011 "Real-time Eulerian water simulation using a restricted tall cell grid" freezes inactive
  regions): only tiles with water are kept; inactive ones are not stepped.

**Built.** `PhysXLiveWater3D.streaming`: the area becomes a window of world-aligned cells that
follows the camera (or `stream_follow`) in steps of an eighth of its size; the node moves itself.
- Cells leaving the window fall asleep in tiles of 32 x 32 cells (ground and depth, NAN ground =
  not measured): kept, drawn with the same surface (one mesh per tile, with a row and column from
  the next tiles so they meet), not stepped. Cells coming in wake from their tile with their water at
  rest, or have their ground measured (one ray each).
- The edge is no wall: outside it the pipes see the sleeping water or dry ground just beyond, and
  water that flows over it is added to that sleeping cell (so it piles only until level with the
  water inside). Volume is kept: 30 m^3 in, 30.0 awake plus asleep after walking 100 m away and back.
- Tiles with no water are dropped; `max_sleeping_tiles` (4096, about 32 MB) caps memory, past it
  water leaving the window is counted lost. Sources are world positions while streaming and wait
  while outside the window. Rain and soaking in only act inside it.
- Cost: as a placed area of the same size (about 1.2 ms a tick for 96 x 96 cells on the cloud CPU),
  plus one ray per new cell when the window steps (a strip of an eighth of the window).
- Known limits: water outside the window is still (a stream cut by the window's edge stops there
  and a pond forms at the edge until the window comes); no level of detail yet (far water is drawn,
  not simulated); not saved with the game.
- Bench: live_water `--scene=stream` (a 240 m hillside; the marker walks 100 m away and back).

## Bodies pushing water (15 Nov, FluidNinja parity step 4b)

Paulo's ask: legs, boats and props moving through any water push it aside: a heap at the front, a
hollow and a wake behind, the level standing up against a leg. How others do it:
- FluidNinja LIVE: "interaction" spheres placed on a character's bones (and on props) are written
  into the fluid each frame as density and velocity sources.
- Crest (Unity ocean): a `SphereWaterInteraction` component per limb or hull section, a set of
  spheres that push the dynamic wave layer in proportion to their speed and how deep they sit.
- Height-field solvers with bodies: Chentanez and Müller 2010 ("Real-time simulation of large
  bodies of water with small scale details", section 6) and Thürey et al. 2007 put the body's
  displaced height into the column's pressure head: water flows out of the space a body takes and
  heaps round it, volume kept by construction.

**Built.** `PhysXWaterPusher3D`, a node under the body (or `target`):
- Its spheres come from a `Skeleton3D` (spheres of `bone_radius` along each bone) or from the
  body's `CollisionShape3D` children (sphere; a row along a capsule or cylinder; a grid through a
  box; others their bounds). `max_spheres` per water (16, at most 48) of those under the surface.
- Every live water, sea and river in the tree gets them (rescanned every 2 s). Live water takes
  them as displacement (`PhysXLiveWater3D.submit_sphere`): per cell, the sphere's height under the
  surface is added to the pipe head, so water flows out of the body's space; a sphere thinner than a
  cell spreads its submerged volume over the four nearest cells, so a leg 24 cm across still moves
  water on 50 cm cells. The drawn surface and `sample_height` include it (the level stands round a
  leg rather than dipping into it). Sea and rivers take the spheres in their ripple layer (as
  buoyant bodies already do), pushed harder near the surface and when moving
  (`rest_strength` at rest, full at `reference_speed`).
- Result (live_water `--scene=wade`, legs walking 1.5 m/s through a 0.6 m pond): volume kept
  exactly (227.11 m^3 all run); on 25 cm cells the surface 0.4 m ahead of the legs stands 10 cm above
  the water beside them and 5 cm below behind, with a white wake; on 50 cm cells 1.5 cm and the wake
  is too fine to draw. No measurable cost (a few spheres in a solver step of about 3 ms).
- Known limits: the surface is drawn through the body's space, so an open hull would show water
  inside it (no mask yet); speed comes from the body's motion between frames, not its rotation.
- Benches: live_water `--scene=wade [--no-pusher] [--view=wade_close]`; water_surface `--wader
  [--wader-nopush] --view=wader`.

## Seas and rivers wet the ground (15 Nov, FluidNinja parity step 4a, the rest)

The contact wet map (above) took only live water. The sea already kept its own wet sand behind the swash
(water_swash.glsl, `wet_sand_dry_time` 12 s, drawn by the sea's material over the floor), but nothing else
on the shore knew: rocks, plants and props stayed dry, and the sand dried in seconds. Same approach as for
live water (the wet map is fed by whatever the water covered), here from the swash pass's wetness channel.

**Built.** `PhysXWaterSurface3D.contact_wetness` and `PhysXRiver3D.contact_wetness` (on; the river with
`live_flow`): the swash texture's wetness (RGBA32F on the ripple grid) is read back every eighth step
(async, as the other readbacks; about 1 MB for 256^2 cells) while a WeatherMap3D with contact wetness is
active, and written with `add_contact_wetness_cells()` over land and still water shallower than 0.5 m
(deeper cells skipped, so a pier or hull over open water is not wetted from below), faded over the grid's
outer eighth. The river drops the two reads after its window moves. Result (water_surface `--contact
--view=runup --node=shore_waves:true`): the sand reads wet from the water up to 2.5 m past the still
waterline (where the run-up reached) and dry beyond; river `--live --contact`: the banks and shallows
round the rocks.
Known limit: the wet map is 2D, so anything at that spot is wetted whatever its height (a bridge over a
shallow river bank).

## River profile from hydraulics (15 Nov, Fab idea 1 stage 1)

Paulo's pick "All four ideas to water", first item; references in
[water-river-hydraulics-reference.md](water-river-hydraulics-reference.md). Until now the river's
surface was a running normal depth (Manning per section, then a running minimum down the river), so
it never pooled behind a bump, never drew down before a step, and could not jump.

### Built: `gradually_varied_flow`, `bend_superelevation` (both on)

- **Standard step method** (Chow 1959 ch. 10). Energy `E = z + Q²/(2gA²)`, friction slope
  `Sf = (Q/K)²` with Manning conveyance, averaged over each half-metre step. Sections are measured on
  the bare ground (rocks are obstacles, not the channel), each taking the widest section within
  ±1 m at the same absolute height but never across a step, so one narrow section cannot choke the
  river.
- **Two passes.** Subcritical from each downstream control upstream; supercritical from each
  upstream control down. Where both exist the one with the larger specific force
  `M = Q²/(gA) + ∫d²/2 dn` wins; where fast water gives way to slow, that section is a hydraulic
  jump's toe, recorded with its upstream Froude number. The sequent depth comes out of the momentum
  balance itself (Bélanger), not a formula.
- **Controls.** Critical depth (found by bisection on `Q²T/(gA³) = 1`) at the brink of every step
  and fall, unless the pool below drowns it; the existing brink drawdown (0.715 y_c, Rouse 1936)
  still finishes the lip. Through a cascade's own sections the surface only falls.
- **Foot of a step** (under `fall_min_height`): the water lands fast and shallow, at Rand's (1955)
  depth for a free overfall, `y1 = 0.54 h (y_c/h)^1.275`, kept as an energy fraction so it fits the
  real section. It runs on until the tailwater's momentum holds it, then jumps. A waterfall at or
  over `fall_min_height` plunges into the pool it has scoured instead (natural falls dig one), so
  its foot keeps the pool level.
- **Jump roller.** Fr1 ≥ 1.7: white water over about 6 sequent depths (Peterka 1958), stronger the
  higher Fr1. Below 1.7 the jump is undular (Chanson 1995): no roller; its undulations are stage 2.
- **Supercritical lace.** Fast water alone now foams to at most 0.4 (lace, as past a rock) instead
  of a solid sheet; cascades, plunges and rollers stay fully white. A long fast reach keeps its
  texture.
- **Bend superelevation.** Surface tilt across each section `-curvature · U²/g` (Chow 17-4), held
  to half the mean depth over the half width. Mesh, `sample_height` and the depth across all use it.
- `get_section` adds `froude`, `critical_height`, `jump`, `superelevation`.

### Results (river bench, raster)

- Default river: the mild upper reach is unchanged within a few cm; the 1.2 m step now draws down
  to critical over its last 2 m, leaves as a small free fall, and lands as Fr 3 water that runs
  12 m before a Fr1 1.6 jump (tailwater shallower than the sequent depth: a repelled jump, as on a
  real drop structure). `--view=jump` shows it side on.
- Low roughness (`--river=roughness:0.012`, supercritical everywhere): no false jumps; the fast
  reach is lace, not a white sheet.
- `--falls`: the 6 m fall and its pool are unchanged.
- Cost: bake only (once per rebuild, never per frame): the bench river's rebuild goes from 15.5 ms
  to 33.8 ms on the cloud CPU, the bisections on the real sections being most of it.

Next (stage 2, shader): standing waves `U² = (g/k) tanh(kh)` over bed features, undular jump
waves for Fr1 1–1.7, cross-waves at `sin β = 1/Fr` from bank changes, and a Froude/regime debug
overlay.

## Ocean detail: ripples, foam, bubbles (15 Nov, research for Paulo)

Paulo: the references show much more detailed crests and foam than our sea. Bench baseline
(`--view=sea --node=wave_cascades:3`, `--view=storm_low`): near water smooth and rubbery; foam flat
noise blots.

**Why it looks smooth.** The finest FFT tile is `ocean_domain_size` (40 m) on `ocean_grid_resolution`
(64 by default, raised to at most 256 by the peak rule): its Nyquist wavelength is 2 L / N, 1.25 m at
64. Nothing shorter exists, yet in the gravity range the slope spectrum is flat per octave
(Phillips saturation, `S(k) = alpha / (2 k^3)`, mean square slope per band `alpha / 2 ln(k2 / k1)`), so
the missing short waves carry about half of the sea's slope: Cox and Munk (1954) measured a total
`sigma^2 = 0.003 + 5.12e-3 U` (0.054 at 10 m/s), ours resolves well under half of it. The Ocean
Optics Web Book ("numerical resolution") shows the same: a sampled sea is too smooth unless the
unresolved slope is put back.

**What others do.**
- Cascades down to centimetres: War Thunder (NVIDIA CGDC 2015) splits the spectrum into 4 cascades
  from about 5 m to 1 km, 128 to 512 texels; OceanPro (UE5) three bands, swell, wind waves, ripples;
  HDRP a separate "Ripples" band with its own local wind; Crest adds a detail normal map.
- The rest into the BRDF: Bruneton, Neyret and Holzschuch, "Real-time Realistic Ocean Lighting using
  Seamless Transitions from Geometry to BRDF" (Eurographics 2010): waves too small for a pixel are
  filtered out of the geometry and their slope variance added to the reflectance's (Beckmann)
  roughness, so the far sea keeps the right glitter and nothing shimmers. Dupuy and Bruneton 2012 do
  the same for whitecaps.
- Foam look: Crest draws a foam texture with its own normals ("Foam Normal Strength"), and a
  separate bubble layer under it ("Foam Bubbles Color", "Foam Bubbles Parallax", "to give feeling of
  volume"). War Thunder: foam thickness from surface stretching (squeezed thicker, stretched thinner)
  and bubbles plus "milkiness" added to the refraction colour. Meta Meow (80.lv, 2024): procedural
  foam as Voronoi bubble cells, thick foam as a scattering base eroded by a height map. Sea of
  Thieves (SIGGRAPH 2018): peak foam blurred with feedback, blended with authored textures; crest
  colour from a peak mask. FluidNinja: no open-ocean foam (its strength is the live 2D solver).
- Physics of the look: decaying sea foam drains into a network of Plateau borders around ever larger
  cells (Weaire and Hutzler, The Physics of Foams, 1999), which is the lace an old whitecap leaves;
  fresh whitecaps are solid. Under a whitecap the entrained bubble plume (Czerski et al.) scatters
  strongly and glows turquoise through the water above it.

**Plan (Paulo picked all three, 15 Nov).**
1. A detail cascade: an FFT tile of `8 L / N` metres (5 m at the defaults) on 128 texels carrying the
   band from the finest cascade's Nyquist down to about 8 cm, normals only (the mesh cannot carry
   it). Faded out by the pixel's footprint, and the faded slope variance (`alpha_J / 2 ln(k2/k1)`,
   alpha_J from the fetch law) added to the roughness (Bruneton 2010), so the far sea glitters right
   instead of shimmering.
2. Foam as foam: fresh foam solid; as it fades it drains to a Voronoi network of borders (cells about
   a metre) instead of value noise; a fine bubble-cell layer (centimetres) gives it texture and its
   own normals, fading to its mean with the footprint. Thickness from the Jacobian: squeezed foam
   thicker, stretched thinner.
3. Bubbles under foam: the foam amount read again along the refracted view at about half a metre down
   (parallax), smooth, lighting the water body turquoise in raster; in the path tracer the same
   amount raises the water's scattering.

### Built (15 Nov)

- `detail_waves` (on): the detail tile `8 pi / k_min` (5 m at the defaults) on 128 texels, its band
  from cascade 0's Nyquist (k_min = pi N / L) to about 8 cm, foam pass off. In the fragment its
  derivatives join the per-pixel normal, shown by the share of the band (in log k) a pixel can hold:
  `clamp(ln(pi / (2 footprint) / k_min) / ln(k_max / k_min))`, footprint from `fwidth`. The rest of
  its slope variance (`get_detail_slope_variance()`, Phillips with the fetch's alpha) is added to the
  water's GGX alpha squared (raster reflections and the path tracer's roughness).
- Foam (`foam_structure`): holes open per Voronoi cell as the foam drains (radius per cell, two
  scales, cells warped), then broken strands along the borders; bubble cells of a few cm give grain
  and their own dome normals (`foam_bubble_normal`); thickness by the Jacobian (x1.2 squeezed, x0.85
  stretched). Every layer fades to its mean cover past the pixel footprint. The old value-noise lace
  is gone (`foam_breakup` now only drives the shore foam).
- Bubbles (`bubbles_under`): the persistent foam read along the refracted view at 0.3 and 0.7 m
  (`bubble_depth` 0.6), mixed into the body colour (both the sky's and the sun's part) as
  `bubble_color` (0.40, 0.82, 0.78) at `bubble_strength` 1.2; the path tracer's scattering gains
  `bubble_scattering` (4 /m) times the same share.
- Checked on the water bench: `--view=sea --node=wave_cascades:3`, `--view=storm`,
  `--view=storm_low`, raster before/after and path traced (8 spp on the cloud CPU, no denoiser:
  noisy, but the lace, grain and plume all show; denoised PT frames go on the PC list). Cost: one
  128^2 FFT a step and a few Voronoi lookups where there is foam; GPU timing on the PC list.

## Camera-centred water mesh, displaced ripples (15 Nov, research for Paulo)

Paulo asked for more geometry on the ocean and water bodies. Today the open sea (`render_extent` past the
simulated square) draws `_build_extended_plane`: a uniform core of `ocean_grid_resolution * mesh_detail`
cells over the simulated square (40 m / 64 = 62 cm between vertices at defaults), then square rings 15%
further apart each. The core follows the camera smoothly (no snapping, see `_update_mesh_offset`). Nothing
near the camera is finer than 62 cm, so the short waves only exist in the per-pixel normal: crests and chop
have no silhouette closer than that, and the path tracer (which traces the mesh) sees them flat.

What others do:
- Geometry clipmaps (Losasso and Hoppe 2004; GPU Gems 2 ch. 2, Asirvatham and Hoppe): nested square rings
  around the viewer, each twice the spacing of the one inside it, snapped to its own grid, with a geomorph
  band at each ring's outer edge so vertices slide onto the coarser ring's positions (no T-junction cracks,
  no pops).
- Crest (Unity, Huw Bowles, "Crest: novel ocean rendering techniques", SIGGRAPH 2017/2019): the same
  nested LOD rings, spacing doubling per LOD, geomorphing, and each ring samples only the wave cascades its
  vertex spacing can carry (shape LODs matched to geometry LODs).
- Unreal Water (UE5): a quadtree of water tiles with LOD by distance; Sea of Thieves (Rare, "The Technical
  Art of Sea of Thieves", SIGGRAPH 2018): FFT on a camera-centred, LOD'd grid.
- Projected grid (Johanson 2004): a screen-space grid projected onto the sea; uniform in screen space but
  swims badly and is poor for path tracing (it only covers the view).

Choice: keep the existing smooth-following radial mesh (its seamless, crack-free single grid already avoids
the clipmap's stitching and the projected grid's view dependence) and replace its uniform core with
geometric spacing from the camera: the step starts at `near_mesh_step` (3 cm) at the camera and grows 5% per
ring to the horizon. That is the clipmap's spacing law (spacing proportional to distance) as one continuous
grid: no LOD seams or geomorph bands to tune. At 3 cm and 5% growth a 2 km reach takes about 166 rings
(about 110k vertices), against about 23k today: a few hundred kB of vertex buffer, cheap on any GPU.
Swimming (vertices sliding over the waves as the camera moves) stays below what the eye sees as long as the
waves a ring carries are at least four of its steps long, which the next point enforces.

Displaced ripples: the detail cascade (#304, normals only) also produces height and displacement. The vertex
shader adds them where the local step can carry them, with the same log-band share as the fragment's normal
(Bruneton, Neyret and Holzschuch 2010), using the vertex step at that distance instead of the pixel
footprint, so the geometry gains the ripples near the camera and hands them back to the normal and the
roughness further out. Under path tracing the hit re-runs the vertex shader on the rest pose, so the traced
surface carries them too.

Lakes (a `surface_mesh` footprint mesh) stay on their even grid for now; the same spacing law for them needs
the footprint clipped per frame (stage 2).

### Built (16 Nov): `near_mesh_step`, off by default
Raster at 100% crop (storm_glitter from 1.4 and 2 m): almost no visible change. The waves shorter than the
old core's 31 to 62 cm are millimetres high, and the per-pixel normal already shows them; the near mesh moves
the glints a little, no more. Cost: about five times the vertices (110k against 23k); on the cloud's software
renderer a raster frame took 115 s against 75 s, a PT frame 548 s against 431 s. So it ships off; 0.03 for
cameras right at the water (a swimmer, a boat's rail), and GPU and PT checks on the PC list.

### Revised (16 Nov): never coarser than the core, about 1.5 to 2 times the vertices
The near rings now grow 8% each only until they reach the even core's spacing (domain / grid resolution),
keep that spacing out to the core's edge, then grow 15% as before: finer near the camera, nowhere coarser
than without it. Count at defaults (40 m, 64 cells): 42k vertices against 22k for a 3 km sea, 48k against
27k for 8 km. A water-level scout (eye 1 m, 5 m/s, crests against a low sun, a box cutting the surface)
showed no visible difference either, so it stays off by default.

Coverage: only the open sea mesh. Lakes (`surface_mesh` footprint) use an even grid over the footprint;
the same law would need the camera-following mesh with the footprint as a wet mask (discarding outside it)
or a per-frame re-mesh of the footprint. Rivers (`PhysXRiver3D`) build their own ribbon at the section
spacing, and `PhysXLiveWater3D` draws one vertex per simulation cell (its `cell_size`), so their geometry
is set by their simulations. For all of them the visible gap is the same as the sea's: not triangle density
but crest shape (see "Sharper crests and breaking lips").


## Sharper crests and breaking lips on the open sea (16 Nov, research for Paulo)

The finer mesh (#307, #308) showed that triangle density is not what the reference water has over ours: the
waves finer than our mesh are millimetres high. The difference is crest shape. Real wind seas have sharp,
tall crests and flat troughs, and at whitecap steepness the crest spills or plunges. Ours are linear FFT
waves with one global `choppiness`.

What others do:
- Tessendorf FFT seas (Sea of Thieves, Black Flag, Crest, Atlas) all start from the same linear waves and get
  the sharp look from horizontal (choppy) displacement, raised per sea state (Black Flag keys choppiness to
  the Beaufort level). Crest and Sea of Thieves add no breaking geometry in deep water: whitecaps are foam
  and spray. Black Flag adds spray sheets and particle splashes where crests fold.
- Second-order Stokes waves (Stokes 1847; Fenton 1985): each wave's crest is raised and its trough
  flattened by about (k a^2 / 2) cos 2 theta. In an FFT sea the second-order correction (the "bound
  harmonics"; Tick 1959, Creamer et al. 1989 transform) can be approximated from the summed slope:
  crests up by about a quarter of the local steepness squared times the wavelength. Measured ocean crests
  are about 10 to 20% higher than linear theory at Hs-scale steepness (Forristall 2000).
- Breaking in deep water: a crest breaks once its steepness passes about 0.14 (ak about 0.44; the Stokes
  limit) or its particle speed reaches the crest speed (Barthelemy et al. 2018). Most deep water breakers
  spill (a turbulent roller rides down the front face); plunging lips in deep water are rare and short
  (Duncan 1981, Melville 1982).
- Procedural breakers in graphics: Thürey et al. 2007 (a parametric curl along a wave front, for shallow
  water), Mihalef et al. 2004, Jeschke and Wojtan's water wave packets (2017) for detail, and the Fournier
  and Reeves 1986 crest skew we use on shore breakers (`shore_wave_curl`, step 5c, already plunges with a
  folded lip mesh).
- Spray sheets: Black Flag and Uncharted 4 tear camera-facing particle sheets off folding crests; our
  `crest_spray` already finds those points (the Jacobian test on the CPU readback) and throws droplets and
  spume puffs from them.

Options, cheapest first:
1. Crest sharpening (shader only): a second-order Stokes lift from the summed slope (crests higher, troughs
   flatter), and choppiness raised locally where the sea is steep rather than everywhere. Cost: a few ALU
   per vertex, no textures. One PR.
2. Spilling rollers: where a crest folds, a turbulent roller of white water riding down its front face,
   as a displaced bulge with churned foam and a short rough lip, from the same Jacobian. Shader plus the
   foam pass. One or two PRs.
3. Plunging lips and spray sheets on storm crests: lip ribbons along the folding crest lines found on the
   CPU (the shore breaker's lip mesh, reused), falling over and tearing into spray sheets. The biggest
   visual change in a gale and the most work (several PRs, a particle system per crest).

### Built (16 Nov): crests checked, spilling rollers (`roller_strength`, on)
- Crest sharpening: not needed as a separate term. The choppy FFT displaces points horizontally, a
  first-order Lagrangian (Gerstner) surface, whose Eulerian profile already carries the second-order Stokes
  crest-up, trough-flat shape (`a cos(kx) + (k a^2 / 2) cos(2kx)` up to a mean shift). Adding a Stokes lift
  on top would double it. Steeper seas remain `choppiness` and `wave_amplitude` (the Black Flag approach).
- Rollers: where the summed displacement folds (the crest foam's Jacobian test), the crest gains a mass of
  churned white water as geometry, a quarter of the crest's height at full fold (Duncan 1981: rollers are
  0.1 to 0.3 of the wave height), boiling with two drifting noise octaves and thrown a little ahead along
  the wind. The vertex stage raises it (so the path tracer sees it); per pixel its churn's slope is
  analytic (value-noise gradient; screen derivatives gave 2x2 blocks). Material uniform `roller_strength`
  (1; 0 = off). Cost: one more derivative and displacement lookup set per vertex on the open sea.

### Revised (16 Nov): rollers from the docs review
The first rollers read as snow on rocks from 8 m up: a smooth white mound with the aged lace's dark holes
in it, the foam smeared down its steep sides, and the crest spume as pasted puffs. A low view across the
wind (bench `storm_roller`, 1.75 m) showed a second fault: almost no rollers near the camera at all.
- Height from the local mean: the crest was measured from mean sea level, so the chop breaking in a swell
  trough (in a gale, hundreds of metres of sea at once) got none. The chop's crest (the two shortest
  cascades) and the swell's are now counted each from its own mean (`roller_crest`), at half that height
  on average (about a fifth of the wave's height, inside Duncan's 0.1 to 0.3).
- Lasting: the fold lasts a fraction of a second, a spilling roller several seconds. Fresh dense persistent
  foam on the crest keeps the roller going after the fold; it fades as that foam drains and as the foam
  slides off the crest.
- Gentle sides: the fold weight ramps over a wider band with a smooth step, so the mound no longer rises
  within one mesh cell (the cliffs the foam smeared down).
- Fresh right through: the roller fills the foam to solid (no lace holes); two more churn octaves (about
  13 and 5 cm) in the per-pixel slope give it a bubbly, broken surface.
- Spume sheets only from a near-gale (14 to 20 m/s, Beaufort 7 to 8), at two thirds of their old opacity.
- Eased toward 40 cm (16 Nov, later): measured from the local mean, a gale's metres-high crests raised the
  full share as a faceted white peak over a few 60 cm mesh cells. The height now eases toward 40 cm
  (`0.4 tanh(h / 0.4)`, per `roller_strength`): a lumpy churning band on the crest that reads at 1.75 m in
  the `storm_roller` view, without the peak.
- Rounded band (16 Nov, docs review of #312): the lumps still ended in cones, as the fold and the fresh foam
  are sharp at a point and the mesh raised one or two vertices under it. The roller's extent and crest are
  now blurred over five taps 1.2 m apart, centred 0.4 m upwind (lower at the front, where it spills), and
  its geometric churn is in lobes about 2 m across; the fine churn stays in the slope only. Forward throw
  0.25 of the height (was 0.4). Cost: ten more foam and height lookups per open-sea vertex, and per pixel
  under foam.
- Solid where it rises (16 Nov, docs review of #313): an aged lace hole lifted onto the band's face showed as a
  dark notch with a hard rim, since the fill only ran where foam was already above 5%. The fill is now tied
  to the vertex raise (a varying, as a share of the 40 cm cap): wherever the band rises, it is white.

## Torn spray sheets on the open sea (16 Nov, Paulo's pick)

The plunging lips (branch `claude/ocean-lips`) read as fins and logs on the FFT's gentle faces, so the open sea
finishes its breakers with what a gale shows instead: thin sheets of water torn off the folding crests by the
wind, flying downwind, tearing apart into streaks and droplets.

How it happens:
- Veron 2015 (Annual Review of Fluid Mechanics, "Ocean spray"): above about 7 to 10 m/s the wind tears spume
  off the crests directly, as sheets and ligaments that break into droplets of 20 µm to a few mm; the amount
  grows steeply with the wind.
- Troitskaya et al. 2017 (Scientific Reports 7, "Bag-breakup fragmentation as the dominant mechanism of
  sea-spray production in high winds"): filmed from 10 to 35 m/s, most spray comes from small canopies of
  water the wind lifts off the crests and inflates until they burst (bag breakup), then the rims tear into
  ligaments and drops. Each event lasts a fraction of a second.
- Erinin et al. 2019 (JFM, plunging breaker spray): the sheets and ligaments are thrown up at a few m/s and
  fall back within about a second.

What games do: Assassin's Creed IV: Black Flag and Uncharted 4 tear camera-facing particle sheets off folding
crests. We already throw droplets and spume puffs from the crests the Jacobian test finds (`crest_spray`).

What fits the fork: a camera-facing particle is a card the path tracer can only face with `billboard_particles`
(no stretch along the velocity), so the sheets are real geometry instead, rebuilt each physics tick on the CPU
like the shore breakers' lips:
- Spawned at the strongest folds of the same crest sample `crest_spray` uses, from a near-gale up
  (`smoothstep(12, 20, wind)`), 8 to 30 m in front of the camera, a few dozen alive at once (`crest_sheet_amount`).
- Each sheet is a ribbon fed from its root on the crest for about half a second: each row is the water
  that left the root at one moment, thrown downwind at 0.3 to 0.5 of the wind's speed and up at 2.5 to 5 m/s,
  then falling with the wind's push (the droplets' law), so the sheet curls over downwind and falls back. Its
  span (2 to 5 m, across the wind, jittered) stretches as it flies.
- The shader tears it: streaks along the flow, holes that open as the sheet ages, a fraying rim; the
  path-traced branch uses stochastic alpha (as the falls' sheets), the raster one blends. Thin water lit
  from behind (backlight), no shadows cast.
- `crest_sheets` (off by default until the GPU cost is measured on the PC), `crest_sheet_amount`.

## River standing waves (16 Nov, Fab idea 1 stage 2)

Stage 1 gave the river a hydraulic profile (`gradually_varied_flow`): backwater, drawdown, supercritical
reaches and jump toes with their Froude number. Stage 2 adds the standing surface patterns that profile
implies. Both are stationary in the world (the water moves, the pattern stays), so they are baked once per
rebuild.

- **Undular jumps** (Fr1 from 1 to 1.7; Chanson and Montes 1995, Chanson 2009 "Current knowledge in
  hydraulic jumps"): no roller, a train of smooth stationary waves downstream of the toe, five to ten of
  them, decaying. Their wavelength is the one whose phase speed equals the current downstream: from linear
  dispersion `U2^2 = (g/k) tanh(k h2)`, that is `Fr2^2 kh = tanh(kh)` (Lemoine 1948's linear theory, the
  classic estimate for undular jumps and Favre waves). First crest height grows with Fr1; we take
  `0.4 (y2 - y1)`, capped at 0.3 y2, decaying over about four wavelengths. Above Fr1 1.7 the roller
  (stage 1) carries most of the energy away and only a weak tail (a third) is left. Wavelengths are
  1 to 5 m, finer than the river's 1 m sections, so the train is evaluated in the shader from per-vertex
  parameters (amplitude, wavenumber, toe position, decay): height in the vertex stage, slope per pixel.
- **Cross-waves in supercritical flow** (Ippen 1951, Ippen and Dawson 1951 "Design of channel
  contractions"; the shallow-water analogue of supersonic flow): a wall that turns into fast flow by a small
  angle theta raises the depth along a front at the Mach angle `sin beta = 1/Fr`, by
  `dy/y = Fr^2 theta / sqrt(Fr^2 - 1)` in linear theory, and the front reflects off the far wall. Along
  each characteristic the disturbance is carried unchanged, so the depth at a point is the sum of the wall
  angles where the characteristics through it left the walls: from each wall directly and after one to
  three reflections (each reflection losing a fifth). Width changes of the channel set the wall angles.
  This makes the diamond patterns of fast water in narrowing and widening channels. Baked into the mesh
  heights at section spacing (their period along the river, `2B / tan beta`, is several metres). The
  section mean is subtracted so the 1D profile stays as stage 1 solved it.
- **Froude overlay**: a debug view colouring the water by its Froude number (blue subcritical, green
  near critical, red supercritical, white at jump toes), for tuning rivers in the editor.
- Built (16 Nov), `standing_waves` (on by default). On the river bench the jump below the step (Fr1 1.64)
  gets a train about 10 cm high and 1.8 m long (bench view `train`). Two fixes from the first look: waves
  shorter than six sections were raised in the mesh as faceted terraces, so they are now in the per-pixel
  slope only (the mesh carries them from six sections a wavelength, fully past that), fading at the banks;
  and the crests were ruler straight, so they now bow downstream mid-channel by a quarter wavelength at the
  banks, as in photographs of undular jumps. Bodies still ride the full train (`sample_height`).
  Cross-waves with `--narrows` (5 m to 3.5 m below the step) reach about 10 cm in the Fr 1.7 to 2.4 water,
  but that water is white from the step on this bench, so they show only in clear fast water (a smooth
  chute). `get_section()` gives `wave_amplitude`, `wave_length` and `cross_waves`.
- Later: V-shaped wakes behind rocks in fast water (the same characteristics from each obstacle), cross-
  waves in supercritical bends (Knapp 1951: they double the outer bank's superelevation).

## Lake outlets (16 Nov, Fab idea 2)

Step 4e left a lake's outlet river drawing its own `discharge` from nowhere: the lake did not drain
towards it, and its level did not answer to what flows in.

**What the hydraulics says.**
- A lake drains over the highest point of its outlet channel, the sill, as a **broad-crested weir**: the
  water speeds up to critical depth (two thirds of the head) over the crest, so the discharge follows
  from the head alone, `Q = Cd (2/3)^(3/2) b sqrt(g) H^(3/2)` with H the lake level above the sill and b
  the crest width (Henderson 1966, Open Channel Flow, ch. 6; Bos 1976, Discharge Measurement
  Structures). Cd is about 0.85 for a rough natural sill (1.0 for a smooth long crest). A 6 m wide sill
  under 30 cm of head passes about 1.4 m3/s; under 60 cm, about 4.
- **Level budget.** In steady state a lake's level sits where its outlets pass what flows in (rivers,
  plus the rain and seepage of its catchment): `sum Q_out(level) = Q_in`. Because Q grows as H^(3/2), a
  lake is self-levelling: doubling the inflow raises it by only 1.59 times the head (a flood raises a
  big lake a few tens of centimetres). Its time to settle is the volume between the two levels over the
  change in flow: hours for a small lake, days for a large one, so a steady level per inflow is right for
  a game (no level that creeps during play).
- **Approach flow.** Towards the outlet the lake's water converges as a sink in a half plane: speed
  `Q / (pi r h)` at distance r, depth h, until it reaches the river's speed at the sill. It is slow
  (a few cm/s 20 m out) but it is what carries leaves, foam and floaters to the outlet, and the
  surface lines bunching towards the lip are the visual cue of an outlet.

**Plan.**
- `PhysXRiver3D.source_lake` (a `PhysXWaterSurface3D`): the river's discharge comes from the lake head
  over its sill (the highest bed in its first 12 m, or `outlet_crest` if set), with `outlet_coefficient`
  (0.85). No head, no flow.
- The river registers its outlet with the lake (`set_outflow`): the lake adds the half-plane sink to its
  flow texture next to the inflow plumes, so waves, foam and floaters drift to the outlet.
- `PhysXWaterSurface3D.level_from_flows` (off): the lake's level settles where its outlets pass its
  inflows plus `catchment_inflow` (m3/s); the outlet rivers rebuild when it moves more than a centimetre.

**Built (16 Nov).** As planned, plus the **drowned sill**: the weir law assumes the water goes critical on
the crest, which holds only if the channel below can carry it away (steep enough). On a mild channel the
normal depth stands above critical, the sill is drowned, and the discharge is the one whose energy at the
sill (normal depth plus velocity head, over the real cross-section) the lake's level just reaches. The lake
drains by the same law (the weir coefficient scaled by the share the channel lets through), so its level
budget and approach flow agree with the river. On the river bench (`--outlet`, `--view=outlet`, a basin above
a 5 m sill on a 0.5% channel): a 0.30 m head passes 1.2 m3/s free but 0.41 drowned, and the river starts 2 cm
below the lake; with `level_from_flows` and 3 m3/s of `catchment_inflow` the lake settles 0.79 m above the
sill (outlet passing 3.04). Approach flow 2, 5 and 10 m from the sill: 14, 2.4 and 0.5 cm/s. Not yet: lakes
filling or draining over time (the steady level is applied at once), and a chain of lakes settles only
through the 1 cm rebuild loop between each pair.

## Buoyancy from shapes with densities (16 Nov, Fab idea 3)

`PhysXBuoyancy3D` floats a body on six probe points sharing a footprint area and a hull height: right
for a box-like hull, wrong for anything else (a ball, a log, a ship whose air inside is what floats it),
and the body's mass is set by hand, so whether it floats is a tuning accident.

**What others do.**
- *Pontoons* (Unreal Water's `BuoyancyComponent`): spheres placed on the hull, each pushed up by the
  water it displaces (the submerged cap of the sphere), with drag per pontoon. Simple and stable; the
  designer places them.
- *Voxels* (common in Unity water assets, and Habrador's "make a realistic boat" series): the hull's volume
  cut into small cells; each cell's submerged share comes from the water height at its centre, so a long
  hull feels the waves along its length and a tilted body is righted by its own geometry.
- *Exact waterline* (Kerner 2015, "Water interaction model for boats in video games", Gamasutra; Just
  Cause 3): every hull triangle clipped against the water, pressure integrated over the wet part, plus
  slamming and viscous terms. The best for hero boats; heavy for debris.
- *Planing* (Savitsky 1964): lift on a fast hull rising with speed squared. For speedboats; not now.

**Plan.** A `shapes` mode beside the probe points: the body's collision shapes are its volume.
- Spheres are pontoons: the exact submerged cap, `pi x^2 (3R - x) / 3` for a depth x into the sphere,
  pushed at its centroid.
- Boxes, cylinders and capsules are cut into cells (up to `max_cells` a body, 32 by default, more along
  the long axis), each the exact share of the shape's volume; a cell's submerged share is its depth into
  the water at its centre over its vertical extent, clamped. Cells in one column share a water sample.
- Other shapes: their bounding box at three quarters' fill, as now.
- Each `CollisionShape3D` may carry a density (`buoyancy_density` metadata, kg/m3): wood about 500, ice
  917, steel 7850, 0 for air (it displaces water but weighs nothing, so a steel shell around an air box
  floats). With `mass_from_densities`, the body's mass and centre of mass come from them; otherwise the
  shapes share the body's mass. Then a log floats with half its volume out, a ball of ice with a tenth,
  and a steel block sinks, with no tuning.
- Drag per wet cell, on the cell's frontal area (its volume to the two thirds), against the flow, as now.
- Later: the exact waterline for hero hulls (Kerner), planing lift (Savitsky), ragdoll bones' shapes.

**Built (16 Nov).** `volume_mode` Shapes on `PhysXBuoyancy3D`, with `max_cells`, `mass_from_densities`,
`default_density`, and two damping terms the first runs showed were missing: a body dropped 1 m dived metres
under (the probe mode's linear drag is tuned for floating, not diving), so **form drag** `0.5 rho Cd A |v| v`
(`drag_coefficient`, 1), and floating bodies bobbed for over 15 s, so **wave radiation damping** (`wave_damping`,
0.2 of critical for the waterline stiffness `rho g A_wp`, spread over the waterline; Faltinsen 1990 gives 0.1
to 0.3 for barge-like hulls). Bench `docs/bench/buoyancy --report` after 14 s on calm water (`--still`):
wood box 0.500 submerged (expect 0.500), ice ball 0.920 (0.917), log 0.600 (0.600), cork capsule 0.240
(0.240), steel hull (a 2 cm steel shell round a 4 x 1 x 1.6 m air box, 2763 kg) 0.409 (0.409), steel block
sinks. With the bodies dimpling the water, within 3% (their own dimple lowers them a little); in 6 m/s wind
waves within 4%. The probe mode is unchanged and stays the default.

## Lamp beams under water, raster (16 Nov, Fab idea 4)

The path tracer scatters every light in the water: a diver's torch or a submerged lamp draws a beam in the
murk. The raster underwater pass scatters only the sun and the sky, so lamps under the water light what they
hit but leave the water between dark.

**What others do.**
- Froxel volumetric fog (Wronski 2014, Assassin's Creed IV; Hillaire 2015, Frostbite): every light injected
  into a camera-aligned 3D grid, integrated along the view. Godot's Volumetric Fog is this, but its medium is
  grey (one density, one albedo), and water absorbs strongly per channel (red is gone within metres), which is
  why the underwater medium is our own pass (see "Shadows in the water").
- Analytic in-scattering of a point light (the "airlight" integral): without extinction the single scattering
  along a ray past a point light has a closed form in arctangents (Macklin 2010, "Faster fog"); with
  extinction, Sun, Ramamoorthi, Narasimhan and Nayar 2005 ("A practical analytic single scattering model")
  tabulate it. Water's extinction is too strong to drop, and spot cones and the Henyey-Greenstein phase
  break the closed forms.
- Ray marching per light inside its range (Unity HDRP's and many engines' local volumetric lights; Crest's
  underwater lights): a few jittered samples on the part of the ray inside the light's sphere, each taking the
  light's attenuation, its cone, the phase, and the water's extinction both ways. Cheap for a handful of lamps.

**Plan.** The underwater pass takes up to four lamps: the `OmniLight3D` and `SpotLight3D` nodes under the
surface nearest the camera, found every half second. For each, the view ray is clipped to the light's range
sphere and marched in 12 jittered steps; each step adds `sigma_s HG(cos) E exp(-sigma_t (r + t))`, E the
light's irradiance there by Godot's own falloff (`(1 - (r/range)^4)^2 r^-decay`, pi x energy x colour, the
spot's cone and rim), so the beam matches what the lamp lights on surfaces. No lamp shadows in the medium yet
(a rock does not cut a beam).

**Built (16 Nov).** As planned (`uw_lamps()` in the underwater fog and underside passes; the node keeps the four
nearest lamps under the surface, rescanning the tree every 30 frames while the camera is under). Bench
`docs/bench/water_surface --view=submerged --lamps --sun-energy=0.15`: an omni lamp now glows in the water and a
spot's lit patch on the floor sits under a faint cone, as in the path-traced frame; the path tracer's halo is
wider (it scatters more than once; raster is single scattering). A spot seen side-on stays faint in both: the
water's phase (Henyey-Greenstein, g 0.85) sends little light sideways. Cost: 12 steps a lamp a pixel, only
where the ray crosses the lamp's range. Not yet: lamp shadows in the medium, lamps seen through the surface
from above.

## Waterfall sheets near black in shade, path traced (16 Nov, bug)

The big fall's sheet read nearly black in deep shade under path tracing (a tenth of the rock beside it) and white
in sun. Reproduced on the valley's 336 m fall from below (`--time=16.5 --cam=620,-60,-1380 --look=545,112,-1470`):
sheet 1.0, rock 9.1 (sRGB mean); raster 44.

- **Cause.** The contact sheet's traced material wrote a fractional ALPHA, so the tracer took it as a blended
  layer (a transparency peel): such a layer is shaded with its direct light and goes on to indirect light only
  with a chance, which in shade, with no sun on it, left it close to black. The mist, the bounce count (2 or 6)
  and the falling clumps made no difference; making the sheet opaque did.
- **Fix: stochastic transparency** (Enderton et al. 2010), as the falling clumps already do: each 2.5 cm cell of
  the sheet is water or nothing, with the cover as the chance (`ALPHA_SCISSOR_THRESHOLD`), so the sheet is an
  opaque surface lit by the sky and bounces like the rock beside it; the samples of a pixel average to the same
  cover, wet rock included. Same frame after: sheet 45, rock 9.1 (raster 44).
- **Also wrong, fixed with it.** The sheet and the river's falling ribbon wrote IOR and ABSORPTION, making each
  the boundary of a medium: a path that went in through the water and left through one of the sheet's gaps
  (threads, tongues, wet rock) never left the water, and every later segment was absorbed as if under water. Both
  are thin walls now (thin-walled transmission, as BaseMaterial3D's Transmission with thin_walled on): straight
  through, tinted by `exp(-absorption * thickness)` (`film_thickness`, `sheet_thickness`, 0.1 m). A film
  centimetres deep bends a ray by less than its own thickness, so the refraction was invisible anyway. On the
  river bench's fall in shade (`--falls --view=fallfront --sun=-35,180`) the sheet matches, and a cyan glow along
  its lip, light trapped in the medium, is gone.
- The river bench takes `--fall-run=M` (a sloping face) and `--sun=P,Y`.

## Crisp foam on the open sea (16 Nov, Paulo's pick: "all three, crispy foam first")

Paulo asked whether the sea surface could be better, and for "crispy foam". Next to Sea of Thieves, Black
Flag, Atlas and Crest our foam reads soft: a blobby outline (the foam amount comes from a low-resolution
texture, bilinear), a translucent fringe (thin foam fades in over amounts 0.02 to 0.35), and a grey, noisy
fill (the bubble grain darkens it to about 0.6). The holes and strands themselves are already sharp.

What the references do:
- **Threshold the amount against a fine pattern** (Sea of Thieves, Ang SIGGRAPH 2018; Atlas, GDC 2019;
  Unreal's water foam mask): the simulation says how much foam, a high-frequency pattern says where, and a
  narrow step between them gives a hard, pixel-sharp edge whatever the simulation's resolution.
- **Foam has thickness** (Crest "foam 3D lighting"): a normal from the foam's gradient, so the rims of
  patches and holes catch the sun on one side and darken on the other. It reads as a raised white skin on
  the water, which is most of what "crisp" means at a distance.
- **White, not grey**: the grain is a small modulation on a bright sheet.

Plan (shader only, no new textures):
1. `foam_crisp` (0 to 1, default 1): the outline is a threshold of the amount roughened by fine noise,
   with a step about a pixel wide (from the footprint), so edges are crisp and ragged, not blobby; thin foam
   is a sparse but opaque web rather than a translucent haze; the grain darkens at most 20 %.
2. `foam_relief` (0 to 1, default 0.5): the foam's screen-space gradient (dFdx/dFdy of the coverage, solved
   for world xz) tilts the normal as a 3 cm thick skin, so rims light up toward the sun.
Cost: two extra noise reads per foam pixel and two derivatives; no extra texture reads.

Built (16 Nov): a hard threshold of the amount (first try) lost the faint haze and scattered specks over clear
water; a threshold spread evenly over 0 to 1 (per cell) kept the share but turned thin foam into square flakes.
Kept: the density stepped against a ragged threshold near 0.32 for the patches' edges, max'd with 0.8 of the
soft density so the haze stays. Relief from screen derivatives as planned.

Docs review: at normal viewing distance off and on were near identical: past a few metres the structure's detail
is under a pixel and the edge softens again. Fix: after the foam is read, its edge is stepped once more at the
pixel's own scale (fwidth of the foam, as signed-distance text is sharpened), keeping 0.8 of the soft value.
Bench `--view=storm_foam_close`: 1.5 m above gale foam, riding the sea, low side sun.
Check: off/on pair at whitecap distance (storm_foam) and close (storm_plume), PT frame.

Docs review 2: straight seams and polygonal patches up close, plus near-round holes. Ruled out by bench tests:
detail waves, breaker lips, underwater passes, spray, wave shadows, depth order (depth_prepass_alpha,
depth_draw_always), mesh folds (a Jacobian limiter), the shadow map and the sun share (light() split into its terms).
Cause: each hole's radius came from the nearest Voronoi cell only (Worley's F1 with a per-cell value), and a hole
wider than its cell was cut along the straight bisector to the next cell. Fix: every feature of the 3x3
neighbourhood carves its own hole (the usual fix for per-cell radii; a radius under one cell keeps the 3x3 search
exact), min'd together, with the edge wandered by lobe noise in proportion to the radius at all three scales.

## Terrain holes and shore queries (16 Nov)

Terrain holes (TerrainGenerator3D add_hole, cave mouths) leave the heightfield's collision open (NaN heights), so the
sea's seabed rays fell through onto nothing and read the constant depth: open water showing through a hole above
the waterline. `terrain_source` names the terrain: a ray that finds nothing where `is_hole()` is true takes the
terrain's own height (`get_height`), and a static floor under the hole (a sea cave's, later) is used as it is. The
seabed is measured again when `holes_changed` names a rectangle over the simulated square. Rivers and live water
already treat a ray that finds nothing as dry.

For the sand's shore coupling (ground stage 4): `sample_depth()` (the drawn surface over the measured seabed) and
`sample_swash()` (the swash texture's run-up film and wet sand, read back every 8th step once asked for).
Test: `docs/bench/water_surface/holes_test.gd` (-s).

Docs review 3 (close frame): near holes were clean ovals, one wedge, stair-stepped rims with a dark outline.
- Holes: each layer warps its own cell space by noise a few times finer than the cells (film rupture stretched by
  surface strain: the walls between holes bend, no straight border is left) and tears each edge in lobes plus a
  finer tear, both in proportion to the hole's size; the radius cap drops to 0.65 cells so a hole's reach with its
  tear stays inside the 3x3 search (a larger one was cut straight where the search ended: the wedge).
- Rafts: unruptured film left as islands well inside the larger holes (only where the point is deep in a hole, so
  no hairline gap at the rim), faded before they shrink to pixel specks.
- The foam amount is a bilinear texture: up close its contours run straight across each texel. Its lookup is
  wandered by smooth noise about a texel long.
- Rims: the pixel step spans about three pixels (fwidth is per 2x2 quad), and the relief reads the foam before the
  step (the step's one-pixel jump tilted the normal hard over every rim: the dark outline).

## Short-crested shore waves (16 Nov)

Sand's shore frame showed a straight line through the surf foam, parallel to the beach: the plunging breaker's
crest and the trough where its lip lands, both exactly long-crested over a straight beach (one direction per train,
the eikonal phase from a plane wave). Real swell spreads over a range of directions (directional spreading, e.g.
Longuet-Higgins' cos^2s), so crests are short: they bend, and a breaker peels and breaks unevenly along its length.
Fix: the baked phase wanders by up to about a fifth of a wave, slowly along the crest (1.5 wavelengths) and more
slowly across it (4 wavelengths); the gradient (direction of travel) follows it, and the CPU lip sheets and
sample_height read the same baked phase, so the lips stay on their crests.

## Crest glow (raster, 16 Nov; Paulo's pick 2 of 3)

Research: Atlas (GDC 2019, "Wakes, Explosions and Lighting: Interactive Water Simulation in Atlas") approximates
light scattered through a crest as k1 H <L,-V>^4 (0.5 - 0.5 L.N)^3 + k2 <V,N>^2; Crest and Sea of Thieves use the
same shape of term (a forward lobe toward the sun, weighted by how far the face turns from it and by height).
The path tracer already scatters through crests, so this is raster only.
Built: light() adds LIGHT_COLOR * ATTENUATION * colour * <L,-V>^4 (0.5 - 0.5 L.N)^3 for the sun, not through the
wave self-shadow (the light crosses the crest on purpose). The colour is the water's single-scattering albedo
(pathtraced_scattering / sigma_t, so it matches what the path tracer scatters) times crest_glow, the height above
still water (smoothstep 0..1.5 m, from the vertex), the open water's share (no foam, sand, swash or lips).
The k2 <V,N>^2 term is left out: the open water's body light (sun_body) already carries it.
Bench: water_surface --view=storm_sun --sun-pitch=10 (into a low sun); crest_glow:0 for off.

### Round 2: only the thin top of the crests (16 Nov, docs review)

Docs declined round 1: the glow was a broad tint over whole mid-field wave faces. The fixed height window
(0 to 1.5 m above still water) caught half of a 2.5 m sea. A test that weighted by the length the sun's ray crossed
in the water (the self-shadow march) was no better: at a 10 degree sun nearly every lee face is crossed by a
short path, so it stayed broad, and the crest tops (where the ray crosses nothing) lost the glow entirely.
Built: Atlas's height term made relative to the chop itself. v_crest is the height of the two shortest cascades
from their own mean (as roller_crest()): a crest thin enough to pass light is a wave of the chop, wherever the long
swell under it puts it. The C++ measures that chop's rms height from the CPU grids it already reads back
(`crest_sigma`, a few times a second; the JONSWAP integral over the same bands until the first readback), and the
glow weight is smoothstep(1.5, 2.5, height / sigma): a thin line along the high crests' tops, in a calm or a gale
alike. The whole sea's sigma (1.5 m in the bench gale) was the wrong scale: the long swell carries most of it,
and no chop crest reached the threshold. 1.0 to 2.0 sigma still lit whole wave faces; 1.8 to 3.0 lit nothing.
Bench: water_surface --view=storm_crest (camera 1.3 m up riding the sea, sun 10 degrees up straight ahead, clear
sky with a crisp disc); --frames=420; --water=crest_glow:0 for off; --node=caustics_enabled:false
--node=crest_sheets:false keep the cloud renders quick (neither shows here).

## Everything on by default (16 Nov, Paulo's standing rule)

Paulo: every feature ships on; the desktop trims cost later, and tests switch off what they do not need. On now:
`crest_sheets`, `caustics_enabled` (raster; PT caustics were already on), `shore_waves`, and on WeatherMap3D
`cloud_evolution` and `aerosol_haze`. Shore waves on by default meant every lake would get the open sea's 0.8 m
swell, so enclosed water (a surface_mesh outline) now takes at most its own wind sea's rms wave (H_rms = 2 sqrt 2
sigma from the fetch-limited JONSWAP integral), re-baked when the wind moves it by a tenth; a still pond gets none.
Left off: ReSTIR DI and GI (`light_reuse`, `indirect_reuse`) until they are checked under PT with Ray Reconstruction
on the PC (the cloud has no denoiser); `follow_weather`, `open_sea_beyond`, `seabed_from_floor`, `level_from_flows`,
river `carve`, spray `water_collision` and buoyancy `mass_from_densities` are scene wiring, not features (each
changes what an authored scene means: its wind, its edge, its level, its ground).

## Far-sea filtering on every cascade (16 Nov, Paulo's pick 3 of 3)

Research: Bruneton, Neyret and Holzschuch 2010 ("Real-time Realistic Ocean Lighting using Seamless Transitions
from Geometry to BRDF"): waves smaller than a pixel leave the normal and go into the BRDF's roughness as slope
variance, so the far sea keeps its glitter width and nothing aliases. Only the detail band did this so far.
Built: each cascade knows its band of wavenumbers (`cascade_k_lo`/`cascade_k_hi`) and its mean square slope
(`cascade_variance`: the Phillips saturation range, 0.5 alpha ln(hi / max(lo, k_peak)) amplitude squared, with
k_peak = 0.77 g / U^2). The fragment finds the wavenumber a pixel can still show from its footprint
(fwidth of the rest position), keeps the shown fraction of each cascade's slope (in log k) in the normal and adds
the hidden fraction's variance to the roughness (sqrt(sqrt(a^2 + hidden))). Foam still reads the full field.
`far_filter` (shader uniform, on) turns it off for comparisons.
Status: in every crest glow render (on); a dedicated far-sea before/after and PT pair are next.

### Round 3: artefacts in the low-sun frame (17 Nov, docs review)

Docs passed the glow but declined the frame for artefacts. Found and fixed:
- Dark hard-edged holes in the sun's path: the raster glitter was D F / (4 n.v) with no masking term, so it
  peaked as a facet turned from a low sun and then cut to nothing at n.l = 0. Now the height-correlated Smith
  visibility (Heitz 2014) times n.l, which fades both ways (water_raster_sun_light()).
- Thin bright posts along distant crests: the far rings of the open sea mesh (steps growing 15% a ring) took the
  short chop at their sparse vertices as random spikes, whose faces caught the sun. Each vertex now keeps only the
  share of each cascade its step can carry (four steps a wavelength, far_shown() at the step), on the open mesh
  with or without a near step; the normal and roughness keep the rest (Bruneton 2010).
- Aliased horizon dashes and stair-stepped foam rims: the bench rendered without anti-aliasing; storm_crest
  shots use --taa (as the game does), and far_filter keeps the far glitter as roughness.
- The sun: a warm (1.0, 0.84, 0.64) disc 0.8 degrees across with a tight glow, not a grey blob.

## Far sea: the mean Fresnel of the hidden slopes (17 Nov)

Checked `far_filter` against a path-traced frame of the `sea` view. The path-traced far sea is darker than
either raster frame (rows just under the horizon about 92 against 135 with the filter off and 151 on). With
the filter on, the normal near the horizon is nearly level, so the mirrored sky took the full grazing
Fresnel (close to 1) and the far band turned a pale grey sheet. Scaling the hidden variance changed nothing:
the raster sky mirror ignored roughness.

Fix (Bruneton, Neyret and Holzschuch 2010, section 4: the reflectance is the Fresnel averaged over the
visible slopes, not the Fresnel of the mean normal): `water_raster_reflect` takes three Gauss-Hermite slopes
along the view's tilt (0 and plus or minus sqrt(3) sigma, weights 2/3, 1/6, 1/6), sigma from the pixel's
roughness (the hidden variance included), each weighted by the area it turns toward the eye (n_k.v over
n_k.n). Facets facing the eye mirror less and a higher sky, so the band under the horizon drops to about 118,
nearer the path-traced frame, and keeps the swell's dark troughs. Near water is unchanged (its roughness is
small). Cost: two more panorama reads per raster water pixel.
