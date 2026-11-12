# Ground vegetation through the year

Grass, flowers, ferns and litter follow the one season state (`WeatherMap3D.get_season()`, keys
in its class reference), as trees do with `TreeGenerator.get_leaf_season()`. This note covers
everything under about 1.5 m (VegetationField3D, GrassField3D, PlantGenerator).

## What changes, and the published models for it

**Sward greenness.** Remote sensing tracks the green-up and browning of grassland with the
Growing Season Index (Jolly, Nemani and Running 2005): the product of three ramps, on the daily
minimum temperature (-2 to 5 C), day length (10 to 11 h) and vapour pressure deficit (4.1 to
0.9 kPa), smoothed over 21 days. It fits the start and end of the season across biomes within
days. Here the season state has the day's mean temperature, day length and soil moisture (a
Thornthwaite and Mather bucket), so the index is temperature x day length x moisture, with the
moisture ramp standing in for the deficit (Jolly et al. use the deficit as the drought term; in
grassland the soil water is the better one, Hufkens et al. 2016). The index sets how much of the
sward is green; the rest is last year's thatch (`dry_color`). A temperate grass in a mild winter
stays half green; a continental one goes tan; a summer drought browns the sward until rain.

**Flowering.** First flowering dates of herbs follow thermal time, the degree days above a base
since 1 January (Fitter and Fitter 2002: 385 British species, about 4 days earlier per 1 C), with
a window of a few weeks to months. The season state carries this sum (`forcing`, base 5 C). Each
flowering preset gets an onset and an end in degree days, from those records and the floras:
wood anemone and violet in early spring, wood sorrel and woodruff in spring, sorrel, foxglove,
herb Robert and wild carrot in early summer, knapweed, scabious and yarrow in high summer into
autumn. Before the onset the plant is in leaf only; after the end its heads are seed heads (the
tall flowers already build a seed form, `in_seed`); the share in seed rises through the window.

**Ferns and bracken.** Bracken croziers rise in May, the fronds are green through summer, turn
russet with the first frosts and stand dead and brown through winter, lodging under snow
(Watt 1976, bracken ecology). Male fern and the other woodland ferns follow the same course
later and partly stay green. Driven by the senescence sum (Delpierre et al. 2009) and frost.

**Litter.** Already seasonal: `stand_litter_leaves()` takes the fallen fraction and autumn colour
from the trees (`get_leaf_season()`), so fresh leaves fall into the older brown litter.

**Snow.** Snow cover belongs to the snow system (SnowCover3D); plants under deep snow are hidden
by it. No work here beyond hiding grass blades where the snow is deeper than they are tall.

## How games do it

Forza Horizon 4 swaps authored looks per season; Microsoft Flight Simulator changes ground and
tree colour with the date. Neither, as far as published, ties ground plants to a climate model.
The plan here keeps one state for everything and changes looks in two ways:

- **Colour by a uniform, every day.** Greenness and frond colour are shader parameters (no mesh
  rebuild): the grass material's dryness and the plant shader's season tint. Path tracing reads
  the same material, so it follows.
- **Shape by a rebuild, rarely.** Flowers opening, seed heads and leaf-only plants are mesh
  variants: species re-generate when their stage crosses a step (a quarter of the window), a few
  times a year per species, over several frames.

The far grass colour bake re-runs when the greenness moves by more than 0.05 (it is a few frames
of a 96 x 96 view), so the far meadow follows the near one.

## Plan

1. `season_source` (a WeatherMap3D) on GrassField3D and VegetationField3D; a daily read.
2. Sward greenness index (Jolly 2005 ramps with soil moisture) into GrassField3D's dryness and
   the far colour bake; bench with `--day=` across the year.
3. Flowering windows per preset in degree days; leaf-only, flowering and seed forms, re-generated
   when the stage steps.
4. Bracken and fern frond colour by the senescence sum and frost; standing dead fronds in
   winter.
5. Raster and path-traced shots through the year (spring, high summer, drought, autumn, winter).

