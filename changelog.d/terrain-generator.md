### Added

- `TerrainGenerator3D`: generates mountain ranges for a whole world from tectonic uplift (painted map, Path3D ranges, noise) against stream power erosion (Cordonnier et al. 2016, Braun and Willett 2013 implicit solver), with sediment-filled basins, a critical slope, hillslope diffusion and multiple-flow-direction drainage. Deterministic per seed. Writes into Terrain3D (by name) or a `HeightMapShape3D`, re-applying a `TerrainDeformer3D`'s brushes on the new ground; exposes height, drainage area and sediment maps.
- `TerrainGenerator3D` detail: an erosion filter (Clay John 2018, Fewes 2023, Johansen 2026) carves branching gullies and spurs into the simulated ranges, computed per point so tiles join exactly; written in parallel (`detail_wavelength`, `detail_octaves`, `detail_depth`).

- `TerrainGenerator3D` geology and rivers: soil (Heimsath 1997 soil production), alluvial valley floors, bare rock and scree maps (`get_geology_at()`, `terrain_geology()` shader function via shader globals), and the river network with widths and discharges (`get_rivers()`, `build_river_paths()` makes `PhysXRiver3D` paths joined to each other).

### Fixed

- `TerrainGenerator3D` detail no longer notches crests into a regular comb: gullies fade to a share where opposite slopes meet (slope coherence) and a ridged noise gives crests irregular pinnacles and cols (`detail_crest`). New `sample_heights()` returns the final ground on a grid, filled in parallel.
