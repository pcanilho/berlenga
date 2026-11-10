# Wet materials (vegetation gap 5)

Rain should leave every surface wet: bark and soil darken, leaves and stone gain a sheen.
`WeatherMap3D` already tracks wetness per cell (the water film from rain, drying by
evaporation, puddles) and publishes it in the global `weather_state_map` (green channel,
0..1) over `weather_map_rect`. No material reads it yet except a bench shader. This adds it
to `BaseMaterial3D`, in raster and under path tracing, so foliage, bark, ground and streets
all respond with one switch. Puddles stay in the existing puddle code.

## How others do it

- **Lagarde, "Water drop" 2a/2b/3a/3b (2013)**, from Remember Me: the reference for
  physically based wet surfaces. Measured wet versus dry materials show two cases:
  - porous surfaces (soil, bark, concrete, fabric) darken because water fills the pores and
    light scatters longer under the surface; the surface stays rough;
  - smooth or non-porous surfaces get a thin water film: highlights sharpen, colour barely
    changes.
  His shader (3b):
  `factor = lerp(1, 0.2, (1 - metalness) * porosity)`,
  `diffuse *= lerp(1, factor, wet)`,
  `gloss = lerp(1, gloss, lerp(1, factor, 0.5 * wet))`.
  Water's F0 (0.02) against a dielectric's 0.04 is "not visible enough" and is left alone.
  Porosity is authored, or guessed from roughness (`saturate(2.5 * roughness - 1.25)`),
  which he calls a rough heuristic. Accumulated water (puddles) is a separate blend towards
  water: smooth, flat normal, F0 0.02.
- **Nakamae et al. (1990)**, cited by Lagarde: wet diffuse is 0.1 to 0.3 of dry, specular 5
  to 10 times stronger.
- **Unreal Engine**: no built-in wetness. Projects drive a Material Parameter Collection
  (global scalar) into a material function that darkens base colour and lowers roughness,
  usually Lagarde's numbers (Fortnite, the City Sample). The Water plugin's wetness is
  shoreline-only.
- **Unity HDRP**: none built in; the HDRP "Rain" samples do the same as Unreal through a
  global shader property.
- **CryEngine**: a global rain wetness term in the deferred pass (with a top-down rain
  occlusion map), applied to every opaque surface; darkens and sharpens the G-buffer.
- **Frostbite / Assassin's Creed IV**: wetness in the G-buffer pass from a global rain amount
  times a top-down occlusion map, Lagarde-style darkening.

So the common shape is: one global wetness source, per-material porosity, Lagarde's model.
Rain occlusion (dry under roofs and trees) is a separate top-down map, listed for the weather
work; not part of this step.

## What we build

`BaseMaterial3D` Wetness group (`wetness_enabled`, off by default, so no existing material
changes or pays anything):

- `wetness_porosity` (0..1, default 0.5): how much the surface darkens when wet.
- `wetness_amount` (0..1, default 1): scales the weather's wetness.
- `wetness_from_weather` (default on): read `weather_state_map`. Off: `wetness_amount` is the
  wetness, set by a script (a dunked prop, a sprinkler).

Model, Lagarde 3b in roughness terms (gloss = 1 - roughness):

```
factor    = mix(1.0, 0.2, (1.0 - metallic) * porosity);
albedo   *= mix(1.0, factor, wet);
roughness *= mix(1.0, factor, 0.5 * wet);
```

Surfaces that face down (the underside of a leaf or a ledge) get less rain:
`wet *= smoothstep(-0.6, 0.2, world_normal.y)`. Lagarde's thin-film case on non-porous
surfaces (no colour change, smoother) falls out of the same lines with porosity near 0, only
weakly; that is his choice too, since the film on smooth surfaces is the puddle blend.

- **Raster**: the generated shader reads the global `weather_state_map` and
  `weather_map_rect`. Godot needs a global uniform to exist before a shader using it
  compiles, so the material registers both with no data (rect width 0 = no weather, so
  nothing is wet) when the first wet material is built; `WeatherMap3D` then sets them.
- **Path tracing**: BaseMaterial3D reaches the path tracer as packed `RT_MaterialData`, not
  as its shader. Porosity, amount and the two switches go in the spare bits of the
  subsurface word; the weather texture (bindless) and rect go in `rt_params`; the
  closest-hit material evaluation applies the same three lines. Same input, same formula,
  so raster and path traced frames match.

Foliage gets the same feature (low porosity on waxy leaves, high on bark); drips and leaf
sheen are later, on top.

## Checks

Bench `docs/bench/wetness`: a row of spheres and a ground plane with porosities 0 to 1 under
a `WeatherMap3D` raining, raster and path traced at 100% crop; dry (feature off) must equal
before; wet must match Lagarde's numbers on a flat albedo (0.2 times the albedo at porosity 1
and wetness 1).

## Results

Bench `docs/bench/wetness` (porosity 0 to 1 on five spheres, ground porosity 0.9), mean
pixel difference 0 to 255 at 960 x 540:

- feature on with a dry weather map against the feature off: 0.0 in raster, 0.00003 path
  traced (sampling noise). Nothing changes until it rains, and nothing changes for any
  material without the feature.
- wet against dry: 24.2 raster, 22.0 path traced; the porous ground and the right-hand
  spheres darken, the non-porous left sphere only gains a sharper highlight.
- raster against path traced: 5.6 dry and 3.3 wet (the usual lighting difference between
  the two, not wetness); at 100% crop the wet spheres and ground match.
- `--mode=half` (wet where x < 0): only the left half darkens, in both.
- `--weather`: a real `WeatherMap3D` raining (time sped up) gives exactly the hand-set fully
  wet frame (difference 0.0), so its globals reach the materials.

Cost: none for materials without the feature; with it, one texture read and a few
multiplies per shaded pixel and per path tracer hit.
