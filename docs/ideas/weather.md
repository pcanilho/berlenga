# Weather: clouds, sky, wind, wetness, snow, fog, lightning

Research note, 7 Nov 2026. Which published methods fit an open world on this
fork. Some talks could only be read as abstracts; those are marked
(abstract).

## Published methods

### Volumetric clouds: Guerrilla's Nubis

Horizon Zero Dawn, SIGGRAPH Advances 2015.

- **Shapes** come from noise, not hand placement: a 128^3 Perlin-Worley base,
  a 32^3 Worley detail texture and a curl-noise texture.
- **Weather map**: one texture drives everything. Red is coverage, green is
  precipitation, blue is cloud type. Cloud type blends three height profiles.
- **Raymarch**: 64 steps, up to 128 toward the horizon. Steps stay cheap until
  they reach cloud, then full detail. Six cone samples toward the sun.
- **Lighting**: Beer's law, Henyey-Greenstein phase, and a "powder" term for
  dark edges.
- **Cost**: about 20 ms naive. Rendering at quarter resolution and updating 1
  pixel in 16 per frame, with reprojection, brought it to about 2 ms on PS4.

Nubis Evolved, Horizon Forbidden West, SIGGRAPH 2022 (abstract): clouds you
can fly through, superstorms, and lightning flashes inside clouds at "near
zero cost".

### Sky and atmosphere: Hillaire, EGSR 2020

Used in Unreal. Read from memory; the paper did not load. Small lookup tables
are rebuilt each frame:
- transmittance
- multiple scattering
- sky view
- an aerial perspective froxel volume

This gives a physically based sky at any time of day and altitude, plus
distance haze over terrain, at low cost.

### Wind, clouds and haze: Ghost of Tsushima, Advances 2021 (abstract)

- One wind system drives grass, trees, particles, clouds and haze.
- Sky irradiance probes come from sky visibility.
- Multiple scattering for haze, clouds and particles.

### Wetness and snow: The Last of Us Part II, Advances 2020 (abstract)

- Surface wetness as a material feature.
- Deformable snow, where characters leave trails.

Studio Gobo (2015): snow sparkle on a jittered 3D grid, anti-aliased so it
does not shimmer.

## Where we are

The current game already has these pieces, in game code:
- one storm rain source with rain rate R (Marshall-Palmer)
- one wind field (Kaimal spectrum, Shinozuka synthesis, gusts) driving rain,
  tape, cloth and iWave
- wet surfaces and puddles from iWave and street water
- lightning in the office scene
- traced haze through Godot's froxel fog, which the path tracer applies to
  camera rays

All of these are fixed per scene. An open world needs them to change across
space and time.

## What to take

| Item | Take | Game |
|---|---|---|
| Weather state | One weather map in the style of Nubis (coverage, precipitation, type), moving with the wind. It drives clouds, rain rate R, wetness, snow and wind strength in one place. The current storm node becomes a reader of it. | Open world |
| Clouds | Start from an existing Godot addon if the open-world project already uses one (SunshineClouds 2, a Godot 4 volumetric cloud compositor effect, is a candidate) rather than writing our own; otherwise Nubis-style raymarched clouds with temporal reprojection. Under path tracing, camera and bounce rays that reach the sky read the cloud layer. A cloud shadow map darkens the ground and sets sun visibility for next event estimation. | Open world |
| Sky | Hillaire's LUT sky and aerial perspective. Under path tracing, use the sky view table as the environment, importance sampled (the "sky sampling" item already on the Valence list). | Open world; distance haze helps both |
| Wind | Grow `wind.gd` into a world wind texture around the camera, read by foliage, particles, cloth (PhysX), clouds and water. | Both |
| Wetness | A wetness value per surface type that rises with R and dries over time, by porosity: darker albedo, lower roughness, puddles in low spots. It replaces per-scene flags. | Both |
| Snow | Accumulation from a top-down occlusion map (no snow under roofs or trees). Deformation from a render target of character and vehicle trails, as in The Last of Us Part II. Snow sparkle as in Studio Gobo. | Open world |
| Lightning | A short-lived emissive light in the path tracer (a bright directional or area light inside the cloud). Nubis-style internal cloud flashes. The office's raster flash keeps working. | Both |
| Fog | Froxel fog already traced. Add Hillaire aerial perspective for kilometre-scale distance, and height fog from the weather map. | Open world |

