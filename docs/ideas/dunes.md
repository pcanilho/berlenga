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

## Built: coastal dune belt (16 Nov, option 3, coastal first)

- `modules/terrain_deform/terrain_dunes.h`: Werner's slabs moved as expected amounts (Nilles 2024), in a fixed order
  so a seed gives the same dunes everywhere. Each step: the dry beach adds sand; every sandy cell out of the wind
  shadow (15 degrees behind a crest) lifts a slab, less under marram; the slab hops downwind and settles a share at
  each landing (more on sand, all in the shadow, more among plants), split bilinearly; slopes past the repose (34
  degrees, 45 under full cover) avalanche over eight neighbours. The wind speeds up over ground standing above its
  surroundings (Jackson and Hunt 1975, as Paris 2019 warp it), lifting more and hopping further, which caps the
  crests. Marram follows Maun's burial response on the deposition averaged over about 30 steps: best under gentle
  burial, dying under heavy burial or erosion (DECAL's rule).
- TerrainGenerator3D `dunes_enabled` (on): the belt is land within `dune_belt_width` of a beach face, on its own
  `dune_cell` grid; supply from the dry beach (`dune_supply` m3 per metre of beach over the run); the wind blows up
  the beaches on average, turned by `dune_wind_angle`; the sea, lakes and river channels swallow sand; marram grows
  behind the beach face in patches (`dune_vegetation`). The sand adds to the ground in `_ground` and to the
  geology's beach sand. `get_dune_height`, `get_dune_stability` (0 bare, 1 fixed) for plants.
- Tiles of 512 m with 128 m aprons run in parallel (WorkerThreadPool), each with the wind up its own beaches and on
  the box of its belt only, blended across the aprons; the result does not depend on the thread count.
- Bench: `--dunes[=steps]`, `--dune-supply`, `--dune-belt`, `--dune-cell`, `--dune-wind`, `--dune-veg`,
  `--dune-map=path.png[,x0,z0,size]` (top view: bare sand tan, marram green).
- Cost: about 0.13 us per cell and step on one core. The hanging-valley coast (4 km, 1500 steps at 2 m) takes 53 s
  on the 4 cloud cores; expect a few seconds on a 16-core desktop. Tuning: full marram stops 90% of the lift; less
  lets most of the sand run off inland as a film, more grows spires.
- Next: desert dune fields (bare sand seas, wind regimes: barchan, transverse, linear, star; nebkha round shrubs).

## Built: desert sand seas (16 Nov, option 3, second half)

- First tried the coastal model's deterministic slabs (Nilles 2024) on bare sand: they left stripes along the grid
  rows and flat plateaus with pits, because every cell moves at once in a fixed order. The stochastic original
  (Werner 1995 as Paris, Galin, Peytavie, Guérin and Gain 2019 run it) does not: `TerrainDuneSea` in
  `terrain_dunes.h` ports Paris's MIT code. A random cell lifts a 10 cm slab unless the shadow behind a crest
  (5 to 15 degrees, within 10 cells) holds it; the slab hops downwind, up to twice as fast up slopes facing the wind,
  and settles at each landing with a chance (60% on sand, 40% on bare ground, certain in the shadow), up to three
  bounces; sand past 33 degrees slides off at the lift and the landing. The grid wraps.
- Two fixes over the original: the shadow marches straight up the wind with bilinear heights instead of along the
  wind's octant, and each slab starts at a random point in its cell. Without them, winds off the grid axes built
  ridges snapped to 45 degrees.
- Lengths and heights are both in cells, so the dunes keep their angles at any `sand_sea_scale` and grow with it:
  crests come some 25 cells apart (100 m at the default 4 m).
- Kinds by wind regime (`sand_sea_kind`): one wind over thick sand gives transverse ridges; one wind over sand gathered
  in low cones gives barchans; two winds 120 degrees apart in turn give linear dunes along their mean; three winds in
  long turns give star-like peaks. All four checked on a 256-cell harness; sand is conserved to the cell.
- Parallel: each step cuts the grid into an even number of row bands at a random offset and runs the even bands side
  by side, then the odd ones (the sublattice decomposition of parallel kinetic Monte Carlo). Bands are at least
  twice the farthest an event reaches (hops, shadow, slides), and each band draws from a hash of seed, step and band,
  so the result is identical on one thread or many. 512 x 512 cells, 300 steps: 55 s on one core, 19 s on four.
- Generator: `sand_seas_enabled` (on; nothing happens until `sand_sea_rect` is set), `sand_sea_depth`,
  `sand_sea_wind_angle`, `sand_sea_scale`, `sand_sea_steps`, `sand_sea_edge`. The sand lies on the generated ground
  (the model's base), filling hollows and sliding off slopes past its repose, so hills stand out of the erg; it
  thins out at the rect's edge (where the base eases to its mean, so the wrapped grid has no step) and on water,
  and adds to the ground and to the geology's sand.
  `get_sand_sea_height()` for placing things.
- Bench: landscape `--sand-sea=x0,z0,w,d` with `--sand-sea-kind`, `--sand-sea-depth`, `--sand-sea-wind`,
  `--sand-sea-scale`, `--sand-sea-steps`; `--dune-map` draws it. terrain_gen `--test` checks it.
- Not yet: nebkha (sand gathered round shrubs), sand seas following a mask instead of a rect, wind from the climate.
