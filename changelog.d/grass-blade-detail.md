### Changed

- `GrassField3D`: blade colour varies over the field and per blade (`color_variation`), a share of dry blades are dead from the root, blades carry a midrib and veins and taper near the tip.
- `GrassField3D.get_ground_shader_code()`: the ground under grass becomes turf matched to the blades (same colour patches, streaks, thatch), so gaps between blades show grass, not soil.
- `GrassField3D.canopy_field`: the sward follows the canopy shade of a `VegetationField3D`.