## Suggested order (open-world game, after the current engine list)

1. Weather map and world wind texture: the shared driver everything reads.
2. Hillaire sky and aerial perspective, sky importance sampling in the path
   tracer.
3. Clouds with a cloud shadow map.
4. Wetness model that replaces per-scene flags.
5. Snow accumulation and trails.
6. Lightning in clouds under path tracing.

## Step 1: the weather map and world wind (10 Nov 2026)

Research for this step, on top of the notes above.

- **Far Cry 6** (Ubisoft article and GDC 2022 abstract, Weick and Zhou): a
  weather manager reads hourly presets per region from a forecast, with
  per-region min/max curves. Static wetness is masked by a top-down "wetness
  shadow map"; dynamic objects ray cast for rain exposure. Puddles are terrain
  decals that grow from their centre. Indirect light fades with cloud cover.
  No numbers for transitions or drying.
- **Assassin's Creed 4** (Wronski, GDC 2014): rain occlusion from a 128x128
  top-down depth map (shared with world AO); wetness darkens albedo and raises
  gloss in the lighting pass; rain ripples in a 256x256 wrapped heightfield
  (about 0.6 ms); up to 320k GPU rain drops pushed by wind. The weather state
  machine itself is not in the slides.
- **Ghost of Tsushima** (Rockenbeck, GDC 2021, via Game Developer): one global
  wind vector plus Perlin noise, plus "vorticles" (small spheres of wind) that
  particles sum by brute force, hundreds at a time; no fluid solve. Grass reads
  a shared 2D noise wind (Wohllaib, GDC 2021).
- **God of War** (Renard, GDC 2019): a full 3D GPU fluid simulation of wind
  with motors (abstract only; the talk is members-only).
- **Godot addons** (code read): TCA_Weather_System keeps 0-1 floats (rain,
  snow, fog, cloud cover, wetness) and one wind vector with sine gusts, pushed
  per material with `set_shader_parameter`; presets snap, nothing is spatial.
  GodotWeatherSystem picks weighted random weather resources; Sky3D has no
  weather or wind. None of them is a map.
- **Wind science.** Mean wind with height: the log law
  u(z) = u10 ln(z/z0) / ln(10/z0), z0 the roughness length (0.0002 m sea,
  0.03 m open land, about 1 m city). Gusts: the Kaimal spectrum of IEC 61400-1,
  f S(f)/sigma^2 = 4 f L/U / (1 + 6 f L/U)^(5/3), with sigma_v = 0.8 sigma_u,
  sigma_w = 0.5 sigma_u and turbulence intensity I = 1/ln(z/z0); turned into a
  field by Taylor's frozen turbulence (the gust pattern drifts with the mean
  wind), as the game's `wind.gd` does in time only. Storm outflow: the
  Oseguera-Bowles downburst profile (NASA TM-100632, 1988), radial speed
  proportional to (R/r)(1 - exp(-(r/R)^2)).
- **Drying.** Penman's (1948) open-water wind function,
  E = 0.26 (1 + 0.54 u) (e_s - e_a) mm/day with vapour pressures in hPa and
  e_s from the Magnus formula: a 0.5 mm film dries in about 3 to 4 hours at
  15 C, 70% humidity and 3 m/s.
- **Sea growth.** Waves take time to grow to the fetch-limited sea: the Shore
  Protection Manual's duration, g t/U = 68.8 (g F/U^2)^(2/3).

