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
