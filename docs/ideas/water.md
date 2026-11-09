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
   it for all of them (issue #169).

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

## Sources

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
