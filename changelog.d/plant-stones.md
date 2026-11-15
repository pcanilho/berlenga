### Added

- Plants between stones: PlantGenerator presets `PRESET_PARSLEY_FERN`, `PRESET_SPLEENWORT`, `PRESET_STONECROP`, `PRESET_SAXIFRAGE` and `PRESET_THRIFT`, `PlantSpecies.stone_gap` (grow only in the gaps between obstacles), and `PlantGenerator.make_stones()` for scree, river gravel and shingle.
- Moss on stone tops: `PlantSpecies.stone_top`, `VegetationField3D.stone_tops` and `stone_top_normals`; the scree and river sets carry top moss.

### Fixed

- Plants on and beside fallen logs follow the log's reach profile from the trees module instead of its widest reach.