What everyone shares: one place holds the weather, everything reads it. What
nobody here does: let it vary across the world. Nubis's map does, for clouds.

### What we build

`WeatherMap3D` (a new `weather` module, so every platform has it): a square
map, `extent` metres wide at `resolution` cells, that follows the camera (or a
target node) and snaps to its cells, Nubis-style.

- **State per cell**, refreshed on the CPU at `update_rate` Hz: mean wind at
  10 m (x, z), cloud cover, cloud type, rain rate (mm/h), and two quantities
  that integrate over time: the water film (mm) and puddle depth (mm).
  Wetness is the film over the surface's storage (`surface_storage`, mm);
  past it, water collects as puddles, which drain at `puddle_drain_rate` and
  evaporate.
- **Where the state comes from:** the base weather (`wind_speed`,
  `wind_direction`, `cloud_cover`, `cloud_type`, `rain_rate`, `temperature`,
  `humidity`), which the map eases toward over `transition_time`; cloud
  cover broken up by noise drifting with the steering wind; an optional
  painted map (Nubis channels: R cover, G rain, B type) over a world rect; and
  storm cells (`add_storm`) that drift with the steering wind and carry a rain
  core and a downburst outflow.
- **Gusts** are not stored: they are a sum of Kaimal-weighted waves drifting
  with the mean wind, the same few numbers on the CPU and in shaders, so
  every consumer sees the same gust at the same place and time.
- **Time:** `time_scale` makes weather run faster than the clock (a game day
  in an hour); drying, wave growth and storm drift use it.
- **Out to everything:** global shader uniforms (`weather_wind_map`,
  `weather_state_map`, `weather_map_rect`, `weather_gusts`), so any shader,
  raster or path traced, reads the same weather; and `get_wind_at()`,
  `get_rain_at()`, `get_wetness_at()`, `get_puddle_at()`, `get_cloud_cover_at()`
  for scripts and C++.
- **First readers:** PhysXWaterSurface3D (`follow_weather`: wind speed and
  direction at the sea, with duration-limited wave growth), PhysXCloth3D and
  PhysXHair3D (their wind, when a map exists), and a bench material that
  darkens and glosses with wetness (to check raster against path tracing).

Not in this step: clouds and their shadows (step 3), the sky (step 2), rain
occlusion under roofs (the AC4 / Far Cry 6 top-down map, with step 4),
snow, lightning.

### Checks (docs/bench/weather, `--test`)

- Log law, u10 8 m/s over z0 0.03 m: 5.79, 8.00 and 9.91 m/s at 2, 10 and
  40 m (formula 5.78, 8.00, 9.91).
- Gusts at 10 m over 60 s: sigma_u / U 0.138. The IEC value 0.165 is for a
  10 minute record; a minute holds 70% of the Kaimal variance
  ((1 + 6 n)^(-2/3) at n = L / (U 60 s)), which gives 0.138. sigma_v / sigma_u
  0.86 (0.8), sigma_w / sigma_u 0.57 (0.5); peak 10.7 m/s on 8.
- Storm, R 1000 m, outflow 15 m/s: 0, 10.4, 14.9, 15.0, 14.0 and 11.5 m/s at
  0, 500, 1000, 1121, 1500 and 2000 m.
- An hour of 5 mm/h leaves the film full and 2.7 mm of puddle (5 in, 0.5
  held, 2 drained); at 15 C, 70% and 3 m/s the puddles go in 1.3 h and the
  surface is dry after 5.3 h (Penman 0.12 mm/h).
- Sea after the weather jumps from 4 to 15 m/s over 20 km of fetch (SPM 2.7 h
  to full growth): its wind is 8.7, 10.6 and 12.2 m/s after 20, 60 and 120
  minutes.
- One map update, 128 x 128 cells and a storm, rows on the worker threads:
  about 1 ms on the 4 cloud cores (4 updates a second by default).
