### Added

- `TerrainMaterial3D`: a ground material that writes its own shader: rock, scree, soil, alluvium and sand layers chosen by the terrain generator's geology (or by slope without it), height-blended, biplanar on cliffs, with the erosion detail normal, grass ground tint, wetness and snow.
- `BaseMaterial3D.get_snow_shader_code()` and `get_wetness_shader_code()`: the engine's snow and wetness lines for any `ShaderMaterial`, the same text `BaseMaterial3D` uses.
