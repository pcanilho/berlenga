### Added

- Vegetation: dune plants as real geometry: marram, lyme grass and sea holly in embryo, mobile and fixed dune sets by distance to the water (`PlantGenerator.make_dunes()`). Placed plants now shelter blowing sand and trap it when their field is in a ground surface's `wind_shelters` (`VegetationField3D.sample_lateral_cover()`, `PlantSpecies.wind_frontal_area`).