- The wet street (bench `--view=street`): raster and path traced frames
  match at 100% crop; path tracing adds the reflections in the puddles.

## Step 1b: wind near the ground (10 Nov 2026)

The weather map holds the mean wind in 32 m cells and gusts as long Kaimal
waves. Vegetation needs two more things: wind that drops inside a forest and
behind buildings, and small gusts that sweep across a field.

### Published methods

- **Inside a canopy**, the mean wind falls off exponentially from its top:
  `u(z) = u(hc) exp(a (z / hc - 1))` (Inoue 1963; Cionco 1965). Cionco's
  attenuation coefficient `a` runs from about 1 for open orchards to 2 to 3
  for crops and most forests and about 4 for dense stands (Cionco 1978).
- **Above a canopy** the log law still holds, but from a displaced ground:
  `u(z) = u*/k ln((z - d) / z0)`, with `d` about 0.7 `hc` and `z0` about
  0.1 `hc` (Raupach 1994).
- **Turbulence in canopies** is driven by coherent sweeps from the shear at
  the canopy top, not by the mean wind (Finnigan 2000; the mixing-layer
  analogy of Raupach, Finnigan and Brunet 1996). The sweeps travel at about
  1.8 times the wind at the top, and they are what makes honami, the waves
  that run across a crop (Inoue 1955; Py, de Langre and Moulia 2006). At
  the top of a crop the speed varies by 0.3 to 0.5 of its mean.
- **Behind an obstacle** there is a recirculating cavity about 1.5 to 2.5
  heights long (Hosker 1984). Behind dense windbreaks the wind is 0.6 to 0.8
  of the open wind at 10 heights and nearly all of it by 20 (Heisler and
  DeWalle 1988).
- **Games**:
  - Ghost of Tsushima (Wohllaib and Rockenbeck, GDC 2021) and Horizon Zero
    Dawn (van Muijden, GDC 2017) push grass with wind noise that travels
    across fields, so gusts are visible as patches.
  - Unreal's wind source and Unity's wind zones are uniform with pulsing
    gusts. Neither has shelter unless artists place local wind volumes.

### As built

- **A shelter square**:
  - It is 512 m wide with 2 m cells and follows the camera.
  - Per cell it holds canopy height, Cionco's `a`, the lee factor, and the
    height of the obstacle that sets the lee.
  - It rebakes when canopies or obstacles change, when the camera has moved
    64 m, or when the wind turns by 3 degrees. A bake with one wall takes
    7.6 ms on the cloud CPU.
- **Canopies** come from `add_canopy()` (round stands) or a `canopy_map`
  image over a world rect, for vegetation that places whole forests.
  - Inside a canopy: Cionco's profile.
  - Above it: the displaced log law, which meets the open-ground wind at a
    blending height of 60 m (or 4 `hc`).
- **Obstacles** come from `add_shelter_box()` and from visible nodes in the
  `weather_shelter` group.
  - Each cell marches upwind and keeps the strongest lee: 0.15 of the open
    wind through a 2-height cavity, then recovery over 6 heights.
  - The lee fades out between one and two obstacle heights.
- **Gust patches** are two-octave value noise of `gust_patch_size` (6 m)
  that travels at 1.8 times the 1 m wind.
  - They scale the mean so its spread is `gust_patch_strength` (0.35).
  - They fade out above twice their size, where the Kaimal gusts carry the
    turbulence alone.
  - Under shelter, the gusts scale with the square root of the shelter
    factor, since turbulence drops more slowly than the mean.
- **Shaders get the same wind.** `get_wind_shader_code()` returns
  `weather_wind(world_pos, height)` over the global uniforms. It uses the
  same noise bits (an integer hash with a 1024-cell period) and the same
  formulas as `get_wind_at()`.

### Checks (docs/bench/weather, `--test=shelter`)

