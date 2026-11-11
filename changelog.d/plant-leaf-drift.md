### Added

- `PlantSpecies.litter_range` (`RULE_LITTER`) and `VegetationField3D.get_litter_at()`: fallen leaves drift past the crowns of broad-leaved trees into forest edges and clearings, thinning over about 6 m; `PlantGenerator.make_floor()` places leaf litter and twigs by it.
- `VegetationField3D.litter_wind`: the leaf drift reaches farther downwind and less upwind, from the weather map's wind by default.
