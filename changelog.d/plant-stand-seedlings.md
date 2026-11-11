### Changed

- `PlantGenerator.make_floor()` seedlings are now young plants of the stand's own trees (beech, oak, sycamore, ash, maple), with their leaf forms; `litter_leaves` also applies to `PRESET_SEEDLING`.

### Fixed

- `PlantGenerator.litter_leaves` was not registered as a property, so scripts could not set it.