- Forest 20 m tall with `a` 2.5, at 8 m/s:
  - At 2 m: 0.36 m/s (open ground: 5.78 m/s).
  - At 20 m: 3.41 m/s (open ground: 8.95 m/s).
  - At 80 m: the same as open ground.
  - Inside, the profile matches `exp(a (z / hc - 1))` to three decimals.
- A 10 m wall, wind at 2 m:
  - 1.00 two heights upwind.
  - 0.15 at 1 height downwind.
  - 0.49 at 5, 0.78 at 10, 0.96 at 20.
- Gust patches at 2 m: the spread is 0.35 of the mean. The correlation is
  0.93 at 1 m, 0.36 at 4 m and 0 by 8 m, so the patches are about 6 m
  across.
- The shader code against `get_wind_at()` at 25 points, with gusts,
  patches and a wall: 0.009 m/s mean difference, 0.014 m/s at worst.

Limits:
- The lee runs straight downwind, with no spreading of the wake and no
  speed-up over roofs or round corners.
- Canopies are uniform per cell, with no gaps from single trees.
- The shelter does not move with moving obstacles until `rebake_shelter()`.

## Step 3: clouds and cloud shadows (10 Nov 2026)

Research for this step. The Frostbite and Nubis 2022/2023 slides were read in
full this time; Unreal and HDRP are from their documentation as remembered.

- **Frostbite** (Hillaire, Physically Based Sky, Atmosphere and Cloud
  Rendering, SIGGRAPH 2016 course). Measured extinction: stratus 0.04 to
  0.06 per metre, cumulus 0.05 to 0.12, albedo near 1. A 2D weather texture
  (R density, G type) and a type texture (x type, y height) give the
  vertical profile. Energy-conserving step:
  (S - S exp(-sigma_t d)) / sigma_t instead of S d. Phase: a mix of two
  Henyey-Greenstein lobes (forward and back). Multiple scattering by
  Wrenninge's octaves: scattering times a^n, extinction times b^n, phase
  eccentricity times c^n, with a <= b. Sun visibility from four samples
  towards the sun at growing distance. Ambient from a top and bottom sky
  colour. Cloud shadows: a 2D transmittance texture seen from the sun,
  projected on the world and used for opaque, transparent, particles and GI.
  0.91 ms at 720p, half resolution, 16 samples, with temporal reprojection.
- **Nubis 2017** (Schneider, Nubis: Authoring Real-Time Volumetric
  Cloudscapes): weather map with coverage, type (0 stratus, 0.5
  stratocumulus, 1 cumulus) and precipitation; a height gradient per type;
  density = a Perlin-Worley base remapped by coverage, then eroded by Worley
  detail at the edges; Beer's law, a silver-lining second lobe, "powder" dark
  edges. 54 to 96 steps with early out, about 2 ms on PS4.
- **Nubis Evolved 2022 / Nubis Cubed 2023**: step length grows with distance
  (3 m near plus 60 m per 16 km); light = direct (transmittance times
  phase) + multiple scattering (a second, wider phase) + ambient
  (pow(1 - profile, 0.5) times exp(-ambient density)). 2023 moves to authored
  voxel clouds with signed distance fields for empty-space skipping (2.2 to
  4 ms); voxels are authored per scene, so we keep the procedural map and use
  its idea in a cheap form: steps stay long until cloud is reached.
- **Unreal 5** (VolumetricCloud component): a layer between two altitudes on
  a round planet, a material for density, and cloud shadows from a "beer
  shadow map" (front depth, mean extinction, maximum optical depth) applied
  to the sun on surfaces and in the atmosphere.
- **Unity HDRP** (Volumetric Clouds): the cloud shadow is rendered into the
  sun's cookie, a texture projected along the sun with a size and offset in
  metres. Any surface lit by the sun gets it for free.
