### Added

- `TerrainGenerator3D.get_detail_shader_code()`: the erosion detail filter's next octaves as a shader function (`terrain_detail_normal()`), its slope differentiated analytically, for gullies below the vertex spacing in the terrain material; octaves fade out by pixel footprint.
