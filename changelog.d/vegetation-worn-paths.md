### Added

- Vegetation: worn paths: give `VegetationField3D` and `GrassField3D` a list of `paths` (centre line, width, walkers a year) and plants thin out and grow shorter by how well their growth form resists trampling, leaving bare trodden soil in the middle and tough rosettes and grasses at the edges (`PlantSpecies.trample_resistance`, `GrassField3D.trample_resistance`).