- **Godot addons** (code read): SunshineClouds2 (MIT) is a CompositorEffect
  that raymarches at reduced resolution with a temporal ping-pong, Beer and
  Henyey-Greenstein; no shadows on the scene and nothing the path tracer can
  see, since it draws over the finished frame.
- **Under path tracing.** Our tracer already gets the sky shader for camera
  rays that miss (the screen sky texture) and the radiance map for bounce rays,
  so clouds written in a sky shader appear in both. What it lacks is the
  sun's shadow from the clouds.

### What we build

1. **Directional light projector** (core). `DirectionalLight3D` gains a
   projector, as HDRP's cookie: `light_projector` (already on Light3D, until
   now hidden for directional lights), `projector_size` and
   `projector_offset` in metres in the light's plane. The texture is
   projected along the light; outside it the light is untouched. Raster
   (Forward+ and Mobile) multiplies the sun's colour by it, and the path
   tracer multiplies its sun sample by the same texture, so both match. This is
   a general feature: cloud shadows, canopy dapples, a stained-glass sun.
2. **Cloud layer in WeatherMap3D**: `cloud_base_height`,
   `cloud_thickness` (cumulonimbus tops grow from it), `cloud_density`.
   One density function, Nubis style, used in three places: the CPU
   (`get_cloud_density_at`, `get_sun_visibility_at`), the shadow bake and the
   sky shader. Cover and type come from the same drifting noise and storms
   the map already has, evaluated at any distance (the map only covers a few
   km; clouds reach the horizon). Shape noise is a tiling 3D Perlin-Worley
   (base) and Worley (detail) texture made once on the CPU.