## Built

**Step 1, sward greenness.** `GrassField3D.season_source` reads the season once a second. The
day-length ramp of the Growing Season Index is left out: it describes when growth starts, but a
temperate sward stays green through short days, and with it every British winter turned the
grass brown. What is left follows PhenoGrass (Hufkens et al. 2016), where grassland greenness is
driven by soil water and temperature: green = smoothstep(0.1, 0.4, soil moisture) x
smoothstep(-8, 2, daily mean C). The effective dryness is dryness + (1 - dryness)(1 - green) x
`season_response`; it goes to the blade shader and the ground's thatch at once (a uniform, no
rebuild), and the far colour is measured again when it moves by more than 0.05. Checks in the
grass bench: a mild wet winter 1.0, a -7 C winter 0.03, a summer drought 0.0, a dry spell at a
quarter of capacity 0.5.

**Step 2, flowering.** `PlantGenerator.season` takes the season state; each flowering preset has
a window in degree days (base 5 C, from 1 January): the British flowering months of the floras
(Stace 2019) turned into the degree days of a lowland year (about 110 by the end of March, 415 by
the end of May, 1055 by the end of July, 1665 by the end of September). Degree days rather than
dates, because first flowering follows thermal time (Fitter and Fitter 2002), so a warm year, a
warmer climate or a lower valley flowers earlier on its own. A population flowers as a spread of
individual heads (Rathcke and Lacey 1985): each flowering stem draws its place in the window from
a hash (not the generator's random stream, so a plant without a season is built as before). In a
flush the stems open in turn over the first 55% of the window and flower for 45% of it; daisy,
clover, yarrow and herb Robert keep opening heads, with a quarter in seed. After its flowers a
stem goes to seed: dandelion clocks (beaks with pappus parachutes on a sphere), thistledown, a
green ball of buttercup or anemone achenes, violet and wood sorrel capsules, herb Robert's beak,
foxglove capsules filling the spike from the bottom as the bells open higher up, the wild carrot's
nest. Tall flowers die back over about 400 degree days through the shader's dead-tissue term
(vertex alpha: yellowing to brown, less light through) and stand through winter, a share of them
into the next spring (60% in January, none by June). Short-lived seed heads go after their hold.
`get_bloom_shares(preset, season)` gives the shares in leaf, flower and seed, for tests and for
deciding when to generate a species again. Checks in the plants bench (`--test`): knapweed
before its window all leaves, in August 59% in flower and 41% in seed, in autumn all in seed, in
January 47% of last year's stalks; anemone in flower in April, done by July; daisy a quarter in
seed in summer; every flowering preset builds through the year; a knapweed rosette in May (5 cm)
and 75 cm in July. Not yet: spring plants (anemone) keep their leaves after flowering; their
dying back belongs with the bracken and ferns (step 3), as the field's seasonal presence.

## References

- Jolly, Nemani and Running, "A generalized, bioclimatic index to predict foliar phenology in response to climate", Global Change Biology 11, 2005.
- Hufkens et al., "Productivity of North American grasslands is increased under future climate scenarios despite rising aridity", Nature Climate Change 6, 2016.
- Fitter and Fitter, "Rapid changes in flowering time in British plants", Science 296, 2002.
- Stace, "New Flora of the British Isles", 4th edition, C&M Floristics, 2019.
- Rathcke and Lacey, "Phenological patterns of terrestrial plants", Annual Review of Ecology and Systematics 16, 1985.
- Grime, Hodgson and Hunt, "Comparative Plant Ecology", 2nd edition, Castlepoint Press, 2007.
- Delpierre et al., "Modelling interannual and spatial variability of leaf senescence for three deciduous tree species in France", Agricultural and Forest Meteorology 149, 2009.
- Watt, "The ecological status of bracken", Botanical Journal of the Linnean Society 73, 1976.
- Zhang et al., "Monitoring vegetation phenology using MODIS", Remote Sensing of Environment 84, 2003.
