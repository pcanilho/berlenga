# Dunes in the terrain generator (research, 16 Nov)

The sand work (GroundSurface3D: prints, grains, wind ripples, streamers, wet sand, sheltering) covers the metre
scale and rides on whatever ground it is given. Dunes are the scale above it, from a 5 m foredune to a 300 m star
dune, so they belong to TerrainGenerator3D. Paulo picked "hand dunes to the terrain thread" on the sand card.

## What the established models do

- **Werner 1995 slab model** (Geology 23): sand as slabs on a grid. A slab is lifted and hops a fixed distance
  downwind; it sticks with a higher chance on sand than on bare ground and always in the wind shadow behind a crest
  (about 15 degrees below the horizon). Slopes over the angle of repose (about 34 degrees) avalanche. With only the
  wind regime and the sand supply as inputs it grows barchans, transverse, linear and star dunes (Wasson and Hyde
  1983's supply-by-wind-variability diagram), which is why graphics papers build on it.
- **Paris, Galin, Peytavie, Guérin and Gain 2019, "Desertscape Simulation"** (CGF 38(7)): a layered heightfield
  (bedrock, sand, vegetation) with stochastic saltation events, bounce, reptation and avalanching; the wind is warped
  by the terrain (Venturi speed-up over crests, deflection round obstacles); vegetation raises the repose and traps
  sand, which adds nebkha and parabolic dunes and yardangs on soft rock. The reference for terrain-scale dunes.
- **Taylor and Keyser 2023** (PACMCGIT, real-time sand dune simulation): a GPU Werner-type model, with echo dunes
  in front of cliffs.
- **Nilles, Günther and Müller, GRAPP 2024, "Real-Time Desertscapes Simulation with CUDA"**: Paris 2019 made
  deterministic. Each step moves the expected sand (lift, bounce, advect, deposit, reptation) instead of random
  events, on float heights with bilinear advection and wind shadow (tan 10 to 15 degrees), and a parallel
  eight-neighbour avalanche (repose 33 degrees bare, up to 45 under full vegetation, 68 for bedrock). Bidirectional
  winds with a strength ratio give the dune types. About 1 ms a step at 1024 squared on a desktop GPU; 1000 steps of
  512 squared take half a second. They found Paris's reptation only acted above repose in the reference code.
- **Nilles et al., GRAPP 2025, "Oasis"**: the same with hydraulic erosion and dynamic, competing vegetation on a
  layered map (bedrock, soil, sand, water, humus), and a divergence-free wind by pressure projection.
- **Vegetated coasts**: Baas 2002 and Nield and Baas 2008 (DECAL, a cellular automaton on Werner's slabs) grow
  foredunes, blowouts and parabolic dunes from plants that tolerate burial; Durán and Herrmann 2006 and the Coastal
  Dune Model (Moore et al., Esurf 5, 417, 2017) do it as a continuum with plant growth against sand burial. Foredunes
  form where the beach supplies sand and marram grass traps it; blowouts and parabolic dunes where the cover breaks.
- **Tools**: World Machine and Gaea shape dunes with pattern and erosion nodes rather than a sand model; Houdini
  heightfield setups and asset-store "procedural desert" packs layer dune noises. None gives the dune types from the
  wind, which the slab models do for free.

## How it fits our generator

- **Scale.** The generator's cells are 16 m. Draa and big star or linear dunes (crest spacing 0.5 to 2 km, 50 to
  300 m high) can live on that grid. Barchans (50 to 300 m across), transverse dunes and coastal foredunes (5 to 20 m
  high, 20 to 100 m wide) need 1 to 4 m cells: a dune pass on its own finer grid over the sand areas only, written
  into the ground like the touch-ups, so the 16 m solve does not change.
- **Model.** Deterministic expected-value transport (Nilles 2024) over the random slabs: no RNG, so the same seed gives
  the same dunes everywhere, which our generator promises; it converges in fewer steps. CPU C++ in a fixed order like
  the rest of the generator (the GPU numbers above suggest 512 squared for a few thousand steps runs in seconds to a
  minute on CPU; to be measured).
- **Inputs we already have.** Sand supply: the beach band and Coast/Beach guides, river and outwash sand
  (braidplains), the geology map's loose cover. Wind regime: WeatherMap3D (get_wind_at at 10 m) or a direction and
  spread on the generator. Vegetation: the vegetation module's cover and a marram-type PlantSpecies raise the repose
  and trap sand. Thresholds: the sand work's GroundRipples friction_speed and saltation_flux (Bagnold threshold,
  Lettau) and Raupach's drag partition (shelter_ratio_for) for plant sheltering.
- **Outputs.** Dune heights into the ground; a sand layer in the geology so TerrainMaterial3D draws sand and
  GroundSurface3D's ripples and streamers ride the dunes; slip faces and crest lines for the material (darker lee
  sand, avalanche grain flows); a stability map (bare, partly fixed, fixed) for vegetation.
- **Showcase.** The hanging-valley layout has a beach and a coastal plain: a foredune belt behind the beach, with
  blowouts and a few parabolic dunes where the cover breaks, is the natural place. Full desert fields suit the open
  world (mortal-plane) more than the showcase.

## Options for Paulo

1. Coastal dune belt behind beaches (foredunes, blowouts, parabolic dunes with marram), on a 2 m grid along the coast.
2. Desert dune fields (barchan, transverse, linear, star by wind regime and supply), on their own grid over a
   sand-sea region, with nebkha round shrubs.
3. Both, coastal first (one model, two settings: the same transport with vegetation on or off).