3. **Cloud shadow**: `sun_light` names a DirectionalLight3D. The map marches
   the base shape along the sun ray through the layer for each texel of a
   square around the camera (Frostbite's transmittance texture), on the
   worker threads, about once a second. Between bakes the projector offset
   slides with the cloud drift, so shadows move smoothly. The result goes in
   that light's projector.
4. **`WeatherSkyMaterial`**: Godot's physical sky plus raymarched clouds,
   rendered at half resolution (fewer steps for the radiance map).
   Energy-conserving steps, two-lobe phase, three Wrenninge octaves,
   six-sample sun march, Nubis ambient. The map pushes the cloud state to it
   each frame.

Off by default: a light without a projector and a sky without this
material are upstream Godot.

Two choices made while checking:
- **Cover means share of sky.** The shape noise is equalised (each value
  equally common) and stretched twice vertically, so a column sees about one
  value of it; cover 0.4 then clouds about half the ground at the sun's
  zenith (shadows at their edges included) instead of nearly all of it.
- **Light under and inside clouds** is not exp(-tau): clouds barely absorb,
  so what they scatter mostly goes on. The shadow uses the two-stream
  diffuse transmission 1 / (1 + 3/4 (1 - g) tau), g = 0.85 (Bohren 1987,
  Multiple scattering of light and some of its observable consequences, Am.
  J. Phys. 55), which leaves about an eighth of the sun under a thick
  cumulus, as measured under broken cloud. In the sky the same term lights
  the cloud's shaded side, with the Wrenninge octaves and the sky's ambient.

### Checks (docs/bench/weather)

- Projector (`--view=projector`): an 8 x 8 checker with a red corner on a
  sun at 60 degrees lands on the same ground in raster and path traced
  frames, the same way round.
- Share of the ground below half sun, sun overhead, type 0.6: cover 0.2,
  0.4, 0.6, 0.8 and 1.0 give 16, 31, 50, 71 and 97% (mean sun 0.86, 0.71,
  0.57, 0.40, 0.24).
- Thickest column at cover 0.6: optical depth 65, peak extinction
  0.076 per metre (Frostbite cumulus 0.05 to 0.12).
- Baked shadow against a 10 m march at 200 points: mean difference 0.014.
- One bake, 256 x 256 texels, 12 steps, on the worker threads: 26 ms of
  the 4 cloud cores, in the background once a second.
- After a minute of drift, a point at a shadow's edge followed 861 m
  downwind reads the same sun (0.398 and 0.398).
- `--view=clouds` and `--view=clouds_low`: raster and path traced frames show
  the same clouds (with `pathtracing_sky_shader_enabled`) and the same cloud
  shadows on the fields; path tracing adds the box shadows past the raster
  shadow distance. GPU cost waits for the PC.

## Step 3b: clouds drawn a sixteenth at a time (10 Nov 2026)

**Technique**: temporal reprojection of the cloud pass, as Guerrilla ships it (Schneider, "The
Real-time Volumetric Cloudscapes of Horizon Zero Dawn", Advances 2015; "Nubis: Authoring
Real-Time Volumetric Cloudscapes", Advances 2017). Each frame the clouds are marched for one
pixel of every 4x4 block, in a Bayer order that visits all 16 in 16 frames. A resolve keeps
the other 15 from last frame, found by turning each pixel's view direction into last frame's
camera (the sky is at infinity, so the rotation is enough), and fills what last frame didn't
see from this frame's samples. Before it, every half resolution pixel was marched every frame.

As built:
- A sky shader render mode, `half_res_reprojection` (with `use_half_res_pass`): the half
  resolution pass is drawn at a quarter of each axis with the projection shifted to the frame's
  pixel, and `sky_reproject.glsl` (compute) resolves it into a full resolution image that
  `HALF_RES_COLOR` reads. Multiview (XR) keeps the plain half resolution pass.
- History through a Catmull-Rom filter from 9 bilinear taps (Jimenez, Filmic SMAA, 2016), as
  TAA does, so turning the camera doesn't blur the clouds a little more each frame.
- A drawn pixel is averaged with what it held (half and half). The march's jitter changes every
  frame (a golden-ratio step), so the stipple a fixed jitter left in the half resolution clouds
  settles into a smooth image.
- `WeatherSkyMaterial.cloud_reprojection`, on by default.

Results (lavapipe, `--view=clouds`, 960x540, 100% crops in the PR):
- Quality: full resolution clouds with their edges sharper, and no stipple. Turning the camera at
  20 degrees a second, they stay as sharp as without reprojection.
- Work: the march runs for 1/16 of the screen's pixels a frame instead of 1/4, so the march
  costs 4x less. It is 64 steps, each with 6 light steps and 3D noise reads, against a
  resolve of 9 reads per pixel.
- Lavapipe runs compute shaders on the CPU, where the full resolution resolve costs more than
  the march it saves (153 against 101 ms a frame). That tells us nothing about a GPU, so the
  GPU time is on the PC queue. If it costs more there, the default flips to off.
- Path traced, camera rays that miss show the same clouds as before (mean difference 1.5/255,
  sample noise).

## Sources

- Schneider, The Real-time Volumetric Cloudscapes of Horizon Zero Dawn,
  SIGGRAPH Advances 2015:
  https://advances.realtimerendering.com/s2015/
- Nubis, Evolved, SIGGRAPH Advances 2022 (abstract):
  https://advances.realtimerendering.com/s2022/
- Hillaire, A Scalable and Production Ready Sky and Atmosphere Rendering
  Technique, EGSR 2020: https://sebh.github.io/publications/egsr2020.pdf
- Patry, Real-Time Samurai Cinema (Ghost of Tsushima), SIGGRAPH Advances 2021:
  https://advances.realtimerendering.com/s2021/
- Naughty Dog, The Technical Art of The Last of Us Part II, SIGGRAPH Advances
  2020: https://advances.realtimerendering.com/s2020/
- Studio Gobo, Sparkly but not too Sparkly, SIGGRAPH Advances 2015:
  https://advances.realtimerendering.com/s2015/
- Ubisoft, Simulating Tropical Weather in Far Cry 6 (article), and Weick and
  Zhou, GDC 2022 (abstract):
  https://www.ubisoft.com/en-us/company/how-we-make-games/technology/articles/simulating-tropical-weather-in-far-cry-6
- Wronski, Assassin's Creed 4: Black Flag, Lighting, Weather and Atmospheric
  Effects, GDC 2014.
- Rockenbeck, Blowing from the West: Simulating Wind in Ghost of Tsushima, GDC
  2021: https://www.gamedeveloper.com/design/using-vorticles-to-simulate-wind-in-i-ghost-of-tsushima-i-
- Renard, Wind Simulation in God of War, GDC 2019 (abstract):
  https://gdcvault.com/play/1025972/Wind-Simulation-in-God-of
- IEC 61400-1 (2005), Annex B: Kaimal turbulence model.
- Oseguera and Bowles, A Simple Analytic 3-Dimensional Downburst Model Based on
  Boundary Layer Stagnation Flow, NASA TM-100632 (1988).
- Penman, Natural evaporation from open water, bare soil and grass, Proc. R.
  Soc. A 193 (1948).
- US Army CERC, Shore Protection Manual (1984), wave growth.
- TCA_Weather_System (MIT): https://github.com/kS222138/TCA_Weather_System
- Hillaire, Physically Based Sky, Atmosphere and Cloud Rendering in
  Frostbite, SIGGRAPH 2016 Physically Based Shading course.
- Schneider, Nubis: Authoring Real-Time Volumetric Cloudscapes with the
  Decima Engine, SIGGRAPH Advances 2017.
- Schneider, Nubis Evolved (2022) and Nubis Cubed (2023), SIGGRAPH Advances:
  https://advances.realtimerendering.com/
- Wrenninge, Kulla and Lundqvist, Oz: The Great and Volumetric, SIGGRAPH 2013
  talk (multiple scattering octaves).
- Unity HDRP, Volumetric Clouds (cloud shadows through the sun's cookie).
- SunshineClouds2 (MIT): https://github.com/Bonkahe/SunshineClouds2
- Cionco, A mathematical model for air flow in a vegetative canopy, J. Appl. Meteor. 4 (1965); Cionco, Analysis of canopy index values for various canopy densities, Boundary-Layer Meteorol. 15 (1978).
- Raupach, Simplified expressions for vegetation roughness length and zero-plane displacement, Boundary-Layer Meteorol. 71 (1994).
- Raupach, Finnigan and Brunet, Coherent eddies and turbulence in vegetation canopies: the mixing-layer analogy, Boundary-Layer Meteorol. 78 (1996).
- Finnigan, Turbulence in plant canopies, Annu. Rev. Fluid Mech. 32 (2000).
- Py, de Langre and Moulia, A frequency lock-in mechanism in the interaction between wind and crop canopies, J. Fluid Mech. 568 (2006).
- Hosker, Flow and diffusion near obstacles, in Atmospheric Science and Power Production (1984).
- Heisler and DeWalle, Effects of windbreak structure on wind flow, Agric. Ecosyst. Environ. 22/23 (1988).

## Snow and the water film (10 Nov)

- Falling snow no longer adds to the rain film. Only the rain share of the precipitation fills it. That share is `rain_fraction()`, the complement of the Jennings et al. (2018) snowfall fraction that `SnowCover3D` also uses, at the eased air temperature and humidity.
- Where a snowpack lies, the film is fed by what leaves the pack instead: rain through it plus its melt. `SnowCover3D` passes its runoff grid with `set_surface_water_source()` after each update. The map resamples it onto its own cells on the main thread before the parallel cell pass, so the workers never read the pack.
- The snow thread can read `get_air_temperature()` and `get_air_humidity()`, which give the eased state the cells use.
- It can also read `get_canopy_closure_at()` for interception. This takes the shelter canopy's Cionco coefficient as a leaf area index and passes it through Beer's law, with an extinction of 0.5 (Monsi and Saeki 1953).
