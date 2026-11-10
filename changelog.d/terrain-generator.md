### Added

- `TerrainGenerator3D`: generates mountain ranges for a whole world from tectonic uplift (painted map, Path3D ranges, noise) against stream power erosion (Cordonnier et al. 2016, Braun and Willett 2013 implicit solver), with sediment-filled basins, a critical slope, hillslope diffusion and multiple-flow-direction drainage. Deterministic per seed. Writes into Terrain3D (by name) or a `HeightMapShape3D`, re-applying a `TerrainDeformer3D`'s brushes on the new ground; exposes height, drainage area and sediment maps.
